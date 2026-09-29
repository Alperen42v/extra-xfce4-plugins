#ifndef EXTRAS_MENU_QUICK_ACTIONS_H
#define EXTRAS_MENU_QUICK_ACTIONS_H

#include <gtk/gtk.h>

G_BEGIN_DECLS

/* Called right before a quick action launches its program, so the
 * caller can close the dropdown first -- otherwise the dropdown would
 * stay on screen (and even show up in screenshots). */
typedef void (*ExtrasMenuQuickActionsCloseFunc)(gpointer user_data);

/*
 * Wires up the four quick-action buttons in the popover's top bar.
 * Any button argument may be NULL.
 *
 *   screenshot_button -> xfce4-screenshooter (its own capture dialog)
 *   settings_button   -> xfce4-settings-manager
 *   lock_button       -> xflock4 (XFCE's lock wrapper; uses whichever
 *                        locker is installed: xfce4-screensaver,
 *                        light-locker, xscreensaver, ...)
 *   power_button      -> xfce4-session-logout (logout/restart/shutdown)
 *
 * close_func (may be NULL) is invoked with close_data before each
 * launch. Every button's behavior lives in quick-actions.c, so
 * extras-menu.c only needs this one call from construct().
 */
void extras_menu_quick_actions_connect(GtkWidget *screenshot_button,
                                        GtkWidget *settings_button,
                                        GtkWidget *lock_button,
                                        GtkWidget *power_button,
                                        ExtrasMenuQuickActionsCloseFunc close_func,
                                        gpointer close_data);

G_END_DECLS

#endif /* EXTRAS_MENU_QUICK_ACTIONS_H */