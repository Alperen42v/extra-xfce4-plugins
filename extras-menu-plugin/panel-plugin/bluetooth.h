#ifndef EXTRAS_MENU_BLUETOOTH_H
#define EXTRAS_MENU_BLUETOOTH_H

#include <glib.h>

G_BEGIN_DECLS

/*
 * Thin wrapper around BlueZ's D-Bus API (org.bluez, system bus).
 * Talks to the first Bluetooth adapter found (typically /org/bluez/hci0)
 * and exposes:
 *   - the adapter's Powered state (on/off), and changes to it
 *   - the list of devices BlueZ knows about: paired ones, plus -- while
 *     discovery is running -- nearby ones that have announced a name
 *   - connecting to a device (pairing it first if needed), disconnecting
 *     from it, and forgetting (unpairing) it
 *
 * Uses GDBus (part of GIO) rather than a dedicated Bluetooth library,
 * since the surface we need is small.
 */

typedef struct _ExtrasMenuBluetooth ExtrasMenuBluetooth;

/* One device, as reported to the devices_changed callback below. All
 * strings are owned by the backend and only valid for the duration of
 * that callback -- copy anything you need to keep. */
typedef struct
{
    const gchar *path;      /* D-Bus object path; identifies the device in later calls */
    const gchar *name;      /* human-readable name, never NULL */
    const gchar *icon;      /* BlueZ's icon hint ("audio-headset", "input-mouse", ...), may be NULL */
    gboolean paired;
    gboolean connected;
} ExtrasMenuBluetoothDevice;

/* Fired once we know the adapter's current Powered state (shortly
 * after extras_menu_bluetooth_new() returns), and again every time it
 * changes -- whether we caused it via extras_menu_bluetooth_set_powered()
 * or something else did (bluetoothctl, rfkill, a hardware switch).
 * available is FALSE if no Bluetooth adapter could be found at all
 * (no Bluetooth hardware, or bluetoothd isn't running), or the adapter
 * was unplugged; in that case powered's value is meaningless and callers
 * should treat the feature as absent (e.g. keep the toggle visually
 * disabled). If an adapter shows up later (a dongle plugged in), this
 * fires again with available TRUE. */
typedef void (*ExtrasMenuBluetoothChangedFunc)(gboolean available,
                                                gboolean powered,
                                                gpointer user_data);

/* Fired whenever the device list as shown to the user changes: a device
 * appearing or disappearing, being paired/unpaired, connecting or
 * disconnecting, or being renamed. Bursts of changes are folded into one
 * call. Signal-strength updates deliberately do not trigger it (they
 * arrive every second or so during discovery and are not displayed).
 *
 * devices is an array of count entries, ordered for display: connected
 * devices first, then other paired ones, then the rest, each group
 * alphabetically. Unpaired devices that never announced a name (BlueZ
 * then only knows their address) are left out, since a list of bare
 * hardware addresses is just noise. Also fired with an empty list when
 * the adapter goes away. */
typedef void (*ExtrasMenuBluetoothDevicesChangedFunc)(const ExtrasMenuBluetoothDevice *devices,
                                                       guint count,
                                                       gpointer user_data);

/* Fired when a connect/disconnect/forget request (below) finishes.
 * error_message is NULL on success; otherwise a short sentence suitable
 * for showing to the user. Always fired exactly once per request, except
 * that requests still in flight when extras_menu_bluetooth_free() is
 * called are dropped without a call. A request that cannot even be
 * started (no adapter, unknown device) is answered immediately, before
 * the request function returns -- callers that track "a request is
 * running" must register it before making the call. */
typedef void (*ExtrasMenuBluetoothResultFunc)(gboolean success,
                                               const gchar *error_message,
                                               gpointer user_data);

/* devices_callback may be NULL if the caller doesn't need the list. */
ExtrasMenuBluetooth *extras_menu_bluetooth_new(ExtrasMenuBluetoothChangedFunc callback,
                                                ExtrasMenuBluetoothDevicesChangedFunc devices_callback,
                                                gpointer user_data);

/* Requests the adapter be powered on/off. Fire-and-forget: the actual
 * resulting state comes back through the callback, since this is a
 * D-Bus call under the hood. Safe to call before the adapter has been
 * found yet -- the request is simply dropped in that case. */
void extras_menu_bluetooth_set_powered(ExtrasMenuBluetooth *bluetooth, gboolean powered);

/* Starts/stops scanning for nearby devices. Scanning drains battery and
 * is only needed while someone is looking at the list, so callers should
 * turn it on when the list is shown and off again when it's hidden. It
 * is also stopped automatically while the adapter is powered off, and
 * resumes by itself when it's powered on again if still requested. */
void extras_menu_bluetooth_set_discovering(ExtrasMenuBluetooth *bluetooth, gboolean discovering);

/* Connects to a device by the path from the device list. A device that
 * isn't paired yet is paired first (and marked trusted, so it reconnects
 * by itself next time). Pairing uses a built-in agent that only does
 * "Just Works" pairing -- headphones, speakers, mice and the like.
 * Devices that demand a PIN or passkey can't be paired this way and fail
 * with an explanatory message. result_callback is optional. */
void extras_menu_bluetooth_connect_device(ExtrasMenuBluetooth *bluetooth,
                                           const gchar *device_path,
                                           ExtrasMenuBluetoothResultFunc result_callback,
                                           gpointer result_user_data);

/* Disconnects from a device, keeping it paired. */
void extras_menu_bluetooth_disconnect_device(ExtrasMenuBluetooth *bluetooth,
                                              const gchar *device_path,
                                              ExtrasMenuBluetoothResultFunc result_callback,
                                              gpointer result_user_data);

/* Forgets a device: disconnects it if needed and removes its pairing. */
void extras_menu_bluetooth_remove_device(ExtrasMenuBluetooth *bluetooth,
                                          const gchar *device_path,
                                          ExtrasMenuBluetoothResultFunc result_callback,
                                          gpointer result_user_data);

void extras_menu_bluetooth_free(ExtrasMenuBluetooth *bluetooth);

G_END_DECLS

#endif /* EXTRAS_MENU_BLUETOOTH_H */