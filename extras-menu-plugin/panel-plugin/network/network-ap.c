/*
 * Network backend: access point security parsing and the list of visible
 * access points (fetching each AP's properties, then assembling the
 * sorted, de-duplicated list handed to the UI).
 */

#include "network-private.h"

#include <string.h>

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
gboolean
network_security_needs_password(ExtrasMenuApSecurity security, gboolean fallback)
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
ExtrasMenuApSecurity
network_lookup_known_security(ExtrasMenuNetwork *network, const gchar *ssid)
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
gchar *
network_decode_ssid(GVariant *ssid_bytes_variant)
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
        ap->ssid = network_decode_ssid(ssid_v);
        ap->strength = strength_v != NULL ? (gint8) g_variant_get_byte(strength_v) : 0;

        guint32 flags = flags_v != NULL ? g_variant_get_uint32(flags_v) : 0;
        guint32 wpa_flags = wpa_flags_v != NULL ? g_variant_get_uint32(wpa_flags_v) : 0;
        guint32 rsn_flags = rsn_flags_v != NULL ? g_variant_get_uint32(rsn_flags_v) : 0;
        ap->security = derive_ap_security(flags, wpa_flags, rsn_flags);
        ap->secured = network_security_needs_password(ap->security,
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

void
network_request_ap_list(ExtrasMenuNetwork *network)
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