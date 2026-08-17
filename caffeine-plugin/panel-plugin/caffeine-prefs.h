#ifndef __CAFFEINE_PREFS_H__
#define __CAFFEINE_PREFS_H__

#include <glib.h>
#include <gtk/gtk.h>
#include <libxfce4panel/xfce-panel-plugin.h>

#include "caffeine.h"

G_BEGIN_DECLS

/* CaffeineLockCycleMode and CaffeineSettings are declared in caffeine.h
 * to avoid a circular include. */

/* Returns 0 if the lock cycle is disabled (mode == NEVER). */
guint caffeine_settings_get_interval_minutes (const CaffeineSettings *settings);

/* Returns 0 if the screen-off timer is disabled or the mode/custom
 * value is invalid. */
guint caffeine_settings_get_screen_off_interval_minutes (const CaffeineSettings *settings);

/* Loads settings from xfconf (defaults to lock cycle off if unavailable
 * or unset). channel_name should be unique per plugin instance. */
void caffeine_settings_load (CaffeineSettings *settings, const gchar *channel_name);

/* Persists settings to xfconf under channel_name. */
void caffeine_settings_save (const CaffeineSettings *settings, const gchar *channel_name);

/* Shows the modal preferences dialog. Blocks until closed. On accept,
 * updates *settings, saves to xfconf, and returns TRUE. Returns FALSE
 * if cancelled. */
gboolean caffeine_show_preferences (XfcePanelPlugin *plugin, CaffeineSettings *settings,
                                     const gchar *channel_name);

G_END_DECLS

#endif /* !__CAFFEINE_PREFS_H__ */