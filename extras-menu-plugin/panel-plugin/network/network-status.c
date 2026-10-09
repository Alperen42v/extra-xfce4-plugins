/*
 * Network backend: tracking NetworkManager's primary connection, which
 * drives the Wi-Fi/Ethernet pill (connection kind, Ethernet IP address,
 * Wi-Fi radio on/off state).
 */

#include "network-private.h"

#include <string.h>

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
    if (reply == NULL && call_was_cancelled(error))
    {
        g_clear_error(&error);
        return;
    }

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
        -1, network->cancellable,
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
    if (reply == NULL && call_was_cancelled(error))
    {
        g_clear_error(&error);
        return;
    }

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
    if (reply == NULL && call_was_cancelled(error))
    {
        g_clear_error(&error);
        return;
    }

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
            -1, network->cancellable,
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
        -1, network->cancellable,
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
        -1, network->cancellable,
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

void
network_start_primary_connection_tracking(ExtrasMenuNetwork *network)
{
    network->nm_props_subscription_id = g_dbus_connection_signal_subscribe(
        network->system_bus, NM_BUS_NAME,
        "org.freedesktop.DBus.Properties", "PropertiesChanged",
        NM_OBJ_PATH, NULL, G_DBUS_SIGNAL_FLAGS_NONE,
        on_nm_properties_changed, network, NULL);

    request_primary_connection_status(network);
    request_wifi_enabled_state(network);
}