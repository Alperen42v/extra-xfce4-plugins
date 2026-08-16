#ifndef __CAFFEINE_PREFS_H__
#define __CAFFEINE_PREFS_H__

#include <glib.h>
#include <gtk/gtk.h>
#include <libxfce4panel/xfce-panel-plugin.h>

#include "caffeine.h"

G_BEGIN_DECLS

/* CaffeineLockCycleMode and CaffeineSettings are declared in caffeine.h
 * (kept there to avoid a circular include, since this header includes
 * caffeine.h for XfcePanelPlugin/CaffeinePlugin). See caffeine.h for the
 * full explanation of what this setting does. */

/* Resolves the effective interval in minutes for the current settings.
 * Returns 0 if the lock cycle is disabled (mode == NEVER). */
guint caffeine_settings_get_interval_minutes (const CaffeineSettings *settings);

/* Resolves the effective screen-off interval in minutes. Returns 0 if
 * the screen-off timer is disabled (screen_off_enabled == FALSE) or the
 * mode/custom value is invalid. */
guint caffeine_settings_get_screen_off_interval_minutes (const CaffeineSettings *settings);

/* Loads settings from xfconf (falls back to defaults - lock cycle off - if
 * xfconf is unavailable or nothing has been saved yet). channel_name should
 * be unique per plugin instance, e.g. "xfce4-caffeine-plugin-<panel-id>". */
void caffeine_settings_load (CaffeineSettings *settings, const gchar *channel_name);

/* Persists settings to xfconf under channel_name. */
void caffeine_settings_save (const CaffeineSettings *settings, const gchar *channel_name);

/* Shows the (modal) preferences dialog for the given plugin instance,
 * letting the user pick the lock-cycle duration. Blocks until closed.
 * On accept, updates *settings in place, saves to xfconf under
 * channel_name, and returns TRUE. Returns FALSE if the user cancelled
 * (settings left unchanged). */
gboolean caffeine_show_preferences (XfcePanelPlugin *plugin, CaffeineSettings *settings,
                                     const gchar *channel_name);

G_END_DECLS

#endif /* !__CAFFEINE_PREFS_H__ */