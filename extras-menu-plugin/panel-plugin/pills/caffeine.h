#ifndef EXTRAS_MENU_CAFFEINE_H
#define EXTRAS_MENU_CAFFEINE_H

#include <gtk/gtk.h>

G_BEGIN_DECLS

/*
 * The Caffeine pill's logic: while it is on, the screen stays on and the
 * computer doesn't go to sleep by itself.
 *
 * It does this the way a media player does, by asking the session's
 * services not to idle: it "inhibits" both the screensaver
 * (org.freedesktop.ScreenSaver, falling back to Xfce's and GNOME's own
 * names for it) and the power manager
 * (org.freedesktop.PowerManagement.Inhibit, which xfce4-power-manager
 * provides). Either one being accepted is enough for the pill to turn
 * on. Both requests are tied to this process's connection to the
 * session bus, so if the panel dies the services let go by themselves.
 *
 * The pill always starts off. How long it stays on is a user setting
 * (see preferences/caffeine-config.h): by default until it is clicked
 * off again, otherwise for that many minutes, after which it turns
 * itself off. If the setting is changed while the pill is on, the
 * countdown starts again from that moment.
 *
 * Like the other pill modules, it owns all of its state and keeps hold
 * of the pill itself, so the plugin needs nothing but the one pointer.
 */
typedef struct _ExtrasMenuCaffeine ExtrasMenuCaffeine;

/* How the module shows a warning (the plugin shows it in a dialog parented
 * to the dropdown). `secondary` may be NULL. */
typedef void (*ExtrasMenuCaffeineMessageFunc)(const gchar *primary,
                                               const gchar *secondary,
                                               gpointer user_data);

/* `pill` is the Caffeine pill's GtkToggleButton, as handed out by
 * extras_menu_popover_get_pill(content, EXTRAS_MENU_PILL_CAFFEINE).
 * Returns NULL if `pill` isn't a toggle button. */
ExtrasMenuCaffeine *extras_menu_caffeine_new(GtkWidget *pill,
                                              ExtrasMenuCaffeineMessageFunc message_func,
                                              gpointer user_data);

/* The icon the Caffeine pill shows while it is `active` (on) or not.
 * If the icon theme has both "caffeine-cup-full" and "caffeine-cup-empty"
 * it is the full cup while on and the empty one while off; if either is
 * missing, it is the same stock icon for both states. The pill, the
 * module and the preferences editor all ask this, so they agree. The
 * returned string is a constant; don't free it. */
const gchar *extras_menu_caffeine_icon_name(gboolean active);

/* Turns Caffeine off (letting the screensaver and power manager go),
 * and disconnects from the pill. NULL is allowed. */
void extras_menu_caffeine_free(ExtrasMenuCaffeine *self);

G_END_DECLS

#endif /* EXTRAS_MENU_CAFFEINE_H */
