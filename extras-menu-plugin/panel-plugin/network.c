#include "network.h"

#include <gio/gio.h>
#include <string.h>

#define NM_BUS_NAME              "org.freedesktop.NetworkManager"
#define NM_OBJ_PATH               "/org/freedesktop/NetworkManager"
#define NM_IFACE                 "org.freedesktop.NetworkManager"
#define NM_DEVICE_IFACE          "org.freedesktop.NetworkManager.Device"
#define NM_WIRELESS_IFACE        "org.freedesktop.NetworkManager.Device.Wireless"
#define NM_ACCESS_POINT_IFACE    "org.freedesktop.NetworkManager.AccessPoint"
#define NM_IP4CONFIG_IFACE       "org.freedesktop.NetworkManager.IP4Config"
#define NM_CONNECTION_ACTIVE_IFACE "org.freedesktop.NetworkManager.Connection.Active"
#define NM_SETTINGS_OBJ_PATH     "/org/freedesktop/NetworkManager/Settings"
#define NM_SETTINGS_IFACE        "org.freedesktop.NetworkManager.Settings"
#define NM_SETTINGS_CONNECTION_IFACE "org.freedesktop.NetworkManager.Settings.Connection"

#define NM_DEVICE_TYPE_ETHERNET 1
#define NM_DEVICE_TYPE_WIFI     2

struct _ExtrasMenuNetwork
{
    GDBusConnection *system_bus;

    /* Object path of the first Wi-Fi device found, e.g.
     * "/org/freedesktop/NetworkManager/Devices/0". NULL until (and
     * unless) one is found. */
    gchar *wifi_device_path;

    guint wifi_device_props_subscription_id;
    guint ap_added_subscription_id;
    guint ap_removed_subscription_id;

    /* Subscribed once, on the top-level NetworkManager object, to
     * learn whenever the "primary" active connection (what NM itself
     * considers the main one, e.g. preferring Ethernet over Wi-Fi when
     * both are up) changes -- this is what drives the Wi-Fi/Ethernet
     * pill's label and behavior. */
    guint nm_props_subscription_id;

    /* Object path of whatever IP4Config we're currently watching for
     * changes (belongs to the active Ethernet connection), so we can
     * unsubscribe/resubscribe correctly as the active connection
     * changes over time. NULL when not tracking an Ethernet IP. */
    gchar *watched_ip4config_path;
    guint ip4config_props_subscription_id;

    ExtrasMenuNetworkListChangedFunc list_changed_callback;
    gpointer list_changed_user_data;

    ExtrasMenuNetworkStatusChangedFunc status_changed_callback;
    gpointer status_changed_user_data;

    ExtrasMenuNetworkWifiEnabledChangedFunc wifi_enabled_changed_callback;
    gpointer wifi_enabled_changed_user_data;
};

/* --- helpers -------------------------------------------------------- */

static void
free_ap_array(ExtrasMenuAccessPoint *aps, guint count)
{
    for (guint i = 0; i < count; i++)
        g_free(aps[i].ssid);
    g_free(aps);
}

/* Decodes an AP's Ssid property, which NetworkManager exposes as a
 * byte array ("ay") rather than a string, since SSIDs aren't
 * guaranteed to be valid UTF-8. We treat it as UTF-8 on a best-effort
 * basis (true for the vast majority of real-world networks) and fall
 * back to an empty string rather than crashing on anything exotic. */
static gchar *
decode_ssid(GVariant *ssid_bytes_variant)
{
    if (ssid_bytes_variant == NULL)
        return g_strdup("");

    gsize size = 0;
    gconstpointer data = g_variant_get_fixed_array(ssid_bytes_variant, &size, sizeof(guchar));

    if (size == 0)
        return g_strdup("");

    gchar *text = g_strndup((const gchar *) data, size);
    if (!g_utf8_validate(text, -1, NULL))
    {
        g_free(text);
        return g_strdup("");
    }

    return text;
}

/* --- fetching each access point's properties, then assembling the list -- */

typedef struct
{
    ExtrasMenuNetwork *network;
    gchar *active_ap_path; /* NULL if nothing is currently active */
    GPtrArray *results;    /* of owned ExtrasMenuAccessPoint* */
    guint pending_count;   /* GetAll calls still outstanding */
} ApFetchContext;

static void
finish_ap_fetch_if_done(ApFetchContext *ctx)
{
    if (ctx->pending_count > 0)
        return;

    /* Collapse duplicate SSIDs (the same network broadcasting on
     * multiple APs/channels) down to their strongest instance. */
    GHashTable *best_by_ssid = g_hash_table_new(g_str_hash, g_str_equal);

    for (guint i = 0; i < ctx->results->len; i++)
    {
        ExtrasMenuAccessPoint *ap = g_ptr_array_index(ctx->results, i);
        if (ap->ssid == NULL || ap->ssid[0] == '\0')
            continue; /* hidden/unnamed AP -- nothing sensible to show */

        ExtrasMenuAccessPoint *existing = g_hash_table_lookup(best_by_ssid, ap->ssid);
        if (existing == NULL || ap->strength > existing->strength || ap->is_active)
            g_hash_table_insert(best_by_ssid, ap->ssid, ap);
    }

    GList *unique = g_hash_table_get_values(best_by_ssid);
    guint final_count = g_list_length(unique);
    ExtrasMenuAccessPoint *final_aps = g_new0(ExtrasMenuAccessPoint, final_count);

    guint i = 0;
    for (GList *l = unique; l != NULL; l = l->next, i++)
    {
        ExtrasMenuAccessPoint *src = l->data;
        final_aps[i].ssid = g_strdup(src->ssid);
        final_aps[i].strength = src->strength;
        final_aps[i].secured = src->secured;
        final_aps[i].is_active = src->is_active;
    }

    /* Simple insertion sort by strength descending -- lists here are
     * small (a handful to a few dozen networks), so O(n^2) is fine and
     * keeps this dependency-free. */
    for (guint a = 1; a < final_count; a++)
    {
        ExtrasMenuAccessPoint key = final_aps[a];
        gint b = (gint) a - 1;
        while (b >= 0 && final_aps[b].strength < key.strength)
        {
            final_aps[b + 1] = final_aps[b];
            b--;
        }
        final_aps[b + 1] = key;
    }

    g_list_free(unique);
    g_hash_table_destroy(best_by_ssid);

    if (ctx->network->list_changed_callback != NULL)
    {
        ctx->network->list_changed_callback(TRUE, final_aps, final_count,
                                             ctx->network->list_changed_user_data);
    }

    free_ap_array(final_aps, final_count);

    for (guint j = 0; j < ctx->results->len; j++)
    {
        ExtrasMenuAccessPoint *ap = g_ptr_array_index(ctx->results, j);
        g_free(ap->ssid);
        g_free(ap);
    }
    g_ptr_array_free(ctx->results, TRUE);
    g_free(ctx->active_ap_path);
    g_free(ctx);
}

/* Per-AP call context: pairs the shared fetch context with the one
 * object path this specific GetAll call is for, so the result handler
 * can tell whether *this* AP is the active one. */
typedef struct
{
    ApFetchContext *fetch_ctx;
    gchar *ap_object_path;
} SingleApCallCtx;

static void
on_single_ap_properties_finished(GObject *source, GAsyncResult *result, gpointer user_data)
{
    SingleApCallCtx *call_ctx = user_data;
    ApFetchContext *ctx = call_ctx->fetch_ctx;
    GError *error = NULL;

    GVariant *reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), result, &error);
    if (reply != NULL)
    {
        GVariant *props = NULL;
        g_variant_get(reply, "(@a{sv})", &props);

        GVariant *ssid_v = g_variant_lookup_value(props, "Ssid", G_VARIANT_TYPE("ay"));
        GVariant *strength_v = g_variant_lookup_value(props, "Strength", G_VARIANT_TYPE_BYTE);
        GVariant *wpa_flags_v = g_variant_lookup_value(props, "WpaFlags", G_VARIANT_TYPE_UINT32);
        GVariant *rsn_flags_v = g_variant_lookup_value(props, "RsnFlags", G_VARIANT_TYPE_UINT32);

        ExtrasMenuAccessPoint *ap = g_new0(ExtrasMenuAccessPoint, 1);
        ap->ssid = decode_ssid(ssid_v);
        ap->strength = strength_v != NULL ? (gint8) g_variant_get_byte(strength_v) : 0;

        guint32 wpa_flags = wpa_flags_v != NULL ? g_variant_get_uint32(wpa_flags_v) : 0;
        guint32 rsn_flags = rsn_flags_v != NULL ? g_variant_get_uint32(rsn_flags_v) : 0;
        ap->secured = (wpa_flags != 0 || rsn_flags != 0);

        ap->is_active = (ctx->active_ap_path != NULL &&
                          g_strcmp0(call_ctx->ap_object_path, ctx->active_ap_path) == 0);

        g_ptr_array_add(ctx->results, ap);

        if (ssid_v != NULL) g_variant_unref(ssid_v);
        if (strength_v != NULL) g_variant_unref(strength_v);
        if (wpa_flags_v != NULL) g_variant_unref(wpa_flags_v);
        if (rsn_flags_v != NULL) g_variant_unref(rsn_flags_v);
        g_variant_unref(props);
        g_variant_unref(reply);
    }
    g_clear_error(&error);

    ctx->pending_count--;
    finish_ap_fetch_if_done(ctx);

    g_free(call_ctx->ap_object_path);
    g_free(call_ctx);
}

static void
on_device_props_for_scan_finished(GObject *source, GAsyncResult *result, gpointer user_data)
{
    ExtrasMenuNetwork *network = user_data;
    GError *error = NULL;

    GVariant *reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), result, &error);
    if (reply == NULL)
    {
        g_clear_error(&error);
        if (network->list_changed_callback != NULL)
            network->list_changed_callback(FALSE, NULL, 0, network->list_changed_user_data);
        return;
    }

    GVariant *props = NULL;
    g_variant_get(reply, "(@a{sv})", &props);

    GVariant *ap_paths_v = g_variant_lookup_value(props, "AccessPoints", G_VARIANT_TYPE("ao"));
    GVariant *active_ap_v = g_variant_lookup_value(props, "ActiveAccessPoint", G_VARIANT_TYPE("o"));

    gchar *active_ap_path = active_ap_v != NULL ? g_variant_dup_string(active_ap_v, NULL) : NULL;
    /* An unset ActiveAccessPoint is reported as the root object path
     * "/", not absent -- treat that the same as "no active AP". */
    if (active_ap_path != NULL && g_strcmp0(active_ap_path, "/") == 0)
        g_clear_pointer(&active_ap_path, g_free);

    if (ap_paths_v == NULL || g_variant_n_children(ap_paths_v) == 0)
    {
        if (network->list_changed_callback != NULL)
            network->list_changed_callback(TRUE, NULL, 0, network->list_changed_user_data);

        if (ap_paths_v != NULL) g_variant_unref(ap_paths_v);
        if (active_ap_v != NULL) g_variant_unref(active_ap_v);
        g_free(active_ap_path);
        g_variant_unref(props);
        g_variant_unref(reply);
        return;
    }

    ApFetchContext *ctx = g_new0(ApFetchContext, 1);
    ctx->network = network;
    ctx->active_ap_path = active_ap_path;
    ctx->results = g_ptr_array_new();
    ctx->pending_count = (guint) g_variant_n_children(ap_paths_v);

    GVariantIter iter;
    g_variant_iter_init(&iter, ap_paths_v);
    const gchar *ap_path = NULL;

    while (g_variant_iter_loop(&iter, "&o", &ap_path))
    {
        SingleApCallCtx *call_ctx = g_new0(SingleApCallCtx, 1);
        call_ctx->fetch_ctx = ctx;
        call_ctx->ap_object_path = g_strdup(ap_path);

        g_dbus_connection_call(
            network->system_bus,
            NM_BUS_NAME,
            ap_path,
            "org.freedesktop.DBus.Properties",
            "GetAll",
            g_variant_new("(s)", NM_ACCESS_POINT_IFACE),
            G_VARIANT_TYPE("(a{sv})"),
            G_DBUS_CALL_FLAGS_NONE,
            -1, NULL,
            on_single_ap_properties_finished, call_ctx);
    }

    g_variant_unref(ap_paths_v);
    if (active_ap_v != NULL) g_variant_unref(active_ap_v);
    g_variant_unref(props);
    g_variant_unref(reply);
}

static void
request_ap_list(ExtrasMenuNetwork *network)
{
    if (network->wifi_device_path == NULL)
        return;

    g_dbus_connection_call(
        network->system_bus,
        NM_BUS_NAME,
        network->wifi_device_path,
        "org.freedesktop.DBus.Properties",
        "GetAll",
        g_variant_new("(s)", NM_WIRELESS_IFACE),
        G_VARIANT_TYPE("(a{sv})"),
        G_DBUS_CALL_FLAGS_NONE,
        -1, NULL,
        on_device_props_for_scan_finished, network);
}

/* --- live change notifications ---------------------------------------- */

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
     * than trying to patch the list incrementally; scans are
     * infrequent enough that this is cheap. */
    request_ap_list(network);
}

/* --- tracking the primary connection (drives the Wi-Fi/Ethernet pill) --- */

/* Fired whenever the active Ethernet connection's IP4Config changes
 * (address assigned/renewed/lost). Just re-reads the address and
 * reports it -- cheap enough not to bother diffing. */
static void
on_ip4config_properties_finished(GObject *source, GAsyncResult *result, gpointer user_data)
{
    ExtrasMenuNetwork *network = user_data;
    GError *error = NULL;

    GVariant *reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), result, &error);
    gchar *ip_address = NULL;

    if (reply != NULL)
    {
        GVariant *props = NULL;
        g_variant_get(reply, "(@a{sv})", &props);

        /* AddressData is "aa{sv}", an array of dicts each with at
         * least an "address" (string) and "prefix" (uint32) key -- we
         * only need the first entry's address for a simple readout. */
        GVariant *address_data = g_variant_lookup_value(props, "AddressData", G_VARIANT_TYPE("aa{sv}"));
        if (address_data != NULL && g_variant_n_children(address_data) > 0)
        {
            GVariant *first_entry = g_variant_get_child_value(address_data, 0);
            GVariant *addr_v = g_variant_lookup_value(first_entry, "address", G_VARIANT_TYPE_STRING);
            if (addr_v != NULL)
            {
                ip_address = g_variant_dup_string(addr_v, NULL);
                g_variant_unref(addr_v);
            }
            g_variant_unref(first_entry);
        }
        if (address_data != NULL)
            g_variant_unref(address_data);

        g_variant_unref(props);
        g_variant_unref(reply);
    }
    g_clear_error(&error);

    if (network->status_changed_callback != NULL)
    {
        network->status_changed_callback(EXTRAS_MENU_NETWORK_KIND_ETHERNET, ip_address,
                                          network->status_changed_user_data);
    }

    g_free(ip_address);
}

static void
request_ip4config(ExtrasMenuNetwork *network, const gchar *ip4config_path)
{
    g_dbus_connection_call(
        network->system_bus,
        NM_BUS_NAME,
        ip4config_path,
        "org.freedesktop.DBus.Properties",
        "GetAll",
        g_variant_new("(s)", NM_IP4CONFIG_IFACE),
        G_VARIANT_TYPE("(a{sv})"),
        G_DBUS_CALL_FLAGS_NONE,
        -1, NULL,
        on_ip4config_properties_finished, network);
}

/* Trampoline matching GDBusSignalCallback's exact signature -- used to
 * subscribe to the watched IP4Config's own PropertiesChanged (address
 * renewed/changed) below. Delegates to request_ip4config() to re-read
 * and re-report the current address. */
static void
on_ip4config_changed_signal(GDBusConnection *connection, const gchar *sender_name,
                             const gchar *object_path, const gchar *interface_name,
                             const gchar *signal_name, GVariant *parameters,
                             gpointer user_data)
{
    ExtrasMenuNetwork *network = user_data;
    (void) connection;
    (void) sender_name;
    (void) interface_name;
    (void) signal_name;
    (void) parameters;

    if (network->watched_ip4config_path != NULL && g_strcmp0(object_path, network->watched_ip4config_path) == 0)
        request_ip4config(network, network->watched_ip4config_path);
}

/* Switches which IP4Config object we're watching for address changes
 * (or stops watching entirely, if new_path is NULL). Called whenever
 * the active connection changes, since a new connection means a new
 * (or no) IP4Config object. */
static void
rewatch_ip4config(ExtrasMenuNetwork *network, const gchar *new_path)
{
    if (g_strcmp0(network->watched_ip4config_path, new_path) == 0)
        return; /* already watching the right thing (or correctly watching nothing) */

    if (network->ip4config_props_subscription_id != 0)
    {
        g_dbus_connection_signal_unsubscribe(network->system_bus,
                                              network->ip4config_props_subscription_id);
        network->ip4config_props_subscription_id = 0;
    }

    g_free(network->watched_ip4config_path);
    network->watched_ip4config_path = new_path != NULL ? g_strdup(new_path) : NULL;

    if (new_path == NULL)
        return;

    network->ip4config_props_subscription_id = g_dbus_connection_signal_subscribe(
        network->system_bus, NM_BUS_NAME,
        "org.freedesktop.DBus.Properties", "PropertiesChanged",
        new_path, NULL, G_DBUS_SIGNAL_FLAGS_NONE,
        on_ip4config_changed_signal, network, NULL);
}

/* Reads NetworkManager.Connection.Active's Type and Ip4Config
 * properties for the given active-connection object path, then
 * reports the resulting status (Ethernet+IP, Wi-Fi, or -- if the type
 * is neither -- treats it as no primary connection, e.g. a VPN-only
 * "connection" with no underlying physical link we'd show a pill
 * for). */
static void
on_active_connection_properties_finished(GObject *source, GAsyncResult *result, gpointer user_data)
{
    ExtrasMenuNetwork *network = user_data;
    GError *error = NULL;

    GVariant *reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), result, &error);
    if (reply == NULL)
    {
        g_clear_error(&error);
        rewatch_ip4config(network, NULL);
        if (network->status_changed_callback != NULL)
            network->status_changed_callback(EXTRAS_MENU_NETWORK_KIND_NONE, NULL,
                                              network->status_changed_user_data);
        return;
    }

    GVariant *props = NULL;
    g_variant_get(reply, "(@a{sv})", &props);

    GVariant *type_v = g_variant_lookup_value(props, "Type", G_VARIANT_TYPE_STRING);
    GVariant *ip4config_v = g_variant_lookup_value(props, "Ip4Config", G_VARIANT_TYPE("o"));

    const gchar *type_str = type_v != NULL ? g_variant_get_string(type_v, NULL) : "";

    if (g_strcmp0(type_str, "802-3-ethernet") == 0)
    {
        const gchar *ip4config_path = ip4config_v != NULL ? g_variant_get_string(ip4config_v, NULL) : NULL;
        rewatch_ip4config(network, ip4config_path);
        if (ip4config_path != NULL)
            request_ip4config(network, ip4config_path);
        else if (network->status_changed_callback != NULL)
            network->status_changed_callback(EXTRAS_MENU_NETWORK_KIND_ETHERNET, NULL,
                                              network->status_changed_user_data);
    }
    else if (g_strcmp0(type_str, "802-11-wireless") == 0)
    {
        rewatch_ip4config(network, NULL);
        if (network->status_changed_callback != NULL)
            network->status_changed_callback(EXTRAS_MENU_NETWORK_KIND_WIFI, NULL,
                                              network->status_changed_user_data);
    }
    else
    {
        rewatch_ip4config(network, NULL);
        if (network->status_changed_callback != NULL)
            network->status_changed_callback(EXTRAS_MENU_NETWORK_KIND_NONE, NULL,
                                              network->status_changed_user_data);
    }

    if (type_v != NULL) g_variant_unref(type_v);
    if (ip4config_v != NULL) g_variant_unref(ip4config_v);
    g_variant_unref(props);
    g_variant_unref(reply);
}

static void
on_primary_connection_path_finished(GObject *source, GAsyncResult *result, gpointer user_data)
{
    ExtrasMenuNetwork *network = user_data;
    GError *error = NULL;

    GVariant *reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), result, &error);
    if (reply == NULL)
    {
        g_clear_error(&error);
        rewatch_ip4config(network, NULL);
        if (network->status_changed_callback != NULL)
            network->status_changed_callback(EXTRAS_MENU_NETWORK_KIND_NONE, NULL,
                                              network->status_changed_user_data);
        return;
    }

    GVariant *boxed = NULL;
    g_variant_get(reply, "(v)", &boxed);
    const gchar *primary_path = g_variant_get_string(boxed, NULL);

    if (primary_path == NULL || g_strcmp0(primary_path, "/") == 0)
    {
        /* No primary connection at all -- fully offline. */
        rewatch_ip4config(network, NULL);
        if (network->status_changed_callback != NULL)
            network->status_changed_callback(EXTRAS_MENU_NETWORK_KIND_NONE, NULL,
                                              network->status_changed_user_data);
    }
    else
    {
        g_dbus_connection_call(
            network->system_bus,
            NM_BUS_NAME,
            primary_path,
            "org.freedesktop.DBus.Properties",
            "GetAll",
            g_variant_new("(s)", NM_CONNECTION_ACTIVE_IFACE),
            G_VARIANT_TYPE("(a{sv})"),
            G_DBUS_CALL_FLAGS_NONE,
            -1, NULL,
            on_active_connection_properties_finished, network);
    }

    g_variant_unref(boxed);
    g_variant_unref(reply);
}

static void
request_primary_connection_status(ExtrasMenuNetwork *network)
{
    g_dbus_connection_call(
        network->system_bus,
        NM_BUS_NAME,
        NM_OBJ_PATH,
        "org.freedesktop.DBus.Properties",
        "Get",
        g_variant_new("(ss)", NM_IFACE, "PrimaryConnection"),
        G_VARIANT_TYPE("(v)"),
        G_DBUS_CALL_FLAGS_NONE,
        -1, NULL,
        on_primary_connection_path_finished, network);
}

static void
on_get_wifi_enabled_finished(GObject *source, GAsyncResult *result, gpointer user_data)
{
    ExtrasMenuNetwork *network = user_data;
    GError *error = NULL;

    GVariant *reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), result, &error);
    if (reply == NULL)
    {
        g_clear_error(&error);
        return;
    }

    GVariant *boxed = NULL;
    g_variant_get(reply, "(v)", &boxed);
    gboolean enabled = g_variant_get_boolean(boxed);
    g_variant_unref(boxed);
    g_variant_unref(reply);

    if (network->wifi_enabled_changed_callback != NULL)
        network->wifi_enabled_changed_callback(enabled, network->wifi_enabled_changed_user_data);
}

static void
request_wifi_enabled_state(ExtrasMenuNetwork *network)
{
    g_dbus_connection_call(
        network->system_bus,
        NM_BUS_NAME,
        NM_OBJ_PATH,
        "org.freedesktop.DBus.Properties",
        "Get",
        g_variant_new("(ss)", NM_IFACE, "WirelessEnabled"),
        G_VARIANT_TYPE("(v)"),
        G_DBUS_CALL_FLAGS_NONE,
        -1, NULL,
        on_get_wifi_enabled_finished, network);
}

static void
on_nm_properties_changed(GDBusConnection *connection, const gchar *sender_name,
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

    /* Either PrimaryConnection or WirelessEnabled (or both, or
     * unrelated properties) may have changed -- re-check both rather
     * than inspecting parameters to figure out which. Cheap enough. */
    request_primary_connection_status(network);
    request_wifi_enabled_state(network);
}

static void
start_primary_connection_tracking(ExtrasMenuNetwork *network)
{
    network->nm_props_subscription_id = g_dbus_connection_signal_subscribe(
        network->system_bus, NM_BUS_NAME,
        "org.freedesktop.DBus.Properties", "PropertiesChanged",
        NM_OBJ_PATH, NULL, G_DBUS_SIGNAL_FLAGS_NONE,
        on_nm_properties_changed, network, NULL);

    request_primary_connection_status(network);
    request_wifi_enabled_state(network);
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

        request_ap_list(ctx->network);

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
        -1, NULL,
        on_device_type_finished, ctx);
}

static void
on_get_devices_finished(GObject *source, GAsyncResult *result, gpointer user_data)
{
    ExtrasMenuNetwork *network = user_data;
    GError *error = NULL;

    GVariant *reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), result, &error);
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
        -1, NULL,
        on_get_devices_finished, network);
}

/* --- bus connection lifecycle --------------------------------------------- */

static void
on_bus_get_finished(GObject *source, GAsyncResult *result, gpointer user_data)
{
    ExtrasMenuNetwork *network = user_data;
    (void) source;

    GError *error = NULL;
    network->system_bus = g_bus_get_finish(result, &error);

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
    start_primary_connection_tracking(network);
}

/* --- connecting / disconnecting ------------------------------------------- */

typedef struct
{
    ExtrasMenuNetworkConnectResultFunc callback;
    gpointer user_data;
} ConnectResultCtx;

static void
on_connect_finished(GObject *source, GAsyncResult *result, gpointer user_data)
{
    ConnectResultCtx *ctx = user_data;
    GError *error = NULL;

    GVariant *reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), result, &error);

    if (ctx->callback != NULL)
    {
        if (reply != NULL)
            ctx->callback(TRUE, NULL, ctx->user_data);
        else
            ctx->callback(FALSE, error != NULL ? error->message : "Unknown error", ctx->user_data);
    }

    if (reply != NULL)
        g_variant_unref(reply);
    g_clear_error(&error);
    g_free(ctx);
}

/* Builds a minimal NetworkManager connection settings dict for a given
 * SSID/password, suitable for AddAndActivateConnection. Only used as a
 * fallback when no existing saved profile for this SSID was found --
 * see find_existing_connection_for_ssid() below, which is tried
 * first. */
static GVariant *
build_connection_settings(const gchar *ssid, const gchar *password)
{
    GVariantBuilder connection_builder;
    g_variant_builder_init(&connection_builder, G_VARIANT_TYPE("a{sa{sv}}"));

    /* [connection] */
    GVariantBuilder conn_section;
    g_variant_builder_init(&conn_section, G_VARIANT_TYPE("a{sv}"));
    g_variant_builder_add(&conn_section, "{sv}", "id", g_variant_new_string(ssid));
    g_variant_builder_add(&conn_section, "{sv}", "type", g_variant_new_string("802-11-wireless"));
    g_variant_builder_add(&connection_builder, "{s@a{sv}}", "connection",
                           g_variant_builder_end(&conn_section));

    /* [802-11-wireless] */
    GVariantBuilder wifi_section;
    g_variant_builder_init(&wifi_section, G_VARIANT_TYPE("a{sv}"));
    GBytes *ssid_bytes = g_bytes_new(ssid, strlen(ssid));
    g_variant_builder_add(&wifi_section, "{sv}", "ssid",
                           g_variant_new_from_bytes(G_VARIANT_TYPE("ay"), ssid_bytes, TRUE));
    g_bytes_unref(ssid_bytes);
    g_variant_builder_add(&connection_builder, "{s@a{sv}}", "802-11-wireless",
                           g_variant_builder_end(&wifi_section));

    /* [802-11-wireless-security] -- only included for secured
     * networks; NetworkManager infers WPA-PSK vs other schemes from
     * the AP's own advertised capabilities when key-mgmt is "wpa-psk",
     * which covers the overwhelming majority of home/office networks. */
    if (password != NULL && password[0] != '\0')
    {
        GVariantBuilder security_section;
        g_variant_builder_init(&security_section, G_VARIANT_TYPE("a{sv}"));
        g_variant_builder_add(&security_section, "{sv}", "key-mgmt",
                               g_variant_new_string("wpa-psk"));
        g_variant_builder_add(&security_section, "{sv}", "psk",
                               g_variant_new_string(password));
        g_variant_builder_add(&connection_builder, "{s@a{sv}}", "802-11-wireless-security",
                               g_variant_builder_end(&security_section));
    }

    return g_variant_builder_end(&connection_builder);
}

/* --- finding an already-saved connection profile for an SSID --------- */

typedef struct
{
    ExtrasMenuNetwork *network;
    gchar *ssid;
    gchar *password; /* only used if no existing profile is found */
    ExtrasMenuNetworkConnectResultFunc result_callback;
    gpointer result_user_data;

    gchar **profile_paths; /* NULL-terminated */
    guint index;
} FindProfileCtx;

static void find_profile_check_next(FindProfileCtx *ctx);

static void
free_find_profile_ctx(FindProfileCtx *ctx)
{
    g_free(ctx->ssid);
    g_free(ctx->password);
    g_strfreev(ctx->profile_paths);
    g_free(ctx);
}

/* Once we know whether an existing profile matches (or we've run out
 * of profiles to check), either activates the found one or falls back
 * to creating+activating a fresh one with build_connection_settings(). */
static void
proceed_with_connect(FindProfileCtx *ctx, const gchar *existing_profile_path)
{
    ConnectResultCtx *result_ctx = g_new0(ConnectResultCtx, 1);
    result_ctx->callback = ctx->result_callback;
    result_ctx->user_data = ctx->result_user_data;

    if (existing_profile_path != NULL)
    {
        /* Reactivate the saved profile as-is -- its stored password
         * (if any) is used by NetworkManager itself, so the user isn't
         * prompted again for a network they've already connected to
         * before. */
        g_dbus_connection_call(
            ctx->network->system_bus,
            NM_BUS_NAME,
            NM_OBJ_PATH,
            NM_IFACE,
            "ActivateConnection",
            g_variant_new("(ooo)", existing_profile_path, ctx->network->wifi_device_path, "/"),
            G_VARIANT_TYPE("(o)"),
            G_DBUS_CALL_FLAGS_NONE,
            -1, NULL,
            on_connect_finished, result_ctx);
    }
    else
    {
        GVariant *connection_settings = build_connection_settings(ctx->ssid, ctx->password);

        g_dbus_connection_call(
            ctx->network->system_bus,
            NM_BUS_NAME,
            NM_OBJ_PATH,
            NM_IFACE,
            "AddAndActivateConnection",
            g_variant_new("(@a{sa{sv}}oo)", connection_settings, ctx->network->wifi_device_path, "/"),
            G_VARIANT_TYPE("(oo)"),
            G_DBUS_CALL_FLAGS_NONE,
            -1, NULL,
            on_connect_finished, result_ctx);
    }

    free_find_profile_ctx(ctx);
}

static void
on_profile_settings_finished(GObject *source, GAsyncResult *result, gpointer user_data)
{
    FindProfileCtx *ctx = user_data;
    GError *error = NULL;

    GVariant *reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), result, &error);
    gboolean matches = FALSE;

    if (reply != NULL)
    {
        GVariant *settings = NULL;
        g_variant_get(reply, "(@a{sa{sv}})", &settings);

        GVariant *wifi_section = g_variant_lookup_value(settings, "802-11-wireless", G_VARIANT_TYPE("a{sv}"));
        if (wifi_section != NULL)
        {
            GVariant *ssid_v = g_variant_lookup_value(wifi_section, "ssid", G_VARIANT_TYPE("ay"));
            if (ssid_v != NULL)
            {
                gchar *found_ssid = decode_ssid(ssid_v);
                matches = (g_strcmp0(found_ssid, ctx->ssid) == 0);
                g_free(found_ssid);
                g_variant_unref(ssid_v);
            }
            g_variant_unref(wifi_section);
        }

        g_variant_unref(settings);
        g_variant_unref(reply);
    }
    g_clear_error(&error);

    if (matches)
    {
        proceed_with_connect(ctx, ctx->profile_paths[ctx->index]);
        return;
    }

    ctx->index++;
    find_profile_check_next(ctx);
}

static void
find_profile_check_next(FindProfileCtx *ctx)
{
    if (ctx->profile_paths[ctx->index] == NULL)
    {
        /* No existing profile matched -- create a new one. */
        proceed_with_connect(ctx, NULL);
        return;
    }

    g_dbus_connection_call(
        ctx->network->system_bus,
        NM_BUS_NAME,
        ctx->profile_paths[ctx->index],
        NM_SETTINGS_CONNECTION_IFACE,
        "GetSettings",
        NULL,
        G_VARIANT_TYPE("(a{sa{sv}})"),
        G_DBUS_CALL_FLAGS_NONE,
        -1, NULL,
        on_profile_settings_finished, ctx);
}

static void
on_list_connections_finished(GObject *source, GAsyncResult *result, gpointer user_data)
{
    FindProfileCtx *ctx = user_data;
    GError *error = NULL;

    GVariant *reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), result, &error);
    if (reply == NULL)
    {
        /* Couldn't even list profiles -- fall back to creating a new
         * one rather than failing the connect attempt outright. */
        g_clear_error(&error);
        proceed_with_connect(ctx, NULL);
        return;
    }

    GVariant *paths_v = NULL;
    g_variant_get(reply, "(@ao)", &paths_v);

    guint n = (guint) g_variant_n_children(paths_v);
    ctx->profile_paths = g_new0(gchar *, n + 1); /* NULL-terminated */
    for (guint i = 0; i < n; i++)
    {
        GVariant *child = g_variant_get_child_value(paths_v, i);
        ctx->profile_paths[i] = g_variant_dup_string(child, NULL);
        g_variant_unref(child);
    }

    g_variant_unref(paths_v);
    g_variant_unref(reply);

    ctx->index = 0;
    find_profile_check_next(ctx);
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

    g_bus_get(G_BUS_TYPE_SYSTEM, NULL, on_bus_get_finished, network);

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
extras_menu_network_connect(ExtrasMenuNetwork *network,
                             const gchar *ssid,
                             const gchar *password,
                             ExtrasMenuNetworkConnectResultFunc result_callback,
                             gpointer result_user_data)
{
    if (network == NULL || network->system_bus == NULL || network->wifi_device_path == NULL || ssid == NULL)
    {
        if (result_callback != NULL)
            result_callback(FALSE, "Wi-Fi not available", result_user_data);
        return;
    }

    /* Look for an already-saved profile for this SSID first, so
     * reconnecting to a network we've used before (including the one
     * we're currently on) doesn't prompt for a password again --
     * NetworkManager reuses the profile's stored credentials. Falls
     * back to creating a fresh profile (see build_connection_settings)
     * if none is found. */
    FindProfileCtx *ctx = g_new0(FindProfileCtx, 1);
    ctx->network = network;
    ctx->ssid = g_strdup(ssid);
    ctx->password = password != NULL ? g_strdup(password) : NULL;
    ctx->result_callback = result_callback;
    ctx->result_user_data = result_user_data;

    g_dbus_connection_call(
        network->system_bus,
        NM_BUS_NAME,
        NM_SETTINGS_OBJ_PATH,
        NM_SETTINGS_IFACE,
        "ListConnections",
        NULL,
        G_VARIANT_TYPE("(ao)"),
        G_DBUS_CALL_FLAGS_NONE,
        -1, NULL,
        on_list_connections_finished, ctx);
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
    g_free(network);
}