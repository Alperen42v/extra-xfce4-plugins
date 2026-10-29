/*
 * Network backend: tracking NetworkManager's primary connection, which
 * drives the Wi-Fi/Ethernet pill (connection kind, Ethernet IP address,
 * Wi-Fi radio on/off state).
 */

#include "network-private.h"

#include <string.h>

/* --- request contexts ------------------------------------------------------ */

/* Every call in the status chain carries one of these instead of the
 * bare network pointer, for two reasons:
 *
 *  - Ordering. Finding the status takes three calls in a row, and a new
 *    chain can start before an older one has finished (NetworkManager
 *    emits several PropertiesChanged in quick succession). A chain
 *    whose generation is no longer the current one is stale and drops
 *    its result -- see ExtrasMenuNetwork.status_generation.
 *
 *  - Safety after free. A call that already had its reply in when
 *    extras_menu_network_free() ran still delivers it normally, with
 *    no cancellation error, and by then `network` is gone. The context
 *    holds its own reference to the cancellable, which free() cancels
 *    first thing, so the callback can tell without touching `network`.
 */
typedef struct
{
    ExtrasMenuNetwork *network; /* only valid while the cancellable isn't cancelled */
    GCancellable *cancellable;  /* own reference */
    guint generation;

    /* Only used by the walk over all active connections (see
     * walk_next_active_connection()). */
    gchar **paths;      /* NULL-terminated, owned */
    guint index;
    gboolean wifi_seen; /* an activated Wi-Fi connection was found on the way */
} StatusRequestCtx;

/* NMActiveConnectionState */
#define NM_ACTIVE_CONNECTION_STATE_ACTIVATED 2

static StatusRequestCtx *
status_request_ctx_new(ExtrasMenuNetwork *network, guint generation)
{
    StatusRequestCtx *ctx = g_new0(StatusRequestCtx, 1);
    ctx->network = network;
    ctx->cancellable = g_object_ref(network->cancellable);
    ctx->generation = generation;
    return ctx;
}

static void
status_request_ctx_free(StatusRequestCtx *ctx)
{
    g_object_unref(ctx->cancellable);
    g_strfreev(ctx->paths);
    g_free(ctx);
}

/* TRUE if the callback must drop its result without touching `network`
 * any further: the backend has been freed, or (when check_generation is
 * set) a newer status chain has superseded this one. The cancellation
 * test comes first and short-circuits, so a freed network is never
 * dereferenced. */
static gboolean
status_request_is_stale(const StatusRequestCtx *ctx, gboolean check_generation)
{
    if (g_cancellable_is_cancelled(ctx->cancellable))
        return TRUE;

    return check_generation && ctx->generation != ctx->network->status_generation;
}

static void
report_status(ExtrasMenuNetwork *network, ExtrasMenuNetworkKind kind, const gchar *ip_address)
{
    if (network->status_changed_callback != NULL)
        network->status_changed_callback(kind, ip_address, network->status_changed_user_data);
}

/* --- tracking the primary connection (drives the Wi-Fi/Ethernet pill) --- */

static void request_primary_connection_status(ExtrasMenuNetwork *network);

/* Reports the active Ethernet connection's IP address -- the last link
 * of the status chain. */
static void
on_ip4config_properties_finished(GObject *source, GAsyncResult *result, gpointer user_data)
{
    StatusRequestCtx *ctx = user_data;
    GError *error = NULL;

    GVariant *reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), result, &error);

    if (status_request_is_stale(ctx, TRUE))
    {
        if (reply != NULL)
            g_variant_unref(reply);
        g_clear_error(&error);
        status_request_ctx_free(ctx);
        return;
    }

    ExtrasMenuNetwork *network = ctx->network;
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

    report_status(network, EXTRAS_MENU_NETWORK_KIND_ETHERNET, ip_address);

    g_free(ip_address);
    status_request_ctx_free(ctx);
}

static void
request_ip4config(ExtrasMenuNetwork *network, const gchar *ip4config_path, guint generation)
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
        on_ip4config_properties_finished, status_request_ctx_new(network, generation));
}

/* Trampoline matching GDBusSignalCallback's exact signature -- used to
 * subscribe to the watched IP4Config's own PropertiesChanged (address
 * renewed/changed) below. Re-runs the whole status chain rather than
 * just re-reading the address: a lone IP read started from here would
 * be a second, unordered writer next to the chain, and could report
 * "Ethernet" for a connection the chain is just finding out is gone. */
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
        request_primary_connection_status(network);
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

/* The primary connection is an active Ethernet connection: watch its
 * IP4Config and report Ethernet (with the address once it is known). */
static void
handle_ethernet_connection(ExtrasMenuNetwork *network, const gchar *ip4config_path,
                            guint generation)
{
    /* "/" is D-Bus' "no object": the connection has no IPv4
     * configuration (yet, or at all -- e.g. IPv6-only). There's
     * nothing to read or watch then, and treating it as a real
     * path would only produce a failing call. */
    if (ip4config_path != NULL && g_strcmp0(ip4config_path, "/") == 0)
        ip4config_path = NULL;

    rewatch_ip4config(network, ip4config_path);
    if (ip4config_path != NULL)
        request_ip4config(network, ip4config_path, generation);
    else
        report_status(network, EXTRAS_MENU_NETWORK_KIND_ETHERNET, NULL);
}

/* --- finding the real link under a VPN ---------------------------------------
 *
 * While a VPN is up, NetworkManager's PrimaryConnection is the VPN's
 * own connection (type "vpn", "wireguard", "tun", ...), not the Wi-Fi or
 * Ethernet link it runs over. Treating that as "no connection" made the
 * pill and the panel icon look offline for as long as a VPN was
 * connected. So when the primary connection is neither Ethernet nor
 * Wi-Fi, all active connections are looked at one by one instead:
 * an activated Ethernet one wins (same priority as always), otherwise an
 * activated Wi-Fi one, otherwise there really is no connection. */

static void walk_next_active_connection(StatusRequestCtx *ctx);

static void
on_walk_connection_properties_finished(GObject *source, GAsyncResult *result, gpointer user_data)
{
    StatusRequestCtx *ctx = user_data;
    GError *error = NULL;

    GVariant *reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), result, &error);

    if (status_request_is_stale(ctx, TRUE))
    {
        if (reply != NULL)
            g_variant_unref(reply);
        g_clear_error(&error);
        status_request_ctx_free(ctx);
        return;
    }

    g_clear_error(&error);

    if (reply != NULL)
    {
        GVariant *props = NULL;
        g_variant_get(reply, "(@a{sv})", &props);

        GVariant *type_v = g_variant_lookup_value(props, "Type", G_VARIANT_TYPE_STRING);
        GVariant *state_v = g_variant_lookup_value(props, "State", G_VARIANT_TYPE_UINT32);
        GVariant *ip4config_v = g_variant_lookup_value(props, "Ip4Config", G_VARIANT_TYPE("o"));

        const gchar *type_str = type_v != NULL ? g_variant_get_string(type_v, NULL) : "";
        gboolean activated = state_v != NULL &&
                             g_variant_get_uint32(state_v) == NM_ACTIVE_CONNECTION_STATE_ACTIVATED;
        gboolean found_ethernet = FALSE;

        if (activated && g_strcmp0(type_str, "802-3-ethernet") == 0)
        {
            found_ethernet = TRUE;
            handle_ethernet_connection(ctx->network,
                                        ip4config_v != NULL ? g_variant_get_string(ip4config_v, NULL) : NULL,
                                        ctx->generation);
        }
        else if (activated && g_strcmp0(type_str, "802-11-wireless") == 0)
        {
            ctx->wifi_seen = TRUE;
        }

        if (type_v != NULL) g_variant_unref(type_v);
        if (state_v != NULL) g_variant_unref(state_v);
        if (ip4config_v != NULL) g_variant_unref(ip4config_v);
        g_variant_unref(props);
        g_variant_unref(reply);

        if (found_ethernet)
        {
            status_request_ctx_free(ctx);
            return;
        }
    }

    ctx->index++;
    walk_next_active_connection(ctx);
}

/* Reads the next active connection's properties, or -- when all of them
 * have been looked at -- reports what was found. Only called while ctx
 * is known to be current (its callers have just checked). */
static void
walk_next_active_connection(StatusRequestCtx *ctx)
{
    ExtrasMenuNetwork *network = ctx->network;

    if (ctx->paths == NULL || ctx->paths[ctx->index] == NULL)
    {
        rewatch_ip4config(network, NULL);
        report_status(network,
                      ctx->wifi_seen ? EXTRAS_MENU_NETWORK_KIND_WIFI : EXTRAS_MENU_NETWORK_KIND_NONE,
                      NULL);
        status_request_ctx_free(ctx);
        return;
    }

    g_dbus_connection_call(
        network->system_bus,
        NM_BUS_NAME,
        ctx->paths[ctx->index],
        "org.freedesktop.DBus.Properties",
        "GetAll",
        g_variant_new("(s)", NM_CONNECTION_ACTIVE_IFACE),
        G_VARIANT_TYPE("(a{sv})"),
        G_DBUS_CALL_FLAGS_NONE,
        -1, network->cancellable,
        on_walk_connection_properties_finished, ctx);
}

static void
on_active_connections_finished(GObject *source, GAsyncResult *result, gpointer user_data)
{
    StatusRequestCtx *ctx = user_data;
    GError *error = NULL;

    GVariant *reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), result, &error);

    if (status_request_is_stale(ctx, TRUE))
    {
        if (reply != NULL)
            g_variant_unref(reply);
        g_clear_error(&error);
        status_request_ctx_free(ctx);
        return;
    }

    ExtrasMenuNetwork *network = ctx->network;

    if (reply == NULL)
    {
        g_clear_error(&error);
        rewatch_ip4config(network, NULL);
        report_status(network, EXTRAS_MENU_NETWORK_KIND_NONE, NULL);
        status_request_ctx_free(ctx);
        return;
    }

    GVariant *boxed = NULL;
    g_variant_get(reply, "(v)", &boxed);

    gsize n = g_variant_n_children(boxed);
    ctx->paths = g_new0(gchar *, n + 1);
    for (gsize i = 0; i < n; i++)
    {
        GVariant *child = g_variant_get_child_value(boxed, i);
        ctx->paths[i] = g_variant_dup_string(child, NULL);
        g_variant_unref(child);
    }

    g_variant_unref(boxed);
    g_variant_unref(reply);

    ctx->index = 0;
    ctx->wifi_seen = FALSE;
    walk_next_active_connection(ctx);
}

/* Next link of the status chain when the primary connection is
 * something else than Ethernet or Wi-Fi (a VPN, usually). */
static void
request_physical_connection_status(ExtrasMenuNetwork *network, guint generation)
{
    g_dbus_connection_call(
        network->system_bus,
        NM_BUS_NAME,
        NM_OBJ_PATH,
        "org.freedesktop.DBus.Properties",
        "Get",
        g_variant_new("(ss)", NM_IFACE, "ActiveConnections"),
        G_VARIANT_TYPE("(v)"),
        G_DBUS_CALL_FLAGS_NONE,
        -1, network->cancellable,
        on_active_connections_finished, status_request_ctx_new(network, generation));
}

/* Reads NetworkManager.Connection.Active's Type and Ip4Config
 * properties for the given active-connection object path, then
 * reports the resulting status (Ethernet+IP or Wi-Fi). If the type is
 * neither -- typically a VPN, which NetworkManager makes the primary
 * connection while it is up -- the physical link under it is looked up
 * instead (see request_physical_connection_status()). */
static void
on_active_connection_properties_finished(GObject *source, GAsyncResult *result, gpointer user_data)
{
    StatusRequestCtx *ctx = user_data;
    GError *error = NULL;

    GVariant *reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), result, &error);

    if (status_request_is_stale(ctx, TRUE))
    {
        if (reply != NULL)
            g_variant_unref(reply);
        g_clear_error(&error);
        status_request_ctx_free(ctx);
        return;
    }

    ExtrasMenuNetwork *network = ctx->network;

    if (reply == NULL)
    {
        g_clear_error(&error);
        rewatch_ip4config(network, NULL);
        report_status(network, EXTRAS_MENU_NETWORK_KIND_NONE, NULL);
        status_request_ctx_free(ctx);
        return;
    }

    GVariant *props = NULL;
    g_variant_get(reply, "(@a{sv})", &props);

    GVariant *type_v = g_variant_lookup_value(props, "Type", G_VARIANT_TYPE_STRING);
    GVariant *ip4config_v = g_variant_lookup_value(props, "Ip4Config", G_VARIANT_TYPE("o"));

    const gchar *type_str = type_v != NULL ? g_variant_get_string(type_v, NULL) : "";

    if (g_strcmp0(type_str, "802-3-ethernet") == 0)
    {
        handle_ethernet_connection(network,
                                    ip4config_v != NULL ? g_variant_get_string(ip4config_v, NULL) : NULL,
                                    ctx->generation);
    }
    else if (g_strcmp0(type_str, "802-11-wireless") == 0)
    {
        rewatch_ip4config(network, NULL);
        report_status(network, EXTRAS_MENU_NETWORK_KIND_WIFI, NULL);
    }
    else
    {
        /* A VPN (or similar) on top of the real link. Not "offline". */
        request_physical_connection_status(network, ctx->generation);
    }

    if (type_v != NULL) g_variant_unref(type_v);
    if (ip4config_v != NULL) g_variant_unref(ip4config_v);
    g_variant_unref(props);
    g_variant_unref(reply);
    status_request_ctx_free(ctx);
}

static void
on_primary_connection_path_finished(GObject *source, GAsyncResult *result, gpointer user_data)
{
    StatusRequestCtx *ctx = user_data;
    GError *error = NULL;

    GVariant *reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), result, &error);

    if (status_request_is_stale(ctx, TRUE))
    {
        if (reply != NULL)
            g_variant_unref(reply);
        g_clear_error(&error);
        status_request_ctx_free(ctx);
        return;
    }

    ExtrasMenuNetwork *network = ctx->network;

    if (reply == NULL)
    {
        g_clear_error(&error);
        rewatch_ip4config(network, NULL);
        report_status(network, EXTRAS_MENU_NETWORK_KIND_NONE, NULL);
        status_request_ctx_free(ctx);
        return;
    }

    GVariant *boxed = NULL;
    g_variant_get(reply, "(v)", &boxed);
    const gchar *primary_path = g_variant_get_string(boxed, NULL);

    if (primary_path == NULL || g_strcmp0(primary_path, "/") == 0)
    {
        /* No primary connection at all -- fully offline. */
        rewatch_ip4config(network, NULL);
        report_status(network, EXTRAS_MENU_NETWORK_KIND_NONE, NULL);
    }
    else
    {
        /* Next link of the chain, same generation. */
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
            on_active_connection_properties_finished,
            status_request_ctx_new(network, ctx->generation));
    }

    g_variant_unref(boxed);
    g_variant_unref(reply);
    status_request_ctx_free(ctx);
}

/* Starts a fresh status chain. Bumping the generation is what makes
 * every chain still in flight stale. */
static void
request_primary_connection_status(ExtrasMenuNetwork *network)
{
    guint generation = ++network->status_generation;

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
        on_primary_connection_path_finished, status_request_ctx_new(network, generation));
}

/* --- Wi-Fi radio on/off state ----------------------------------------------- */

static void
on_get_wifi_enabled_finished(GObject *source, GAsyncResult *result, gpointer user_data)
{
    StatusRequestCtx *ctx = user_data;
    GError *error = NULL;

    GVariant *reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), result, &error);

    /* A single call, whose replies NetworkManager sends in the order
     * the requests came in -- only the cancellation check applies, no
     * generation. */
    if (status_request_is_stale(ctx, FALSE))
    {
        if (reply != NULL)
            g_variant_unref(reply);
        g_clear_error(&error);
        status_request_ctx_free(ctx);
        return;
    }

    ExtrasMenuNetwork *network = ctx->network;

    if (reply == NULL)
    {
        g_clear_error(&error);
        status_request_ctx_free(ctx);
        return;
    }

    GVariant *boxed = NULL;
    g_variant_get(reply, "(v)", &boxed);
    gboolean enabled = g_variant_get_boolean(boxed);
    g_variant_unref(boxed);
    g_variant_unref(reply);

    if (network->wifi_enabled_changed_callback != NULL)
        network->wifi_enabled_changed_callback(enabled, network->wifi_enabled_changed_user_data);

    status_request_ctx_free(ctx);
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
        on_get_wifi_enabled_finished, status_request_ctx_new(network, 0));
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

/* Any active connection changed state (activating -> activated, ...).
 * The status only counts *activated* connections, so it has to be read
 * again when one becomes (or stops being) activated. */
static void
on_active_connection_state_changed(GDBusConnection *connection, const gchar *sender_name,
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

    request_primary_connection_status(network);
}

void
network_start_primary_connection_tracking(ExtrasMenuNetwork *network)
{
    network->active_state_subscription_id = g_dbus_connection_signal_subscribe(
        network->system_bus, NM_BUS_NAME,
        NM_CONNECTION_ACTIVE_IFACE, "StateChanged",
        NULL, NULL, G_DBUS_SIGNAL_FLAGS_NONE,
        on_active_connection_state_changed, network, NULL);

    network->nm_props_subscription_id = g_dbus_connection_signal_subscribe(
        network->system_bus, NM_BUS_NAME,
        "org.freedesktop.DBus.Properties", "PropertiesChanged",
        NM_OBJ_PATH, NULL, G_DBUS_SIGNAL_FLAGS_NONE,
        on_nm_properties_changed, network, NULL);

    request_primary_connection_status(network);
    request_wifi_enabled_state(network);
}