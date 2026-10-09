#ifndef EXTRAS_MENU_NETWORK_PRIVATE_H
#define EXTRAS_MENU_NETWORK_PRIVATE_H

/*
 * Internals shared by the network backend's source files. Nothing
 * outside the network/ folder includes this; the public API is
 * network.h.
 *
 *   network.c          bus connection, Wi-Fi device discovery, live
 *                      change notifications, new/free/rescan/
 *                      set_wifi_enabled/disconnect
 *   network-ap.c       access point security parsing and the visible
 *                      access point list
 *   network-status.c   primary connection tracking (Wi-Fi/Ethernet
 *                      pill), Ethernet IP, Wi-Fi radio state
 *   network-connect.c  connecting, watching an activation, building
 *                      connection profiles, finding saved profiles
 *   network-forget.c   forgetting a network (deleting its profiles)
 *
 * Functions shared between those files are not static and carry a
 * network_ prefix, since they end up in the plugin's symbol table.
 */

#include "network.h"

#include <gio/gio.h>

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
static inline gboolean
call_was_cancelled(const GError *error)
{
    return error != NULL && g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
}

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

/* --- defined in network-ap.c --- */

gboolean
network_security_needs_password(ExtrasMenuApSecurity security, gboolean fallback);

ExtrasMenuApSecurity
network_lookup_known_security(ExtrasMenuNetwork *network, const gchar *ssid);

gchar *
network_decode_ssid(GVariant *ssid_bytes_variant);

void
network_request_ap_list(ExtrasMenuNetwork *network);

/* --- defined in network-status.c --- */

void
network_start_primary_connection_tracking(ExtrasMenuNetwork *network);

/* --- defined in network-connect.c --- */

void
network_watch_unref(ActiveConnectionWatchCtx *watch);

void
network_stop_watch_sources(ActiveConnectionWatchCtx *watch);

#endif /* EXTRAS_MENU_NETWORK_PRIVATE_H */