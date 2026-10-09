/*
 * Network backend: bus connection, Wi-Fi device discovery, live change
 * notifications, and the simple public entry points (new, free, rescan,
 * set_wifi_enabled, disconnect). See network-private.h for how the
 * backend is split across files.
 */

#include "network-private.h"

#include <string.h>

/* --- live change notifications ---------------------------------------- */

#define AP_REFRESH_DEBOUNCE_MS 200

static gboolean
on_ap_refresh_timeout(gpointer user_data)
{
    ExtrasMenuNetwork *network = user_data;

    network->ap_refresh_source_id = 0; /* returning G_SOURCE_REMOVE removes it */
    network_request_ap_list(network);
    return G_SOURCE_REMOVE;
}

static void
on_ap_list_relevant_signal(GDBusConnection *connection, const gchar *sender_name,
                            const gchar *object_path, const gchar *interface_name,
                            const gchar *signal_name, GVariant *parameters,
                            gpointer user_data)
{
    ExtrasMenuNetwork *network = user_data;
    (void) connection;
    (void) sender_name;
    (void) object_path;
    (void) interface_name;
    (void) signal_name;
    (void) parameters;

    /* Any of: device PropertiesChanged (covers ActiveAccessPoint
     * changing), AccessPointAdded, AccessPointRemoved -- all mean the
     * list we'd show the user is now stale. Re-fetch everything rather
     * than trying to patch the list incrementally.
     *
     * A scan produces a burst of these signals, so the refresh is
     * scheduled a moment out and further signals arriving before it
     * fires are folded into it, instead of re-fetching (and making the
     * UI rebuild the list) once per signal. */
    if (network->ap_refresh_source_id == 0)
    {
        network->ap_refresh_source_id = g_timeout_add(AP_REFRESH_DEBOUNCE_MS,
                                                       on_ap_refresh_timeout, network);
    }
}

/* --- finding the first Wi-Fi device, one device at a time --------------- */

typedef struct
{
    ExtrasMenuNetwork *network;
    gchar **paths; /* NULL-terminated */
    guint index;
} DeviceWalkCtx;

static void device_walk_check_next(DeviceWalkCtx *ctx);

static void
on_device_type_finished(GObject *source, GAsyncResult *result, gpointer user_data)
{
    DeviceWalkCtx *ctx = user_data;
    GError *error = NULL;

    GVariant *reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), result, &error);
    if (reply == NULL && call_was_cancelled(error))
    {
        g_clear_error(&error);
        g_strfreev(ctx->paths);
        g_free(ctx);
        return;
    }

    guint32 device_type = 0;
    if (reply != NULL)
    {
        GVariant *boxed = NULL;
        g_variant_get(reply, "(v)", &boxed);
        device_type = g_variant_get_uint32(boxed);
        g_variant_unref(boxed);
        g_variant_unref(reply);
    }
    g_clear_error(&error);

    if (device_type == NM_DEVICE_TYPE_WIFI)
    {
        ctx->network->wifi_device_path = g_strdup(ctx->paths[ctx->index]);

        /* Subscribe to everything that can make the AP list stale,
         * then fetch it for the first time. */
        ctx->network->wifi_device_props_subscription_id = g_dbus_connection_signal_subscribe(
            ctx->network->system_bus, NM_BUS_NAME,
            "org.freedesktop.DBus.Properties", "PropertiesChanged",
            ctx->network->wifi_device_path, NULL, G_DBUS_SIGNAL_FLAGS_NONE,
            on_ap_list_relevant_signal, ctx->network, NULL);

        ctx->network->ap_added_subscription_id = g_dbus_connection_signal_subscribe(
            ctx->network->system_bus, NM_BUS_NAME,
            NM_WIRELESS_IFACE, "AccessPointAdded",
            ctx->network->wifi_device_path, NULL, G_DBUS_SIGNAL_FLAGS_NONE,
            on_ap_list_relevant_signal, ctx->network, NULL);

        ctx->network->ap_removed_subscription_id = g_dbus_connection_signal_subscribe(
            ctx->network->system_bus, NM_BUS_NAME,
            NM_WIRELESS_IFACE, "AccessPointRemoved",
            ctx->network->wifi_device_path, NULL, G_DBUS_SIGNAL_FLAGS_NONE,
            on_ap_list_relevant_signal, ctx->network, NULL);

        network_request_ap_list(ctx->network);

        g_strfreev(ctx->paths);
        g_free(ctx);
        return;
    }

    ctx->index++;
    device_walk_check_next(ctx);
}

static void
device_walk_check_next(DeviceWalkCtx *ctx)
{
    if (ctx->paths[ctx->index] == NULL)
    {
        /* Exhausted the device list without finding a Wi-Fi device. */
        if (ctx->network->list_changed_callback != NULL)
            ctx->network->list_changed_callback(FALSE, NULL, 0, ctx->network->list_changed_user_data);

        g_strfreev(ctx->paths);
        g_free(ctx);
        return;
    }

    g_dbus_connection_call(
        ctx->network->system_bus,
        NM_BUS_NAME,
        ctx->paths[ctx->index],
        "org.freedesktop.DBus.Properties",
        "Get",
        g_variant_new("(ss)", NM_DEVICE_IFACE, "DeviceType"),
        G_VARIANT_TYPE("(v)"),
        G_DBUS_CALL_FLAGS_NONE,
        -1, ctx->network->cancellable,
        on_device_type_finished, ctx);
}

static void
on_get_devices_finished(GObject *source, GAsyncResult *result, gpointer user_data)
{
    ExtrasMenuNetwork *network = user_data;
    GError *error = NULL;

    GVariant *reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), result, &error);
    if (reply == NULL && call_was_cancelled(error))
    {
        g_clear_error(&error);
        return;
    }

    if (reply == NULL)
    {
        g_clear_error(&error);
        if (network->list_changed_callback != NULL)
            network->list_changed_callback(FALSE, NULL, 0, network->list_changed_user_data);
        return;
    }

    GVariant *device_paths = NULL;
    g_variant_get(reply, "(@ao)", &device_paths);

    guint n = (guint) g_variant_n_children(device_paths);
    gchar **paths = g_new0(gchar *, n + 1); /* NULL-terminated */
    for (guint i = 0; i < n; i++)
    {
        GVariant *child = g_variant_get_child_value(device_paths, i);
        paths[i] = g_variant_dup_string(child, NULL);
        g_variant_unref(child);
    }

    g_variant_unref(device_paths);
    g_variant_unref(reply);

    if (n == 0)
    {
        g_free(paths);
        if (network->list_changed_callback != NULL)
            network->list_changed_callback(FALSE, NULL, 0, network->list_changed_user_data);
        return;
    }

    DeviceWalkCtx *ctx = g_new0(DeviceWalkCtx, 1);
    ctx->network = network;
    ctx->paths = paths;
    ctx->index = 0;

    device_walk_check_next(ctx);
}

static void
find_wifi_device(ExtrasMenuNetwork *network)
{
    g_dbus_connection_call(
        network->system_bus,
        NM_BUS_NAME,
        NM_OBJ_PATH,
        NM_IFACE,
        "GetDevices",
        NULL,
        G_VARIANT_TYPE("(ao)"),
        G_DBUS_CALL_FLAGS_NONE,
        -1, network->cancellable,
        on_get_devices_finished, network);
}

/* --- bus connection lifecycle --------------------------------------------- */

static void
on_bus_get_finished(GObject *source, GAsyncResult *result, gpointer user_data)
{
    ExtrasMenuNetwork *network = user_data;
    (void) source;

    GError *error = NULL;
    GDBusConnection *bus = g_bus_get_finish(result, &error);

    /* Cancelled means extras_menu_network_free() already ran and
     * `network` no longer exists, so it can't be written to here. */
    if (bus == NULL && call_was_cancelled(error))
    {
        g_clear_error(&error);
        return;
    }

    network->system_bus = bus;

    if (network->system_bus == NULL)
    {
        g_clear_error(&error);
        if (network->list_changed_callback != NULL)
            network->list_changed_callback(FALSE, NULL, 0, network->list_changed_user_data);
        if (network->status_changed_callback != NULL)
            network->status_changed_callback(EXTRAS_MENU_NETWORK_KIND_NONE, NULL,
                                              network->status_changed_user_data);
        return;
    }

    find_wifi_device(network);
    network_start_primary_connection_tracking(network);
}

/* --- public API ------------------------------------------------------------ */

ExtrasMenuNetwork *
extras_menu_network_new(ExtrasMenuNetworkListChangedFunc list_changed_callback,
                         ExtrasMenuNetworkStatusChangedFunc status_changed_callback,
                         ExtrasMenuNetworkWifiEnabledChangedFunc wifi_enabled_changed_callback,
                         gpointer user_data)
{
    ExtrasMenuNetwork *network = g_new0(ExtrasMenuNetwork, 1);
    network->list_changed_callback = list_changed_callback;
    network->list_changed_user_data = user_data;
    network->status_changed_callback = status_changed_callback;
    network->status_changed_user_data = user_data;
    network->wifi_enabled_changed_callback = wifi_enabled_changed_callback;
    network->wifi_enabled_changed_user_data = user_data;
    network->wifi_device_path = NULL;
    network->watched_ip4config_path = NULL;
    network->cancellable = g_cancellable_new();
    network->security_by_ssid = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);

    g_bus_get(G_BUS_TYPE_SYSTEM, network->cancellable, on_bus_get_finished, network);

    return network;
}

void
extras_menu_network_set_wifi_enabled(ExtrasMenuNetwork *network, gboolean enabled)
{
    if (network == NULL || network->system_bus == NULL)
        return;

    g_dbus_connection_call(
        network->system_bus,
        NM_BUS_NAME,
        NM_OBJ_PATH,
        "org.freedesktop.DBus.Properties",
        "Set",
        g_variant_new("(ssv)", NM_IFACE, "WirelessEnabled", g_variant_new_boolean(enabled)),
        NULL,
        G_DBUS_CALL_FLAGS_NONE,
        -1, NULL,
        NULL, NULL); /* fire-and-forget: PropertiesChanged brings the
                       * confirmed state back to us either way */
}

void
extras_menu_network_rescan(ExtrasMenuNetwork *network)
{
    if (network == NULL || network->system_bus == NULL || network->wifi_device_path == NULL)
        return;

    g_dbus_connection_call(
        network->system_bus,
        NM_BUS_NAME,
        network->wifi_device_path,
        NM_WIRELESS_IFACE,
        "RequestScan",
        g_variant_new("(a{sv})", NULL),
        NULL,
        G_DBUS_CALL_FLAGS_NONE,
        -1, NULL,
        NULL, NULL); /* fire-and-forget; results arrive via the usual
                       * AccessPointAdded/PropertiesChanged signals */
}

void
extras_menu_network_disconnect(ExtrasMenuNetwork *network)
{
    if (network == NULL || network->system_bus == NULL || network->wifi_device_path == NULL)
        return;

    g_dbus_connection_call(
        network->system_bus,
        NM_BUS_NAME,
        network->wifi_device_path,
        NM_DEVICE_IFACE,
        "Disconnect",
        NULL,
        NULL,
        G_DBUS_CALL_FLAGS_NONE,
        -1, NULL,
        NULL, NULL);
}

void
extras_menu_network_free(ExtrasMenuNetwork *network)
{
    if (network == NULL)
        return;

    /* Cancel everything still in flight first. Cancelled calls still
     * run their callbacks later (with G_IO_ERROR_CANCELLED), which is
     * why each of them checks for that before touching `network` --
     * see call_was_cancelled(). */
    g_cancellable_cancel(network->cancellable);

    if (network->ap_refresh_source_id != 0)
        g_source_remove(network->ap_refresh_source_id);

    /* Connection attempts still being watched: stop their signal
     * subscriptions and timeouts, and drop them without reporting
     * (the caller is shutting down, nobody is waiting for the result).
     * A watch may still be referenced by its pending State read, which
     * releases it when its cancelled callback runs. */
    for (GSList *l = network->watches; l != NULL; l = l->next)
    {
        ActiveConnectionWatchCtx *watch = l->data;
        watch->finished = TRUE;
        if (network->system_bus != NULL)
            network_stop_watch_sources(watch);
        else if (watch->timeout_source_id != 0)
            g_source_remove(watch->timeout_source_id);
        watch->timeout_source_id = 0;
        watch->subscription_id = 0;
        watch->callback = NULL;
        network_watch_unref(watch);
    }
    g_slist_free(network->watches);
    network->watches = NULL;

    if (network->system_bus != NULL)
    {
        if (network->wifi_device_props_subscription_id != 0)
            g_dbus_connection_signal_unsubscribe(network->system_bus, network->wifi_device_props_subscription_id);
        if (network->ap_added_subscription_id != 0)
            g_dbus_connection_signal_unsubscribe(network->system_bus, network->ap_added_subscription_id);
        if (network->ap_removed_subscription_id != 0)
            g_dbus_connection_signal_unsubscribe(network->system_bus, network->ap_removed_subscription_id);
        if (network->nm_props_subscription_id != 0)
            g_dbus_connection_signal_unsubscribe(network->system_bus, network->nm_props_subscription_id);
        if (network->ip4config_props_subscription_id != 0)
            g_dbus_connection_signal_unsubscribe(network->system_bus, network->ip4config_props_subscription_id);
        g_object_unref(network->system_bus);
    }

    g_free(network->wifi_device_path);
    g_free(network->watched_ip4config_path);
    g_clear_pointer(&network->security_by_ssid, g_hash_table_destroy);
    g_clear_object(&network->cancellable);
    g_free(network);
}