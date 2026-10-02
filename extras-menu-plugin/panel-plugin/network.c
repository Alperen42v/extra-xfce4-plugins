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

#define NM_DEVICE_TYPE_WIFI     2

/* NM80211ApFlags -- the AP's own general capability bits. */
#define NM_802_11_AP_FLAGS_PRIVACY 0x00000001

/* NM80211ApSecurityFlags -- the bits NetworkManager reports in an
 * access point's WpaFlags (WPA1 RSN-less IEs) and RsnFlags (RSN/WPA2+
 * IEs). We only need the key-management bits to tell the generations
 * apart; the cipher bits (TKIP/CCMP/...) are there for completeness of
 * the mask but aren't inspected. */
#define NM_802_11_AP_SEC_KEY_MGMT_PSK            0x00000100
#define NM_802_11_AP_SEC_KEY_MGMT_802_1X         0x00000200
#define NM_802_11_AP_SEC_KEY_MGMT_SAE            0x00000400 /* WPA3-Personal */
#define NM_802_11_AP_SEC_KEY_MGMT_OWE            0x00000800 /* Enhanced Open */
#define NM_802_11_AP_SEC_KEY_MGMT_OWE_TM         0x00001000
#define NM_802_11_AP_SEC_KEY_MGMT_EAP_SUITE_B_192 0x00002000 /* WPA3-Enterprise */

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

    /* Passed to every asynchronous D-Bus call we make and cancelled in
     * extras_menu_network_free(). Every async callback in this file
     * checks for cancellation *before* touching `network` (see
     * call_was_cancelled()): a cancelled call still invokes its
     * callback, just with G_IO_ERROR_CANCELLED, and by then the
     * network struct is already gone. */
    GCancellable *cancellable;

    /* Incremented every time a fresh AP list is requested. Each fetch
     * remembers the number it started with, and a fetch whose number
     * is no longer current when it finishes is dropped -- otherwise two
     * overlapping fetches could finish out of order and the older,
     * staler result would overwrite the newer one. */
    guint ap_generation;

    /* Pending "refresh the AP list shortly" timer (0 if none).
     * NetworkManager emits bursts of PropertiesChanged/AccessPoint*
     * signals during a scan; coalescing them into one refresh avoids
     * rebuilding the whole list once per signal. */
    guint ap_refresh_source_id;

    /* Connection attempts currently being watched
     * (ActiveConnectionWatchCtx*), so extras_menu_network_free() can
     * tear down their signal subscriptions and timeouts. */
    GSList *watches;

    /* SSID (owned gchar*) -> ExtrasMenuApSecurity (as a pointer) for the
     * networks in the most recent AP list. Lets connect() pick the right
     * kind of profile (WPA2-PSK, WPA3-SAE, WEP, OWE, ...) without
     * widening the public API. */
    GHashTable *security_by_ssid;
};

/* A cancelled GDBus call still runs its callback, with this error.
 * When it happens, extras_menu_network_free() has already run, so the
 * callback must only release its own context and return -- it must not
 * touch the network struct or call any user callback. */
static gboolean
call_was_cancelled(const GError *error)
{
    return error != NULL && g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
}

/* --- helpers -------------------------------------------------------- */

static void
free_ap_array(ExtrasMenuAccessPoint *aps, guint count)
{
    for (guint i = 0; i < count; i++)
    {
        g_free(aps[i].ssid);
        g_free(aps[i].bssid);
    }
    g_free(aps);
}

/* Works out which security scheme an AP advertises from the three
 * bitfields NetworkManager exposes:
 *   flags     -- general AP capabilities; PRIVACY means "encrypted"
 *                without saying how (the only signal WEP gives us)
 *   wpa_flags -- key management offered via the older WPA1 IEs
 *   rsn_flags -- key management offered via RSN (WPA2 and later)
 * The generations are told apart by key-management bits rather than
 * ciphers: SAE means WPA3-Personal, PSK under RSN means WPA2, PSK
 * under the WPA1 IEs means WPA1, and an AP advertising both PSK and
 * SAE under RSN is in WPA2/WPA3 transitional mode. */
static ExtrasMenuApSecurity
derive_ap_security(guint32 flags, guint32 wpa_flags, guint32 rsn_flags)
{
    const guint32 enterprise_bits = NM_802_11_AP_SEC_KEY_MGMT_802_1X |
                                     NM_802_11_AP_SEC_KEY_MGMT_EAP_SUITE_B_192;

    if ((wpa_flags & enterprise_bits) != 0 || (rsn_flags & enterprise_bits) != 0)
        return EXTRAS_MENU_AP_SECURITY_ENTERPRISE;

    gboolean rsn_has_sae = (rsn_flags & NM_802_11_AP_SEC_KEY_MGMT_SAE) != 0;
    gboolean rsn_has_psk = (rsn_flags & NM_802_11_AP_SEC_KEY_MGMT_PSK) != 0;
    gboolean wpa_has_psk = (wpa_flags & NM_802_11_AP_SEC_KEY_MGMT_PSK) != 0;

    if (rsn_has_sae && rsn_has_psk)
        return EXTRAS_MENU_AP_SECURITY_WPA2_WPA3;
    if (rsn_has_sae)
        return EXTRAS_MENU_AP_SECURITY_WPA3;
    if (rsn_has_psk && wpa_has_psk)
        return EXTRAS_MENU_AP_SECURITY_WPA_WPA2;
    if (rsn_has_psk)
        return EXTRAS_MENU_AP_SECURITY_WPA2;
    if (wpa_has_psk)
        return EXTRAS_MENU_AP_SECURITY_WPA;

    if ((rsn_flags & (NM_802_11_AP_SEC_KEY_MGMT_OWE | NM_802_11_AP_SEC_KEY_MGMT_OWE_TM)) != 0)
        return EXTRAS_MENU_AP_SECURITY_OWE;

    /* No WPA/RSN key management at all, but the AP still claims to be
     * encrypted -- that only leaves WEP. */
    if ((flags & NM_802_11_AP_FLAGS_PRIVACY) != 0)
        return EXTRAS_MENU_AP_SECURITY_WEP;

    if (wpa_flags == 0 && rsn_flags == 0)
        return EXTRAS_MENU_AP_SECURITY_OPEN;

    return EXTRAS_MENU_AP_SECURITY_UNKNOWN;
}

const gchar *
extras_menu_ap_security_to_string(ExtrasMenuApSecurity security)
{
    switch (security)
    {
        case EXTRAS_MENU_AP_SECURITY_OPEN:       return "Open";
        case EXTRAS_MENU_AP_SECURITY_OWE:        return "Enhanced Open";
        case EXTRAS_MENU_AP_SECURITY_WEP:        return "WEP";
        case EXTRAS_MENU_AP_SECURITY_WPA:        return "WPA";
        case EXTRAS_MENU_AP_SECURITY_WPA2:       return "WPA2";
        case EXTRAS_MENU_AP_SECURITY_WPA_WPA2:   return "WPA/WPA2";
        case EXTRAS_MENU_AP_SECURITY_WPA3:       return "WPA3";
        case EXTRAS_MENU_AP_SECURITY_WPA2_WPA3:  return "WPA2/WPA3";
        case EXTRAS_MENU_AP_SECURITY_ENTERPRISE: return "Enterprise (802.1X)";
        case EXTRAS_MENU_AP_SECURITY_UNKNOWN:
        default:                                  return "Unknown";
    }
}

/* Whether connecting to a network with this security scheme needs a
 * password from the user. `fallback` is the answer to give when the
 * scheme couldn't be determined.
 *
 * This deliberately goes by the derived scheme rather than "are any
 * WPA/RSN flags set": Enhanced Open (OWE) sets RSN flags but needs no
 * password, while WEP sets neither WPA nor RSN flags yet does need one. */
static gboolean
security_needs_password(ExtrasMenuApSecurity security, gboolean fallback)
{
    switch (security)
    {
        case EXTRAS_MENU_AP_SECURITY_OPEN:
        case EXTRAS_MENU_AP_SECURITY_OWE:
            return FALSE;
        case EXTRAS_MENU_AP_SECURITY_UNKNOWN:
            return fallback;
        default:
            return TRUE;
    }
}

/* Security scheme of the network with this SSID in the most recent AP
 * list, or UNKNOWN if it isn't in it (hidden, out of range, ...). */
static ExtrasMenuApSecurity
lookup_known_security(ExtrasMenuNetwork *network, const gchar *ssid)
{
    gpointer value = NULL;

    if (network->security_by_ssid != NULL &&
        g_hash_table_lookup_extended(network->security_by_ssid, ssid, NULL, &value))
    {
        return (ExtrasMenuApSecurity) GPOINTER_TO_UINT(value);
    }

    return EXTRAS_MENU_AP_SECURITY_UNKNOWN;
}

/* Sort predicate for the access point list: the currently-active
 * network always sorts first (so the user sees what they're connected
 * to without having to scroll/scan), everything else by signal
 * strength descending. Used by the insertion sort in
 * finish_ap_fetch_if_done() below. */
static gboolean
ap_sorts_before(const ExtrasMenuAccessPoint *a, const ExtrasMenuAccessPoint *b)
{
    if (a->is_active != b->is_active)
        return a->is_active;

    return a->strength > b->strength;
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
    guint generation;      /* value of network->ap_generation when this fetch started */
    gboolean aborted;      /* a call was cancelled: network is gone, don't touch it */
} ApFetchContext;

static void
free_ap_fetch_context(ApFetchContext *ctx)
{
    for (guint j = 0; j < ctx->results->len; j++)
    {
        ExtrasMenuAccessPoint *ap = g_ptr_array_index(ctx->results, j);
        g_free(ap->ssid);
        g_free(ap->bssid);
        g_free(ap);
    }
    g_ptr_array_free(ctx->results, TRUE);
    g_free(ctx->active_ap_path);
    g_free(ctx);
}

static void
finish_ap_fetch_if_done(ApFetchContext *ctx)
{
    if (ctx->pending_count > 0)
        return;

    /* Cancelled (network already freed), or superseded by a newer
     * fetch that was started while this one was still in flight --
     * either way, drop the results without reporting them. */
    if (ctx->aborted || ctx->generation != ctx->network->ap_generation)
    {
        free_ap_fetch_context(ctx);
        return;
    }

    /* Collapse duplicate SSIDs (the same network broadcasting on
     * multiple APs/channels) down to their strongest instance. The
     * AP we're actually connected to always wins over a stronger
     * sibling: otherwise, on a mesh/repeater setup, the strongest AP
     * could replace the active one and the connected network would
     * stop showing as connected. */
    GHashTable *best_by_ssid = g_hash_table_new(g_str_hash, g_str_equal);

    for (guint i = 0; i < ctx->results->len; i++)
    {
        ExtrasMenuAccessPoint *ap = g_ptr_array_index(ctx->results, i);
        if (ap->ssid == NULL || ap->ssid[0] == '\0')
            continue; /* hidden/unnamed AP -- nothing sensible to show */

        ExtrasMenuAccessPoint *existing = g_hash_table_lookup(best_by_ssid, ap->ssid);
        if (existing == NULL ||
            ap->is_active ||
            (!existing->is_active && ap->strength > existing->strength))
        {
            g_hash_table_insert(best_by_ssid, ap->ssid, ap);
        }
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
        final_aps[i].security = src->security;
        final_aps[i].bssid = src->bssid != NULL ? g_strdup(src->bssid) : NULL;
        final_aps[i].frequency = src->frequency;
        final_aps[i].max_bitrate = src->max_bitrate;
    }

    /* Simple insertion sort: the currently-active network (if any)
     * always sorts first, everything else by strength descending --
     * lists here are small (a handful to a few dozen networks), so
     * O(n^2) is fine and keeps this dependency-free. */
    for (guint a = 1; a < final_count; a++)
    {
        ExtrasMenuAccessPoint key = final_aps[a];
        gint b = (gint) a - 1;
        while (b >= 0 && ap_sorts_before(&key, &final_aps[b]))
        {
            final_aps[b + 1] = final_aps[b];
            b--;
        }
        final_aps[b + 1] = key;
    }

    g_list_free(unique);
    g_hash_table_destroy(best_by_ssid);

    /* Remember each network's security scheme, so a later connect()
     * knows what kind of profile to create. */
    g_hash_table_remove_all(ctx->network->security_by_ssid);
    for (guint k = 0; k < final_count; k++)
    {
        g_hash_table_insert(ctx->network->security_by_ssid,
                            g_strdup(final_aps[k].ssid),
                            GUINT_TO_POINTER((guint) final_aps[k].security));
    }

    if (ctx->network->list_changed_callback != NULL)
    {
        ctx->network->list_changed_callback(TRUE, final_aps, final_count,
                                             ctx->network->list_changed_user_data);
    }

    free_ap_array(final_aps, final_count);
    free_ap_fetch_context(ctx);
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
        GVariant *flags_v = g_variant_lookup_value(props, "Flags", G_VARIANT_TYPE_UINT32);
        GVariant *wpa_flags_v = g_variant_lookup_value(props, "WpaFlags", G_VARIANT_TYPE_UINT32);
        GVariant *rsn_flags_v = g_variant_lookup_value(props, "RsnFlags", G_VARIANT_TYPE_UINT32);
        GVariant *bssid_v = g_variant_lookup_value(props, "HwAddress", G_VARIANT_TYPE_STRING);
        GVariant *frequency_v = g_variant_lookup_value(props, "Frequency", G_VARIANT_TYPE_UINT32);
        GVariant *bitrate_v = g_variant_lookup_value(props, "MaxBitrate", G_VARIANT_TYPE_UINT32);

        ExtrasMenuAccessPoint *ap = g_new0(ExtrasMenuAccessPoint, 1);
        ap->ssid = decode_ssid(ssid_v);
        ap->strength = strength_v != NULL ? (gint8) g_variant_get_byte(strength_v) : 0;

        guint32 flags = flags_v != NULL ? g_variant_get_uint32(flags_v) : 0;
        guint32 wpa_flags = wpa_flags_v != NULL ? g_variant_get_uint32(wpa_flags_v) : 0;
        guint32 rsn_flags = rsn_flags_v != NULL ? g_variant_get_uint32(rsn_flags_v) : 0;
        ap->security = derive_ap_security(flags, wpa_flags, rsn_flags);
        ap->secured = security_needs_password(ap->security,
                                               wpa_flags != 0 || rsn_flags != 0);

        ap->bssid = bssid_v != NULL ? g_variant_dup_string(bssid_v, NULL) : NULL;
        ap->frequency = frequency_v != NULL ? g_variant_get_uint32(frequency_v) : 0;
        ap->max_bitrate = bitrate_v != NULL ? g_variant_get_uint32(bitrate_v) : 0;

        ap->is_active = (ctx->active_ap_path != NULL &&
                          g_strcmp0(call_ctx->ap_object_path, ctx->active_ap_path) == 0);

        g_ptr_array_add(ctx->results, ap);

        if (ssid_v != NULL) g_variant_unref(ssid_v);
        if (strength_v != NULL) g_variant_unref(strength_v);
        if (flags_v != NULL) g_variant_unref(flags_v);
        if (wpa_flags_v != NULL) g_variant_unref(wpa_flags_v);
        if (rsn_flags_v != NULL) g_variant_unref(rsn_flags_v);
        if (bssid_v != NULL) g_variant_unref(bssid_v);
        if (frequency_v != NULL) g_variant_unref(frequency_v);
        if (bitrate_v != NULL) g_variant_unref(bitrate_v);
        g_variant_unref(props);
        g_variant_unref(reply);
    }
    else if (call_was_cancelled(error))
    {
        ctx->aborted = TRUE; /* network is gone -- see finish_ap_fetch_if_done() */
    }
    g_clear_error(&error);

    ctx->pending_count--;
    finish_ap_fetch_if_done(ctx);

    g_free(call_ctx->ap_object_path);
    g_free(call_ctx);
}

/* Identifies one request for a fresh AP list: which network it's for
 * and which generation it is (see ExtrasMenuNetwork::ap_generation). */
typedef struct
{
    ExtrasMenuNetwork *network;
    guint generation;
} ScanRequestCtx;

static void
on_device_props_for_scan_finished(GObject *source, GAsyncResult *result, gpointer user_data)
{
    ScanRequestCtx *request = user_data;
    GError *error = NULL;

    GVariant *reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), result, &error);
    if (reply == NULL && call_was_cancelled(error))
    {
        g_clear_error(&error);
        g_free(request);
        return;
    }

    ExtrasMenuNetwork *network = request->network;
    guint generation = request->generation;
    g_free(request);

    if (reply == NULL)
    {
        g_clear_error(&error);
        if (generation == network->ap_generation && network->list_changed_callback != NULL)
            network->list_changed_callback(FALSE, NULL, 0, network->list_changed_user_data);
        return;
    }

    /* A newer fetch was started while this one was in flight; it will
     * report the up-to-date list, so don't bother with this one. */
    if (generation != network->ap_generation)
    {
        g_variant_unref(reply);
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
    ctx->generation = generation;
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
            -1, network->cancellable,
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

    /* Invalidates any fetch still in flight (see ap_generation). */
    network->ap_generation++;

    ScanRequestCtx *request = g_new0(ScanRequestCtx, 1);
    request->network = network;
    request->generation = network->ap_generation;

    g_dbus_connection_call(
        network->system_bus,
        NM_BUS_NAME,
        network->wifi_device_path,
        "org.freedesktop.DBus.Properties",
        "GetAll",
        g_variant_new("(s)", NM_WIRELESS_IFACE),
        G_VARIANT_TYPE("(a{sv})"),
        G_DBUS_CALL_FLAGS_NONE,
        -1, network->cancellable,
        on_device_props_for_scan_finished, request);
}

/* --- live change notifications ---------------------------------------- */

#define AP_REFRESH_DEBOUNCE_MS 200

static gboolean
on_ap_refresh_timeout(gpointer user_data)
{
    ExtrasMenuNetwork *network = user_data;

    network->ap_refresh_source_id = 0; /* returning G_SOURCE_REMOVE removes it */
    request_ap_list(network);
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
    start_primary_connection_tracking(network);
}

/* --- connecting / disconnecting ------------------------------------------- */

/* NMActiveConnectionState -- the states StateChanged reports. We only
 * care about telling "it worked", "it's still trying" and "it failed"
 * apart. */
#define NM_ACTIVE_CONNECTION_STATE_ACTIVATING   1
#define NM_ACTIVE_CONNECTION_STATE_ACTIVATED    2
#define NM_ACTIVE_CONNECTION_STATE_DEACTIVATING 3
#define NM_ACTIVE_CONNECTION_STATE_DEACTIVATED  4

#define CONNECTION_FAILED_MESSAGE \
    "Connection failed (incorrect password or network unavailable)"

typedef struct
{
    ExtrasMenuNetwork *network; /* only valid while !finished */
    ExtrasMenuNetworkConnectResultFunc callback;
    gpointer user_data;
    guint subscription_id;
    guint timeout_source_id;
    gboolean finished; /* guards against StateChanged firing again after we've already reported */

    /* Profile that THIS attempt created via AddAndActivateConnection,
     * or NULL if an existing saved profile was reused. If the attempt
     * fails, a profile we created is deleted again (see
     * finish_active_connection_watch()); one that was already there is
     * never touched. */
    gchar *created_profile_path;

    /* One reference is held by network->watches until the watch
     * finishes; the one-off State read in watch_active_connection()
     * holds another until it completes. */
    guint ref_count;
} ActiveConnectionWatchCtx;

static void
watch_unref(ActiveConnectionWatchCtx *watch)
{
    if (--watch->ref_count > 0)
        return;

    g_free(watch->created_profile_path);
    g_free(watch);
}

/* Drops the watch's signal subscription and timeout, if still active. */
static void
stop_watch_sources(ActiveConnectionWatchCtx *watch)
{
    if (watch->subscription_id != 0)
    {
        g_dbus_connection_signal_unsubscribe(watch->network->system_bus, watch->subscription_id);
        watch->subscription_id = 0;
    }
    if (watch->timeout_source_id != 0)
    {
        g_source_remove(watch->timeout_source_id);
        watch->timeout_source_id = 0;
    }
}

/* Carries a failure report across the deletion of the failed attempt's
 * profile (see finish_active_connection_watch()). */
typedef struct
{
    ExtrasMenuNetworkConnectResultFunc callback;
    gpointer user_data;
    gchar *error_message;
} FailedProfileCleanupCtx;

static void
on_failed_profile_deleted(GObject *source, GAsyncResult *result, gpointer user_data)
{
    FailedProfileCleanupCtx *cleanup = user_data;
    GError *error = NULL;

    GVariant *reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), result, &error);
    gboolean cancelled = (reply == NULL && call_was_cancelled(error));

    if (reply != NULL)
        g_variant_unref(reply);
    g_clear_error(&error); /* a failed cleanup isn't worth surfacing; the connect failure is what matters */

    if (!cancelled && cleanup->callback != NULL)
        cleanup->callback(FALSE, cleanup->error_message, cleanup->user_data);

    g_free(cleanup->error_message);
    g_free(cleanup);
}

static void
finish_active_connection_watch(ActiveConnectionWatchCtx *watch, gboolean success, const gchar *error_message)
{
    if (watch->finished)
        return;
    watch->finished = TRUE;

    ExtrasMenuNetwork *network = watch->network;

    stop_watch_sources(watch);
    network->watches = g_slist_remove(network->watches, watch);

    if (!success && watch->created_profile_path != NULL)
    {
        /* This attempt created the profile and it never worked. If it
         * were left behind, the next attempt would find it, "reuse" it
         * and fail silently with the same wrong password forever --
         * without ever asking again -- until the user thought to
         * "Forget" the network. So delete it first, and only report the
         * failure once that's done, so a retry that follows straight
         * away (the caller prompting for a password again) can't see
         * the stale profile. */
        FailedProfileCleanupCtx *cleanup = g_new0(FailedProfileCleanupCtx, 1);
        cleanup->callback = watch->callback;
        cleanup->user_data = watch->user_data;
        cleanup->error_message = g_strdup(error_message != NULL ? error_message : CONNECTION_FAILED_MESSAGE);

        g_dbus_connection_call(
            network->system_bus,
            NM_BUS_NAME,
            watch->created_profile_path,
            NM_SETTINGS_CONNECTION_IFACE,
            "Delete",
            NULL,
            NULL,
            G_DBUS_CALL_FLAGS_NONE,
            -1, network->cancellable,
            on_failed_profile_deleted, cleanup);
    }
    else if (watch->callback != NULL)
    {
        watch->callback(success, error_message, watch->user_data);
    }

    watch_unref(watch); /* the reference network->watches held */
}

static void
on_active_connection_state_changed(GDBusConnection *connection, const gchar *sender_name,
                                    const gchar *object_path, const gchar *interface_name,
                                    const gchar *signal_name, GVariant *parameters,
                                    gpointer user_data)
{
    ActiveConnectionWatchCtx *watch = user_data;
    (void) connection;
    (void) sender_name;
    (void) object_path;
    (void) interface_name;
    (void) signal_name;

    guint32 state = 0, reason = 0;
    g_variant_get(parameters, "(uu)", &state, &reason);

    if (state == NM_ACTIVE_CONNECTION_STATE_ACTIVATED)
    {
        finish_active_connection_watch(watch, TRUE, NULL);
    }
    else if (state == NM_ACTIVE_CONNECTION_STATE_DEACTIVATED)
    {
        /* This is the state a failed WPA handshake (wrong password)
         * actually lands in -- the D-Bus call to start activating
         * still returns success, since starting the attempt did
         * succeed; the failure only shows up here, once NM gives up
         * and tears the attempt back down. reason codes are libnm's
         * NMActiveConnectionStateReason -- we don't decode the exact
         * one, just surface that it failed, since the reasons that
         * matter to a user ("wrong password", "network out of range")
         * aren't reliably distinguishable from the reason code alone
         * across NetworkManager versions. */
        (void) reason;
        finish_active_connection_watch(watch, FALSE, CONNECTION_FAILED_MESSAGE);
    }
    /* ACTIVATING and DEACTIVATING are intermediate -- keep waiting. */
}

/* A connection attempt that never reaches ACTIVATED or DEACTIVATED
 * (rare, but possible if NetworkManager wedges) would otherwise leave
 * the caller's "Connecting..." UI spinning forever -- this bounds the
 * wait. */
#define ACTIVE_CONNECTION_WATCH_TIMEOUT_SECONDS 25

static gboolean
on_active_connection_watch_timeout(gpointer user_data)
{
    ActiveConnectionWatchCtx *watch = user_data;
    watch->timeout_source_id = 0; /* this source is about to be removed by returning FALSE */
    finish_active_connection_watch(watch, FALSE, "Connection attempt timed out");
    return G_SOURCE_REMOVE;
}

/* Subscribes to the given active-connection object's StateChanged
 * signal and reports the real outcome (success once ACTIVATED,
 * failure once DEACTIVATED or on timeout) via callback -- this is what
 * lets us tell an actually-successful connection apart from a WPA
 * handshake that fails after the initial D-Bus call already returned
 * "accepted". */
/* Result of the one-off State read in watch_active_connection(). */
static void
on_watch_initial_state_finished(GObject *source, GAsyncResult *result, gpointer user_data)
{
    ActiveConnectionWatchCtx *watch = user_data;
    GError *error = NULL;

    GVariant *reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), result, &error);
    g_clear_error(&error); /* cancelled or failed: nothing to do, StateChanged/timeout still cover us */

    if (reply != NULL)
    {
        GVariant *boxed = NULL;
        g_variant_get(reply, "(v)", &boxed);
        guint32 state = g_variant_get_uint32(boxed);
        g_variant_unref(boxed);
        g_variant_unref(reply);

        /* finish_active_connection_watch() ignores us if the watch
         * already finished through a signal in the meantime. */
        if (state == NM_ACTIVE_CONNECTION_STATE_ACTIVATED)
            finish_active_connection_watch(watch, TRUE, NULL);
        else if (state == NM_ACTIVE_CONNECTION_STATE_DEACTIVATED)
            finish_active_connection_watch(watch, FALSE, CONNECTION_FAILED_MESSAGE);
    }

    watch_unref(watch);
}

static void
watch_active_connection(ExtrasMenuNetwork *network,
                         const gchar *active_connection_path,
                         const gchar *created_profile_path,
                         ExtrasMenuNetworkConnectResultFunc callback,
                         gpointer user_data)
{
    ActiveConnectionWatchCtx *watch = g_new0(ActiveConnectionWatchCtx, 1);
    watch->network = network;
    watch->callback = callback;
    watch->user_data = user_data;
    watch->finished = FALSE;
    watch->created_profile_path = g_strdup(created_profile_path);
    watch->ref_count = 1; /* owned by network->watches */
    network->watches = g_slist_prepend(network->watches, watch);

    watch->subscription_id = g_dbus_connection_signal_subscribe(
        network->system_bus, NM_BUS_NAME,
        NM_CONNECTION_ACTIVE_IFACE, "StateChanged",
        active_connection_path, NULL, G_DBUS_SIGNAL_FLAGS_NONE,
        on_active_connection_state_changed, watch, NULL);

    watch->timeout_source_id = g_timeout_add_seconds(
        ACTIVE_CONNECTION_WATCH_TIMEOUT_SECONDS, on_active_connection_watch_timeout, watch);

    /* StateChanged only reports transitions that happen *after* we
     * subscribed, but the activation call has already returned by now --
     * on a fast connection (or a quick failure) the active connection
     * may have reached its final state in between, and we'd wait the
     * full timeout for a signal that already came and went. So read the
     * current state once as well. */
    watch->ref_count++;
    g_dbus_connection_call(
        network->system_bus,
        NM_BUS_NAME,
        active_connection_path,
        "org.freedesktop.DBus.Properties",
        "Get",
        g_variant_new("(ss)", NM_CONNECTION_ACTIVE_IFACE, "State"),
        G_VARIANT_TYPE("(v)"),
        G_DBUS_CALL_FLAGS_NONE,
        -1, network->cancellable,
        on_watch_initial_state_finished, watch);
}

typedef struct
{
    ExtrasMenuNetwork *network;
    ExtrasMenuNetworkConnectResultFunc callback;
    gpointer user_data;
} ConnectResultCtx;

/* Fired once the initial ActivateConnection/AddAndActivateConnection
 * D-Bus call itself completes. This only tells us whether NetworkManager
 * *accepted the request* to start connecting -- not whether the
 * connection actually succeeds (see watch_active_connection() above,
 * which is what reports the real outcome). On a D-Bus-level failure
 * here (e.g. malformed profile), we report immediately since there's
 * no active connection object to watch in that case. */
static void
on_connect_finished(GObject *source, GAsyncResult *result, gpointer user_data)
{
    ConnectResultCtx *ctx = user_data;
    GError *error = NULL;

    GVariant *reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), result, &error);

    if (reply == NULL && call_was_cancelled(error))
    {
        g_clear_error(&error);
        g_free(ctx);
        return;
    }

    if (reply == NULL)
    {
        if (ctx->callback != NULL)
            ctx->callback(FALSE, error != NULL ? error->message : "Unknown error", ctx->user_data);
        g_clear_error(&error);
        g_free(ctx);
        return;
    }

    /* ActivateConnection returns "(o)" -- just the active connection
     * path. AddAndActivateConnection returns "(oo)" -- the newly
     * created profile's path first, then the active connection path.
     * The active connection path we actually want to watch is always
     * the LAST element, not always index 0 -- using a fixed index 0
     * here previously grabbed the new profile's path instead of the
     * active connection's path when called via
     * AddAndActivateConnection, which meant we were subscribing to
     * StateChanged on the wrong object entirely (a static Settings
     * profile never emits it), so failures were never detected. */
    gsize n_children = g_variant_n_children(reply);
    GVariant *active_path_v = g_variant_get_child_value(reply, n_children - 1);
    const gchar *active_path = g_variant_get_string(active_path_v, NULL);

    /* Only AddAndActivateConnection's "(oo)" reply names a profile that
     * THIS call just created (its first element). ActivateConnection's
     * "(o)" reply means an existing profile was reused, which must
     * never be deleted if the attempt fails. */
    GVariant *created_profile_v = n_children >= 2 ? g_variant_get_child_value(reply, 0) : NULL;
    const gchar *created_profile_path =
        created_profile_v != NULL ? g_variant_get_string(created_profile_v, NULL) : NULL;

    watch_active_connection(ctx->network, active_path, created_profile_path,
                            ctx->callback, ctx->user_data);

    if (created_profile_v != NULL)
        g_variant_unref(created_profile_v);
    g_variant_unref(active_path_v);
    g_variant_unref(reply);
    g_free(ctx);
}

/* Builds a minimal NetworkManager connection settings dict for a given
 * SSID/password, suitable for AddAndActivateConnection. Only used as a
 * fallback when no existing saved profile for this SSID was found --
 * see find_existing_connection_for_ssid() below, which is tried
 * first. */
static GVariant *
build_connection_settings(const gchar *ssid, const gchar *password, ExtrasMenuApSecurity security)
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

    /* [802-11-wireless-security] -- which key-mgmt to ask for depends on
     * what the AP advertises; a profile whose key-mgmt doesn't match
     * can never complete the handshake:
     *   OPEN                 no section at all
     *   OWE (Enhanced Open)  key-mgmt "owe", no password
     *   WPA3-Personal        key-mgmt "sae" (the password goes in "psk")
     *   WEP                  key-mgmt "none" + a WEP key
     *   everything else      "wpa-psk" -- WPA, WPA2, WPA/WPA2 and the
     *                        WPA2/WPA3 transitional mode (which accepts
     *                        WPA2 clients, so it needs no special case)
     * UNKNOWN (network not in the last scan, e.g. a hidden one) falls
     * into the last bucket when a password was given, as before. */
    gboolean have_password = (password != NULL && password[0] != '\0');

    if (security == EXTRAS_MENU_AP_SECURITY_OWE)
    {
        GVariantBuilder security_section;
        g_variant_builder_init(&security_section, G_VARIANT_TYPE("a{sv}"));
        g_variant_builder_add(&security_section, "{sv}", "key-mgmt",
                               g_variant_new_string("owe"));
        g_variant_builder_add(&connection_builder, "{s@a{sv}}", "802-11-wireless-security",
                               g_variant_builder_end(&security_section));
    }
    else if (have_password && security == EXTRAS_MENU_AP_SECURITY_WEP)
    {
        GVariantBuilder security_section;
        g_variant_builder_init(&security_section, G_VARIANT_TYPE("a{sv}"));
        g_variant_builder_add(&security_section, "{sv}", "key-mgmt",
                               g_variant_new_string("none"));
        g_variant_builder_add(&security_section, "{sv}", "wep-key0",
                               g_variant_new_string(password));
        /* 1 = NM_WEP_KEY_TYPE_KEY: the text is the key itself (5/13
         * ASCII characters or 10/26 hex digits), not a passphrase. */
        g_variant_builder_add(&security_section, "{sv}", "wep-key-type",
                               g_variant_new_uint32(1));
        g_variant_builder_add(&security_section, "{sv}", "auth-alg",
                               g_variant_new_string("open"));
        g_variant_builder_add(&connection_builder, "{s@a{sv}}", "802-11-wireless-security",
                               g_variant_builder_end(&security_section));
    }
    else if (have_password)
    {
        GVariantBuilder security_section;
        g_variant_builder_init(&security_section, G_VARIANT_TYPE("a{sv}"));
        g_variant_builder_add(&security_section, "{sv}", "key-mgmt",
                               g_variant_new_string(security == EXTRAS_MENU_AP_SECURITY_WPA3
                                                        ? "sae" : "wpa-psk"));
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
    gboolean requires_password; /* if TRUE and password is empty, refuse to create a passwordless profile */
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
    result_ctx->network = ctx->network;
    result_ctx->callback = ctx->result_callback;
    result_ctx->user_data = ctx->result_user_data;

    if (existing_profile_path != NULL)
    {
        /* Reactivate the saved profile as-is -- its stored password
         * (if any) is used by NetworkManager itself, so the user isn't
         * prompted again for a network they've already connected to
         * before.
         *
         * Device path is deliberately "/" (unspecified) rather than
         * our known Wi-Fi device path: some saved profiles carry an
         * interface-name constraint from whenever they were first
         * created (e.g. a different interface name than the current
         * wlan0), and forcing our device path against a mismatched
         * profile makes NetworkManager reject the activation outright
         * ("mismatching interface name") instead of resolving it
         * itself. Passing "/" lets NM pick the right device using the
         * profile's own settings. */
        g_dbus_connection_call(
            ctx->network->system_bus,
            NM_BUS_NAME,
            NM_OBJ_PATH,
            NM_IFACE,
            "ActivateConnection",
            g_variant_new("(ooo)", existing_profile_path, "/", "/"),
            G_VARIANT_TYPE("(o)"),
            G_DBUS_CALL_FLAGS_NONE,
            -1, ctx->network->cancellable,
            on_connect_finished, result_ctx);
    }
    else if (lookup_known_security(ctx->network, ctx->ssid) == EXTRAS_MENU_AP_SECURITY_ENTERPRISE)
    {
        /* 802.1X needs a username, certificates and so on that we have
         * no way to collect, and a password-only profile could never
         * authenticate. An already-saved profile (handled above) still
         * works; for a new one, say what's needed rather than creating
         * a profile that's doomed to fail. */
        if (ctx->result_callback != NULL)
        {
            ctx->result_callback(FALSE,
                                  "This network uses enterprise (802.1X) authentication. "
                                  "Set it up once in NetworkManager first, then it can be used from here.",
                                  ctx->result_user_data);
        }
        g_free(result_ctx);
    }
    else if (security_needs_password(lookup_known_security(ctx->network, ctx->ssid),
                                      ctx->requires_password) &&
             (ctx->password == NULL || ctx->password[0] == '\0'))
    {
        /* No saved profile, this network needs a password, and we
         * don't have one -- refuse rather than creating a passwordless
         * profile. That would "succeed" at the D-Bus level (profile
         * created, activation request accepted) while the actual WPA
         * handshake silently fails in the background, leaving the UI
         * with no error to show and the user wondering why nothing
         * happened. Report a clear, specific failure instead so the
         * caller (on_connect_result in extras-menu.c) can fall back to
         * prompting for a password. */
        if (ctx->result_callback != NULL)
        {
            ctx->result_callback(FALSE, "No saved password for this network", ctx->result_user_data);
        }
        g_free(result_ctx);
    }
    else
    {
        GVariant *connection_settings = build_connection_settings(
            ctx->ssid, ctx->password, lookup_known_security(ctx->network, ctx->ssid));

        g_dbus_connection_call(
            ctx->network->system_bus,
            NM_BUS_NAME,
            NM_OBJ_PATH,
            NM_IFACE,
            "AddAndActivateConnection",
            g_variant_new("(@a{sa{sv}}oo)", connection_settings, ctx->network->wifi_device_path, "/"),
            G_VARIANT_TYPE("(oo)"),
            G_DBUS_CALL_FLAGS_NONE,
            -1, ctx->network->cancellable,
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
    if (reply == NULL && call_was_cancelled(error))
    {
        g_clear_error(&error);
        free_find_profile_ctx(ctx);
        return;
    }

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
        -1, ctx->network->cancellable,
        on_profile_settings_finished, ctx);
}

static void
on_list_connections_finished(GObject *source, GAsyncResult *result, gpointer user_data)
{
    FindProfileCtx *ctx = user_data;
    GError *error = NULL;

    GVariant *reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), result, &error);
    if (reply == NULL && call_was_cancelled(error))
    {
        g_clear_error(&error);
        free_find_profile_ctx(ctx);
        return;
    }

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
extras_menu_network_connect(ExtrasMenuNetwork *network,
                             const gchar *ssid,
                             const gchar *password,
                             gboolean requires_password,
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
     * if none is found -- unless requires_password is set and we have
     * no password, in which case proceed_with_connect() refuses rather
     * than creating a doomed passwordless profile. */
    FindProfileCtx *ctx = g_new0(FindProfileCtx, 1);
    ctx->network = network;
    ctx->ssid = g_strdup(ssid);
    ctx->password = password != NULL ? g_strdup(password) : NULL;
    ctx->requires_password = requires_password;
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
        -1, network->cancellable,
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

/* --- forgetting a network (deleting its saved profiles) ---------------- */

typedef struct
{
    ExtrasMenuNetwork *network;
    gchar *ssid;
    ExtrasMenuNetworkConnectResultFunc result_callback;
    gpointer result_user_data;

    gchar **profile_paths; /* NULL-terminated */
    guint index;

    guint deleted_count;
    gboolean had_error;
    gchar *first_error; /* owned; first failure's message, for reporting */
} ForgetCtx;

static void forget_check_next(ForgetCtx *ctx);

static void
free_forget_ctx(ForgetCtx *ctx)
{
    g_free(ctx->ssid);
    g_free(ctx->first_error);
    g_strfreev(ctx->profile_paths);
    g_free(ctx);
}

static void
finish_forget(ForgetCtx *ctx)
{
    if (ctx->result_callback != NULL)
    {
        if (ctx->deleted_count == 0 && !ctx->had_error)
        {
            /* Nothing matched -- treat as success rather than an error:
             * the end state the user asked for ("this network isn't
             * saved anymore") is already true. */
            ctx->result_callback(TRUE, NULL, ctx->result_user_data);
        }
        else if (ctx->had_error)
        {
            ctx->result_callback(FALSE,
                                  ctx->first_error != NULL ? ctx->first_error : "Unknown error",
                                  ctx->result_user_data);
        }
        else
        {
            ctx->result_callback(TRUE, NULL, ctx->result_user_data);
        }
    }

    free_forget_ctx(ctx);
}

static void
on_profile_deleted(GObject *source, GAsyncResult *result, gpointer user_data)
{
    ForgetCtx *ctx = user_data;
    GError *error = NULL;

    GVariant *reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), result, &error);
    if (reply == NULL && call_was_cancelled(error))
    {
        g_clear_error(&error);
        free_forget_ctx(ctx);
        return;
    }

    if (reply != NULL)
    {
        ctx->deleted_count++;
        g_variant_unref(reply);
    }
    else
    {
        ctx->had_error = TRUE;
        if (ctx->first_error == NULL && error != NULL)
            ctx->first_error = g_strdup(error->message);
    }
    g_clear_error(&error);

    ctx->index++;
    forget_check_next(ctx);
}

static void
on_forget_profile_settings_finished(GObject *source, GAsyncResult *result, gpointer user_data)
{
    ForgetCtx *ctx = user_data;
    GError *error = NULL;

    GVariant *reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), result, &error);
    if (reply == NULL && call_was_cancelled(error))
    {
        g_clear_error(&error);
        free_forget_ctx(ctx);
        return;
    }

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
        /* Delete this one, then carry on through the rest -- see
         * extras_menu_network_forget()'s doc comment for why we don't
         * stop at the first match. */
        g_dbus_connection_call(
            ctx->network->system_bus,
            NM_BUS_NAME,
            ctx->profile_paths[ctx->index],
            NM_SETTINGS_CONNECTION_IFACE,
            "Delete",
            NULL,
            NULL,
            G_DBUS_CALL_FLAGS_NONE,
            -1, ctx->network->cancellable,
            on_profile_deleted, ctx);
        return;
    }

    ctx->index++;
    forget_check_next(ctx);
}

static void
forget_check_next(ForgetCtx *ctx)
{
    if (ctx->profile_paths == NULL || ctx->profile_paths[ctx->index] == NULL)
    {
        finish_forget(ctx);
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
        -1, ctx->network->cancellable,
        on_forget_profile_settings_finished, ctx);
}

static void
on_forget_list_connections_finished(GObject *source, GAsyncResult *result, gpointer user_data)
{
    ForgetCtx *ctx = user_data;
    GError *error = NULL;

    GVariant *reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), result, &error);
    if (reply == NULL && call_was_cancelled(error))
    {
        g_clear_error(&error);
        free_forget_ctx(ctx);
        return;
    }

    if (reply == NULL)
    {
        ctx->had_error = TRUE;
        if (error != NULL)
            ctx->first_error = g_strdup(error->message);
        g_clear_error(&error);
        finish_forget(ctx);
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
    forget_check_next(ctx);
}

void
extras_menu_network_forget(ExtrasMenuNetwork *network,
                            const gchar *ssid,
                            ExtrasMenuNetworkConnectResultFunc result_callback,
                            gpointer result_user_data)
{
    if (network == NULL || network->system_bus == NULL || ssid == NULL)
    {
        if (result_callback != NULL)
            result_callback(FALSE, "Wi-Fi not available", result_user_data);
        return;
    }

    ForgetCtx *ctx = g_new0(ForgetCtx, 1);
    ctx->network = network;
    ctx->ssid = g_strdup(ssid);
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
        -1, network->cancellable,
        on_forget_list_connections_finished, ctx);
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
            stop_watch_sources(watch);
        else if (watch->timeout_source_id != 0)
            g_source_remove(watch->timeout_source_id);
        watch->timeout_source_id = 0;
        watch->subscription_id = 0;
        watch->callback = NULL;
        watch_unref(watch);
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