#ifndef EXTRAS_MENU_AIRPLANE_MODE_H
#define EXTRAS_MENU_AIRPLANE_MODE_H

#include <gtk/gtk.h>

G_BEGIN_DECLS

/*
 * The "Aeroplane Mode" pill: one toggle that switches the Wi-Fi radio
 * and the Bluetooth adapter off together, and back on again.
 *
 * The module owns everything about the pill's behaviour: it connects to
 * the toggle's "toggled" signal itself, guards against feedback loops
 * itself, and remembers on its own which radios were on before it
 * switched them off. extras-menu.c only has to
 *   - create it from the pill widget,
 *   - tell it how to switch the two radios,
 *   - forward the Wi-Fi and Bluetooth backends' state changes to it, and
 *   - free it.
 *
 * The toggle is not stored as a setting: it shows "on" after the user
 * switched it on, for as long as Wi-Fi and Bluetooth (if there is an
 * adapter) both stay off. Switching just one radio off by itself does
 * not turn it on. It also follows changes made elsewhere -- turning
 * Wi-Fi back on from the Wi-Fi pill, or from nmcli, switches aeroplane
 * mode off by itself. It starts "off" when the plugin loads, even if
 * both radios happen to be off already.
 */

typedef struct _ExtrasMenuAirplaneMode ExtrasMenuAirplaneMode;

/* How the module switches one radio. Fire-and-forget: the confirmed
 * state comes back through extras_menu_airplane_mode_wifi_changed() /
 * extras_menu_airplane_mode_bluetooth_changed(). */
typedef void (*ExtrasMenuAirplaneModeSetRadioFunc)(gboolean enabled, gpointer user_data);

/* Fired whenever what the pill shows changes between "aeroplane mode on"
 * and "off" -- right away on a click (not only once the radios have
 * followed), and when something else switches the radios. Used for
 * things that should look different in aeroplane mode, such as the
 * panel button's network icon. Not fired for the state it starts in
 * ("off"). */
typedef void (*ExtrasMenuAirplaneModeChangedFunc)(gboolean active, gpointer user_data);

/* `toggle` is the pill's GtkToggleButton (see
 * extras_menu_popover_get_pill()). It stays insensitive until both
 * backends have reported their state at least once, since before that
 * there is no way to know what "on" and "off" would mean. `changed` may
 * be NULL. */
ExtrasMenuAirplaneMode *extras_menu_airplane_mode_new(GtkWidget *toggle,
                                                       ExtrasMenuAirplaneModeSetRadioFunc set_wifi,
                                                       ExtrasMenuAirplaneModeSetRadioFunc set_bluetooth,
                                                       ExtrasMenuAirplaneModeChangedFunc changed,
                                                       gpointer user_data);

/* Whether the pill currently shows "on". FALSE for NULL and until the
 * backends have reported. */
gboolean extras_menu_airplane_mode_is_active(const ExtrasMenuAirplaneMode *airplane_mode);

/* Forward the Wi-Fi backend's wifi_enabled_changed callback here. */
void extras_menu_airplane_mode_wifi_changed(ExtrasMenuAirplaneMode *airplane_mode,
                                             gboolean enabled);

/* Forward the Bluetooth backend's state callback here. `available` is
 * FALSE when there is no adapter; in that case `powered` is ignored and
 * Bluetooth simply counts as "off". */
void extras_menu_airplane_mode_bluetooth_changed(ExtrasMenuAirplaneMode *airplane_mode,
                                                  gboolean available,
                                                  gboolean powered);

/* Safe to call with NULL. The toggle itself is not destroyed. */
void extras_menu_airplane_mode_free(ExtrasMenuAirplaneMode *airplane_mode);

G_END_DECLS

#endif /* EXTRAS_MENU_AIRPLANE_MODE_H */