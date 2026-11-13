#ifndef EXTRAS_MENU_CAFFEINE_CONFIG_H
#define EXTRAS_MENU_CAFFEINE_CONFIG_H

#include <glib.h>

G_BEGIN_DECLS

/* How long the Caffeine pill stays on before it switches itself off, in
 * minutes. This value means "never": the pill stays on until the user
 * turns it off. It is the default. */
#define EXTRAS_MENU_CAFFEINE_UNLIMITED 0

/* The longest duration that can be saved (one day). A larger value in
 * the file is read as this one. */
#define EXTRAS_MENU_CAFFEINE_MAX_MINUTES (24 * 60)

/* The saved duration in minutes, EXTRAS_MENU_CAFFEINE_UNLIMITED if
 * nothing has been saved or the file can't be read. Read from
 * ~/.config/xfce4/extras-menu/caffeine.ini (or wherever XDG_CONFIG_HOME
 * points). */
guint extras_menu_caffeine_config_get_duration(void);

/* Saves the duration (anything above EXTRAS_MENU_CAFFEINE_MAX_MINUTES is
 * saved as that) and, on success, calls every registered listener.
 * Does nothing if it already is the saved one. */
gboolean extras_menu_caffeine_config_set_duration(guint minutes, GError **error);

/* Change notification, so a Caffeine pill that is already on notices a
 * new duration the moment the preferences editor saves it. Listeners run
 * in the main thread right after a successful save. Remove a listener
 * before whatever user_data points to goes away. */
typedef void (*ExtrasMenuCaffeineConfigChangedFunc)(gpointer user_data);

guint extras_menu_caffeine_config_add_listener(ExtrasMenuCaffeineConfigChangedFunc func,
                                                gpointer user_data);
void extras_menu_caffeine_config_remove_listener(guint listener_id);

G_END_DECLS

#endif /* EXTRAS_MENU_CAFFEINE_CONFIG_H */
