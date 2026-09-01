#ifndef EXTRAS_MENU_PREFERENCES_H
#define EXTRAS_MENU_PREFERENCES_H

#include <gtk/gtk.h>
#include <libxfce4panel/libxfce4panel.h>

G_BEGIN_DECLS

/* Current plugin version, shown in the preferences dialog. Bump this
 * by hand as the plugin evolves -- there's no build-time version
 * injection set up yet. */
#define EXTRAS_MENU_VERSION "0.1.0-alpha"

/* Builds and shows the preferences dialog for the plugin, anchored to
 * the panel it belongs to. Currently just shows the version/release
 * note; this is where per-user settings (which toggles are enabled,
 * colors, etc.) will be added as the plugin grows. Safe to call
 * multiple times -- each call creates its own dialog instance. */
void extras_menu_preferences_show(XfcePanelPlugin *panel_plugin);

G_END_DECLS

#endif /* EXTRAS_MENU_PREFERENCES_H */