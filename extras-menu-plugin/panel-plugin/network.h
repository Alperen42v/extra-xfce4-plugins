#ifndef EXTRAS_MENU_NETWORK_H
#define EXTRAS_MENU_NETWORK_H

#include <glib.h>

G_BEGIN_DECLS

/*
 * Thin wrapper around NetworkManager's D-Bus API (org.freedesktop.NetworkManager,
 * system bus). Tracks both the first Wi-Fi device and the first
 * Ethernet device found, and exposes:
 *   - which kind of connection (if any) is currently active: Wi-Fi or
 *     Ethernet -- used to decide what the "Wi-Fi"/"Ethernet" pill in
 *     the dropdown should say and do
 *   - for Wi-Fi: the list of currently visible access points (scan
 *     results), connecting to an open or password-protected network,
 *     and disconnecting
 *   - for Ethernet: just the connected interface's IP address, shown
 *     as a simple info readout rather than a list (there's nothing to
 *     "choose" on a wired connection)
 */

typedef struct _ExtrasMenuNetwork ExtrasMenuNetwork;

/* Which kind of connection is currently active, if any. Ethernet takes
 * priority over Wi-Fi when both happen to be active at once (matches
 * typical desktop behavior/expectations: a plugged-in cable is the
 * "real" connection, Wi-Fi is treated as a fallback in the UI even if
 * NM itself is happy to run both). */
typedef enum
{
    EXTRAS_MENU_NETWORK_KIND_NONE,
    EXTRAS_MENU_NETWORK_KIND_WIFI,
    EXTRAS_MENU_NETWORK_KIND_ETHERNET
} ExtrasMenuNetworkKind;

/* The security scheme an access point advertises, derived from
 * NetworkManager's Flags/WpaFlags/RsnFlags bitfields. Mixed modes get
 * their own values rather than being collapsed into the stronger one,
 * since "WPA2/WPA3" tells the user something different from plain
 * "WPA3" (namely that older clients can still connect). */
typedef enum
{
    EXTRAS_MENU_AP_SECURITY_UNKNOWN,
    EXTRAS_MENU_AP_SECURITY_OPEN,
    EXTRAS_MENU_AP_SECURITY_OWE,        /* Enhanced Open -- encrypted, no password */
    EXTRAS_MENU_AP_SECURITY_WEP,
    EXTRAS_MENU_AP_SECURITY_WPA,
    EXTRAS_MENU_AP_SECURITY_WPA2,
    EXTRAS_MENU_AP_SECURITY_WPA_WPA2,   /* transitional, both accepted */
    EXTRAS_MENU_AP_SECURITY_WPA3,
    EXTRAS_MENU_AP_SECURITY_WPA2_WPA3,  /* transitional, both accepted */
    EXTRAS_MENU_AP_SECURITY_ENTERPRISE  /* 802.1X, any generation */
} ExtrasMenuApSecurity;

/* One access point (a visible Wi-Fi network), as reported to the
 * network_list_changed callback below. Strings are owned by the
 * struct and only valid for the duration of that callback -- copy
 * anything you need to keep. */
typedef struct
{
    gchar *ssid;
    gint8 strength; /* 0-100 */
    gboolean secured; /* TRUE if a password is required to connect */
    gboolean is_active; /* TRUE if this is the network we're currently connected to */

    ExtrasMenuApSecurity security;
    gchar *bssid;         /* AP hardware address, e.g. "00:11:22:33:44:55" */
    guint32 frequency;    /* MHz, e.g. 2437 */
    guint32 max_bitrate;  /* kb/s as reported by NM, e.g. 270000 */
} ExtrasMenuAccessPoint;

/* Human-readable name for a security scheme ("WPA2", "WPA2/WPA3",
 * "Open", ...), suitable for showing directly in a UI. Returned string
 * is static and must not be freed. */
const gchar *extras_menu_ap_security_to_string(ExtrasMenuApSecurity security);

/* Fired whenever the visible access point list changes (initial scan
 * results, periodic rescans, a network appearing/disappearing, or the
 * active connection changing). aps is an array of count access
 * points, sorted by signal strength (strongest first) with duplicate
 * SSIDs collapsed to their strongest instance -- ready to display
 * directly as a list. wifi_available is FALSE if no Wi-Fi device was
 * found at all (no Wi-Fi hardware, or NetworkManager isn't managing
 * it), in which case aps/count are meaningless. */
typedef void (*ExtrasMenuNetworkListChangedFunc)(gboolean wifi_available,
                                                   const ExtrasMenuAccessPoint *aps,
                                                   guint count,
                                                   gpointer user_data);

/* Fired whenever the active connection kind or its details change:
 * NetworkManager reporting a different active connection (cable
 * plugged/unplugged, Wi-Fi connected/disconnected), or the IP address
 * of an active Ethernet connection changing. ip_address is NULL unless
 * kind is EXTRAS_MENU_NETWORK_KIND_ETHERNET and an address is known
 * yet (DHCP can take a moment after the cable is plugged in). */
typedef void (*ExtrasMenuNetworkStatusChangedFunc)(ExtrasMenuNetworkKind kind,
                                                     const gchar *ip_address,
                                                     gpointer user_data);

/* Fired after a connect attempt (extras_menu_network_connect below)
 * completes, successfully or not, so the UI can stop showing a
 * "connecting..." state and, on failure, tell the user why (e.g. wrong
 * password). error_message is NULL on success. */
typedef void (*ExtrasMenuNetworkConnectResultFunc)(gboolean success,
                                                     const gchar *error_message,
                                                     gpointer user_data);

/* Fired once we know the Wi-Fi radio's enabled state (shortly after
 * extras_menu_network_new() returns), and again whenever it changes --
 * whether we caused it via extras_menu_network_set_wifi_enabled() or
 * something else did (a hardware kill switch, another app, nmcli).
 * This tracks NetworkManager's global WirelessEnabled property, i.e.
 * the radio itself -- separate from whether we're actually connected
 * to a network. */
typedef void (*ExtrasMenuNetworkWifiEnabledChangedFunc)(gboolean enabled, gpointer user_data);

/* status_changed_callback and wifi_enabled_changed_callback may be
 * NULL if the caller doesn't need them (unlikely in practice, but kept
 * optional for symmetry with the other backends' constructors). */
ExtrasMenuNetwork *extras_menu_network_new(ExtrasMenuNetworkListChangedFunc list_changed_callback,
                                            ExtrasMenuNetworkStatusChangedFunc status_changed_callback,
                                            ExtrasMenuNetworkWifiEnabledChangedFunc wifi_enabled_changed_callback,
                                            gpointer user_data);

/* Turns the Wi-Fi radio itself on/off (NetworkManager's
 * WirelessEnabled property) -- independent of any specific network
 * connection. Fire-and-forget: the resulting state comes back through
 * wifi_enabled_changed_callback. */
void extras_menu_network_set_wifi_enabled(ExtrasMenuNetwork *network, gboolean enabled);

/* Asks NetworkManager to rescan for networks. Fire-and-forget --
 * results (if any new networks are found) arrive through the normal
 * list_changed callback once the scan completes. Safe to call
 * frequently; NetworkManager rate-limits scans on its own. */
void extras_menu_network_rescan(ExtrasMenuNetwork *network);

/* Connects to the network with the given SSID. If a saved connection
 * profile for this SSID already exists, it's reactivated as-is (the
 * password argument is ignored in that case -- NetworkManager uses
 * the profile's own stored credentials). Otherwise, behavior depends
 * on requires_password:
 *   - FALSE (open network): a new profile is created and activated,
 *     password may be NULL.
 *   - TRUE (secured network): if password is NULL or empty, this
 *     fails immediately (via result_callback) with an explanatory
 *     error rather than creating a profile with no password, which
 *     would appear to succeed at the D-Bus level (profile created,
 *     activation "accepted") while never actually completing the
 *     WPA handshake -- silently going nowhere instead of surfacing an
 *     error. If password is non-empty, a new profile is created with
 *     it and activated normally.
 * result_callback (optional) is invoked once the attempt finishes. */
void extras_menu_network_connect(ExtrasMenuNetwork *network,
                                  const gchar *ssid,
                                  const gchar *password,
                                  gboolean requires_password,
                                  ExtrasMenuNetworkConnectResultFunc result_callback,
                                  gpointer result_user_data);

/* Disconnects the Wi-Fi device, if currently connected to anything. */
void extras_menu_network_disconnect(ExtrasMenuNetwork *network);

/* Deletes every saved connection profile matching this SSID, so
 * NetworkManager forgets its stored password and stops auto-connecting
 * to it. Deleting all matches rather than just the first is
 * deliberate: duplicate profiles for one SSID do accumulate in
 * practice (NM appends " 1", " 2", ... when a profile is recreated),
 * and leaving one behind would make "forget" appear not to have
 * worked. result_callback (optional) fires once, after the last
 * deletion finishes. */
void extras_menu_network_forget(ExtrasMenuNetwork *network,
                                 const gchar *ssid,
                                 ExtrasMenuNetworkConnectResultFunc result_callback,
                                 gpointer result_user_data);

void extras_menu_network_free(ExtrasMenuNetwork *network);

G_END_DECLS

#endif /* EXTRAS_MENU_NETWORK_H */