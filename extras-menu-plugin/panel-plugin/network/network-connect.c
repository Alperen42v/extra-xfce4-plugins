/*
 * Network backend: connecting to a network -- finding a saved profile
 * for an SSID, building a new one, activating it, and watching the
 * activation until it succeeds or fails.
 */

#include "network-private.h"

#include <string.h>

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

void
network_watch_unref(ActiveConnectionWatchCtx *watch)
{
    if (--watch->ref_count > 0)
        return;

    g_free(watch->created_profile_path);
    g_free(watch);
}

/* Drops the watch's signal subscription and timeout, if still active. */
void
network_stop_watch_sources(ActiveConnectionWatchCtx *watch)
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

    network_stop_watch_sources(watch);
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

    network_watch_unref(watch); /* the reference network->watches held */
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

    network_watch_unref(watch);
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

/* Reactivates an already-saved profile as-is -- its stored password
 * (if any) is used by NetworkManager itself, so the user isn't
 * prompted again for a network they've already connected to before.
 *
 * Device path is deliberately "/" (unspecified) rather than our known
 * Wi-Fi device path: some saved profiles carry an interface-name
 * constraint from whenever they were first created (e.g. a different
 * interface name than the current wlan0), and forcing our device path
 * against a mismatched profile makes NetworkManager reject the
 * activation outright ("mismatching interface name") instead of
 * resolving it itself. Passing "/" lets NM pick the right device using
 * the profile's own settings. */
static void
activate_existing_profile(ExtrasMenuNetwork *network,
                           const gchar *profile_path,
                           ExtrasMenuNetworkConnectResultFunc callback,
                           gpointer user_data)
{
    ConnectResultCtx *result_ctx = g_new0(ConnectResultCtx, 1);
    result_ctx->network = network;
    result_ctx->callback = callback;
    result_ctx->user_data = user_data;

    g_dbus_connection_call(
        network->system_bus,
        NM_BUS_NAME,
        NM_OBJ_PATH,
        NM_IFACE,
        "ActivateConnection",
        g_variant_new("(ooo)", profile_path, "/", "/"),
        G_VARIANT_TYPE("(o)"),
        G_DBUS_CALL_FLAGS_NONE,
        -1, network->cancellable,
        on_connect_finished, result_ctx);
}

/* --- giving an already-saved profile a new password -----------------------
 *
 * A saved profile whose password is wrong or missing (the router's
 * password changed, or the profile keeps its secret with a secret
 * agent we aren't) fails to connect, and the caller then asks the user
 * for a password. Activating the same profile again would ignore that
 * password and fail the same way, leaving the user stuck until they
 * "forget" the network by hand. So when a password is supplied for a
 * network that already has a profile, the profile's secret is replaced
 * first, and only then is it activated. Everything else in the profile
 * (static IP, DNS, MAC settings, ...) is carried over untouched. */

typedef struct
{
    ExtrasMenuNetwork *network;
    gchar *profile_path;
    gchar *password;
    ExtrasMenuApSecurity security; /* what the AP advertises now; UNKNOWN if not in the last scan */
    ExtrasMenuNetworkConnectResultFunc result_callback;
    gpointer result_user_data;
} UpdatePasswordCtx;

static void
free_update_password_ctx(UpdatePasswordCtx *ctx)
{
    g_free(ctx->profile_path);
    g_free(ctx->password);
    g_free(ctx);
}

/* Keys of an existing [802-11-wireless-security] section that are
 * rewritten by build_settings_with_new_password() and must therefore
 * not be copied over from the old section. */
static gboolean
is_replaced_security_key(const gchar *key)
{
    static const gchar *const replaced[] = {
        "key-mgmt", "psk", "psk-flags",
        "wep-key0", "wep-key-flags", "wep-key-type", "wep-tx-keyidx", "auth-alg",
        NULL
    };

    for (guint i = 0; replaced[i] != NULL; i++)
    {
        if (strcmp(key, replaced[i]) == 0)
            return TRUE;
    }
    return FALSE;
}

/* Returns the profile's settings (as read with GetSettings) with the
 * security section's password replaced, or NULL if this isn't a profile
 * a password can be set on (802.1X, or a key management we don't
 * know) -- the caller then just activates the profile as it is.
 *
 * Which key-mgmt to write follows what the AP advertises now, the same
 * way build_connection_settings() decides it for a new profile, so a
 * profile that is out of date in that respect (saved as WPA2, the
 * router has since moved to WPA3-only) is corrected too. The secret is
 * stored in the profile itself (flags 0), not left to a secret agent. */
static GVariant *
build_settings_with_new_password(GVariant *old_settings, const gchar *password,
                                  ExtrasMenuApSecurity security)
{
    GVariant *old_security = g_variant_lookup_value(old_settings, "802-11-wireless-security",
                                                    G_VARIANT_TYPE("a{sv}"));
    gchar *old_key_mgmt = NULL;

    if (old_security != NULL)
    {
        GVariant *km = g_variant_lookup_value(old_security, "key-mgmt", G_VARIANT_TYPE_STRING);
        if (km != NULL)
        {
            old_key_mgmt = g_variant_dup_string(km, NULL);
            g_variant_unref(km);
        }
    }

    if (old_key_mgmt != NULL &&
        (g_str_has_prefix(old_key_mgmt, "wpa-eap") || strcmp(old_key_mgmt, "ieee8021x") == 0))
    {
        g_free(old_key_mgmt);
        if (old_security != NULL)
            g_variant_unref(old_security);
        return NULL;
    }

    const gchar *target = NULL;
    switch (security)
    {
        case EXTRAS_MENU_AP_SECURITY_WEP:
            target = "none";
            break;
        case EXTRAS_MENU_AP_SECURITY_WPA3:
            target = "sae";
            break;
        case EXTRAS_MENU_AP_SECURITY_WPA2_WPA3:
            /* Either works against a transitional AP; keep what the
             * profile already uses. */
            target = (old_key_mgmt != NULL && strcmp(old_key_mgmt, "sae") == 0) ? "sae" : "wpa-psk";
            break;
        case EXTRAS_MENU_AP_SECURITY_WPA:
        case EXTRAS_MENU_AP_SECURITY_WPA2:
        case EXTRAS_MENU_AP_SECURITY_WPA_WPA2:
            target = "wpa-psk";
            break;
        case EXTRAS_MENU_AP_SECURITY_UNKNOWN:
            /* Not in the last scan (hidden, out of range): all there is
             * to go by is the profile itself. */
            if (old_key_mgmt == NULL)
                target = "wpa-psk";
            else if (strcmp(old_key_mgmt, "wpa-psk") == 0 || strcmp(old_key_mgmt, "sae") == 0 ||
                     strcmp(old_key_mgmt, "none") == 0)
                target = old_key_mgmt;
            break;
        default: /* OPEN, OWE, ENTERPRISE: no password to set */
            break;
    }

    if (target == NULL)
    {
        g_free(old_key_mgmt);
        if (old_security != NULL)
            g_variant_unref(old_security);
        return NULL;
    }

    /* New security section: the old one's other keys, plus ours. */
    GVariantBuilder sec;
    g_variant_builder_init(&sec, G_VARIANT_TYPE("a{sv}"));

    if (old_security != NULL)
    {
        GVariantIter iter;
        const gchar *key = NULL;
        GVariant *value = NULL;

        g_variant_iter_init(&iter, old_security);
        while (g_variant_iter_next(&iter, "{&sv}", &key, &value))
        {
            if (!is_replaced_security_key(key))
                g_variant_builder_add(&sec, "{sv}", key, value);
            g_variant_unref(value);
        }
    }

    g_variant_builder_add(&sec, "{sv}", "key-mgmt", g_variant_new_string(target));

    if (strcmp(target, "none") == 0)
    {
        /* WEP -- same shape as in build_connection_settings(). */
        g_variant_builder_add(&sec, "{sv}", "wep-key0", g_variant_new_string(password));
        g_variant_builder_add(&sec, "{sv}", "wep-key-type", g_variant_new_uint32(1));
        g_variant_builder_add(&sec, "{sv}", "wep-key-flags", g_variant_new_uint32(0));
        g_variant_builder_add(&sec, "{sv}", "wep-tx-keyidx", g_variant_new_uint32(0));
        g_variant_builder_add(&sec, "{sv}", "auth-alg", g_variant_new_string("open"));
    }
    else
    {
        g_variant_builder_add(&sec, "{sv}", "psk", g_variant_new_string(password));
        g_variant_builder_add(&sec, "{sv}", "psk-flags", g_variant_new_uint32(0));
    }

    /* Whole profile: every other section as it was. */
    GVariantBuilder all;
    g_variant_builder_init(&all, G_VARIANT_TYPE("a{sa{sv}}"));

    GVariantIter iter;
    const gchar *name = NULL;
    GVariant *section = NULL;

    g_variant_iter_init(&iter, old_settings);
    while (g_variant_iter_next(&iter, "{&s@a{sv}}", &name, &section))
    {
        if (strcmp(name, "802-11-wireless-security") != 0)
            g_variant_builder_add(&all, "{s@a{sv}}", name, section);
        g_variant_unref(section);
    }
    g_variant_builder_add(&all, "{s@a{sv}}", "802-11-wireless-security",
                           g_variant_builder_end(&sec));

    g_free(old_key_mgmt);
    if (old_security != NULL)
        g_variant_unref(old_security);

    return g_variant_builder_end(&all);
}

/* Reports a failed password update. D-Bus errors carry a
 * "GDBus.Error:org.freedesktop....:" prefix that means nothing to the
 * user; it's stripped first. */
static void
report_update_failure(UpdatePasswordCtx *ctx, GError *error)
{
    if (ctx->result_callback == NULL)
        return;

    gchar *message;
    if (error != NULL)
    {
        g_dbus_error_strip_remote_error(error);
        message = g_strdup_printf("Couldn't save the new password: %s", error->message);
    }
    else
    {
        message = g_strdup("Couldn't save the new password");
    }

    ctx->result_callback(FALSE, message, ctx->result_user_data);
    g_free(message);
}

static void
on_profile_password_updated(GObject *source, GAsyncResult *result, gpointer user_data)
{
    UpdatePasswordCtx *ctx = user_data;
    GError *error = NULL;

    GVariant *reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), result, &error);
    if (reply == NULL && call_was_cancelled(error))
    {
        g_clear_error(&error);
        free_update_password_ctx(ctx);
        return;
    }

    if (reply == NULL)
    {
        report_update_failure(ctx, error);
        g_clear_error(&error);
        free_update_password_ctx(ctx);
        return;
    }

    g_variant_unref(reply);

    /* The profile has its new password now -- connect with it. */
    activate_existing_profile(ctx->network, ctx->profile_path,
                               ctx->result_callback, ctx->result_user_data);
    free_update_password_ctx(ctx);
}

static void
on_profile_settings_for_update_finished(GObject *source, GAsyncResult *result, gpointer user_data)
{
    UpdatePasswordCtx *ctx = user_data;
    GError *error = NULL;

    GVariant *reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), result, &error);
    if (reply == NULL && call_was_cancelled(error))
    {
        g_clear_error(&error);
        free_update_password_ctx(ctx);
        return;
    }

    if (reply == NULL)
    {
        report_update_failure(ctx, error);
        g_clear_error(&error);
        free_update_password_ctx(ctx);
        return;
    }

    GVariant *old_settings = NULL;
    g_variant_get(reply, "(@a{sa{sv}})", &old_settings);

    GVariant *new_settings = build_settings_with_new_password(old_settings, ctx->password,
                                                              ctx->security);
    g_variant_unref(old_settings);
    g_variant_unref(reply);

    if (new_settings == NULL)
    {
        /* Not a profile a password can be set on (802.1X, ...): nothing
         * to update, use it as it is. */
        activate_existing_profile(ctx->network, ctx->profile_path,
                                   ctx->result_callback, ctx->result_user_data);
        free_update_password_ctx(ctx);
        return;
    }

    g_dbus_connection_call(
        ctx->network->system_bus,
        NM_BUS_NAME,
        ctx->profile_path,
        NM_SETTINGS_CONNECTION_IFACE,
        "Update",
        g_variant_new("(@a{sa{sv}})", new_settings),
        NULL,
        G_DBUS_CALL_FLAGS_NONE,
        -1, ctx->network->cancellable,
        on_profile_password_updated, ctx);
}

static void
update_profile_password_then_activate(ExtrasMenuNetwork *network,
                                       const gchar *profile_path,
                                       const gchar *password,
                                       ExtrasMenuApSecurity security,
                                       ExtrasMenuNetworkConnectResultFunc result_callback,
                                       gpointer result_user_data)
{
    UpdatePasswordCtx *ctx = g_new0(UpdatePasswordCtx, 1);
    ctx->network = network;
    ctx->profile_path = g_strdup(profile_path);
    ctx->password = g_strdup(password);
    ctx->security = security;
    ctx->result_callback = result_callback;
    ctx->result_user_data = result_user_data;

    g_dbus_connection_call(
        network->system_bus,
        NM_BUS_NAME,
        profile_path,
        NM_SETTINGS_CONNECTION_IFACE,
        "GetSettings",
        NULL,
        G_VARIANT_TYPE("(a{sa{sv}})"),
        G_DBUS_CALL_FLAGS_NONE,
        -1, network->cancellable,
        on_profile_settings_for_update_finished, ctx);
}

/* Once we know whether an existing profile matches (or we've run out
 * of profiles to check), either activates the found one (first giving
 * it the supplied password, if there is one -- see
 * update_profile_password_then_activate()) or falls back to
 * creating+activating a fresh one with build_connection_settings(). */
static void
proceed_with_connect(FindProfileCtx *ctx, const gchar *existing_profile_path)
{
    ExtrasMenuApSecurity known_security = network_lookup_known_security(ctx->network, ctx->ssid);
    gboolean have_password = (ctx->password != NULL && ctx->password[0] != '\0');

    if (existing_profile_path != NULL)
    {
        if (have_password && known_security != EXTRAS_MENU_AP_SECURITY_ENTERPRISE &&
            network_security_needs_password(known_security, ctx->requires_password))
        {
            /* A saved profile exists, but the caller has a password for
             * it -- which only happens after activating the profile as
             * it was has already failed (see extras-menu.c's
             * on_connect_result()). Activating it again would ignore the
             * password, so the profile gets it first. */
            update_profile_password_then_activate(ctx->network, existing_profile_path,
                                                   ctx->password, known_security,
                                                   ctx->result_callback, ctx->result_user_data);
        }
        else
        {
            activate_existing_profile(ctx->network, existing_profile_path,
                                       ctx->result_callback, ctx->result_user_data);
        }
    }
    else if (known_security == EXTRAS_MENU_AP_SECURITY_ENTERPRISE)
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
    }
    else if (network_security_needs_password(known_security, ctx->requires_password) && !have_password)
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
    }
    else
    {
        ConnectResultCtx *result_ctx = g_new0(ConnectResultCtx, 1);
        result_ctx->network = ctx->network;
        result_ctx->callback = ctx->result_callback;
        result_ctx->user_data = ctx->result_user_data;

        GVariant *connection_settings = build_connection_settings(ctx->ssid, ctx->password,
                                                                  known_security);

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
                gchar *found_ssid = network_decode_ssid(ssid_v);
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