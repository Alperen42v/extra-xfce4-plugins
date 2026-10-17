#ifndef EXTRAS_MENU_BLUETOOTH_UI_H
#define EXTRAS_MENU_BLUETOOTH_UI_H

#include "extras-menu.h"

G_BEGIN_DECLS

/* Creates the device-path -> row lookup, starts the Bluetooth backend and
 * connects the Bluetooth pill (adapter toggle, chevron) and its device
 * list to it. Needs the dropdown content to exist. Catches up on anything
 * the backend reported before the widgets existed. */
void extras_menu_bluetooth_ui_attach(ExtrasMenuPlugin *plugin);

/* Stops the Bluetooth backend and releases what this module keeps on the
 * plugin. */
void extras_menu_bluetooth_ui_free(ExtrasMenuPlugin *plugin);

/* Opens/closes the Bluetooth device list, flips its chevron, and turns
 * scanning for nearby devices on/off to match -- scanning costs battery
 * and must never be left running while the list isn't showing. Safe to
 * call before the widgets exist. */
void extras_menu_bluetooth_ui_set_list_open(ExtrasMenuPlugin *plugin, gboolean open);

G_END_DECLS

#endif /* EXTRAS_MENU_BLUETOOTH_UI_H */