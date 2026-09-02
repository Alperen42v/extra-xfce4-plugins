#ifndef EXTRAS_MENU_BLUETOOTH_H
#define EXTRAS_MENU_BLUETOOTH_H

#include <glib.h>

G_BEGIN_DECLS

/*
 * Thin wrapper around BlueZ's D-Bus API (org.bluez, system bus).
 * Talks to the first Bluetooth adapter found (typically /org/bluez/hci0)
 * and exposes just its Powered state for now -- this is stage one of
 * Bluetooth support (adapter on/off only). Device discovery, pairing
 * and per-device connect/disconnect are a follow-up.
 *
 * Uses GDBus (part of GIO, already a dependency via brightness.c's use
 * of GSubprocess) rather than a dedicated Bluetooth library, since the
 * surface we need right now is small.
 */

typedef struct _ExtrasMenuBluetooth ExtrasMenuBluetooth;

/* Fired once we know the adapter's current Powered state (shortly
 * after extras_menu_bluetooth_new() returns), and again every time it
 * changes -- whether we caused it via extras_menu_bluetooth_set_powered()
 * or something else did (bluetoothctl, rfkill, a hardware switch).
 * available is FALSE if no Bluetooth adapter could be found at all
 * (no Bluetooth hardware, or bluetoothd isn't running); in that case
 * powered's value is meaningless and callers should treat the feature
 * as absent (e.g. keep the toggle visually disabled). */
typedef void (*ExtrasMenuBluetoothChangedFunc)(gboolean available,
                                                gboolean powered,
                                                gpointer user_data);

ExtrasMenuBluetooth *extras_menu_bluetooth_new(ExtrasMenuBluetoothChangedFunc callback,
                                                gpointer user_data);

/* Requests the adapter be powered on/off. Fire-and-forget: the actual
 * resulting state comes back through the callback, since this is a
 * D-Bus call under the hood. Safe to call before the adapter has been
 * found yet -- the request is simply dropped in that case. */
void extras_menu_bluetooth_set_powered(ExtrasMenuBluetooth *bluetooth, gboolean powered);

void extras_menu_bluetooth_free(ExtrasMenuBluetooth *bluetooth);

G_END_DECLS

#endif /* EXTRAS_MENU_BLUETOOTH_H */