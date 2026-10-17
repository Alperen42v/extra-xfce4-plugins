#ifndef EXTRAS_MENU_SYSTEM_STATUS_H
#define EXTRAS_MENU_SYSTEM_STATUS_H

#include "extras-menu.h"

G_BEGIN_DECLS

/* Creates the panel button (plugin->toggle_button) with its three status
 * icons -- network, volume, battery (or a power icon when there is no
 * battery) -- and adds it to the panel. The icons follow the panel's
 * orientation. Call this first: the dropdown hangs off the button. */
void extras_menu_system_status_build_panel_button(ExtrasMenuPlugin *plugin);

/* Re-evaluates the panel button's network icon from the state cached on
 * the plugin (connection kind, Wi-Fi radio, aeroplane mode). Call it
 * whenever one of those changes. */
void extras_menu_system_status_update_network_icon(ExtrasMenuPlugin *plugin);

/* Starts the UPower battery backend, driving the dropdown's battery badge
 * and the panel button's battery icon. The badge stays hidden until a
 * battery is reported. Needs the dropdown content to exist. */
void extras_menu_system_status_attach_battery(ExtrasMenuPlugin *plugin);

/* Starts the audio and brightness backends and connects the volume and
 * brightness sliders to them (both directions). Needs the dropdown
 * content to exist. */
void extras_menu_system_status_attach_sliders(ExtrasMenuPlugin *plugin);

/* Stops the battery, audio and brightness backends. */
void extras_menu_system_status_free(ExtrasMenuPlugin *plugin);

G_END_DECLS

#endif /* EXTRAS_MENU_SYSTEM_STATUS_H */