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
} ExtrasMenuAccessPoint;

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

/* Connects to the network with the given SSID. password may be NULL
 * (or empty) for an open network; for a secured one, pass the
 * Wi-Fi/WPA password. result_callback (optional) is invoked once the
 * attempt finishes. If a saved connection profile for this SSID
 * already exists, it's reactivated as-is (password is ignored in that
 * case); otherwise a new profile is created with the given password. */
void extras_menu_network_connect(ExtrasMenuNetwork *network,
                                  const gchar *ssid,
                                  const gchar *password,
                                  ExtrasMenuNetworkConnectResultFunc result_callback,
                                  gpointer result_user_data);

/* Disconnects the Wi-Fi device, if currently connected to anything. */
void extras_menu_network_disconnect(ExtrasMenuNetwork *network);

void extras_menu_network_free(ExtrasMenuNetwork *network);

G_END_DECLS

#endif /* EXTRAS_MENU_NETWORK_H */