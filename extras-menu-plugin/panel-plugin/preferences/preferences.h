#ifndef EXTRAS_MENU_PREFERENCES_H
#define EXTRAS_MENU_PREFERENCES_H

#include <gtk/gtk.h>
#include <libxfce4panel/libxfce4panel.h>

G_BEGIN_DECLS

/* Current plugin version, shown in the preferences dialog. Bump this
 * by hand as the plugin evolves -- there's no build-time version
 * injection set up yet. */
#define EXTRAS_MENU_VERSION "0.2.2-alpha"

/* Shows the plugin's preferences dialog, anchored to the panel it
 * belongs to. It has a sidebar of sections (Home, Quick action
 * buttons, ...) and opens on Home; the section list lives in the table
 * at the top of preferences.c. The settings it edits are shared by
 * every instance of the plugin, so only one dialog can be open at a
 * time -- calling this while it's already open just brings the
 * existing dialog to the front. */
void extras_menu_preferences_show(XfcePanelPlugin *panel_plugin);

G_END_DECLS

#endif /* EXTRAS_MENU_PREFERENCES_H */