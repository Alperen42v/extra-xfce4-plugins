#ifndef EXTRAS_MENU_QUICK_ACTIONS_H
#define EXTRAS_MENU_QUICK_ACTIONS_H

#include <gtk/gtk.h>

G_BEGIN_DECLS

/* Called right before a quick action launches its program, so the
 * caller can close the dropdown first -- otherwise the dropdown would
 * stay on screen (and even show up in screenshots). */
typedef void (*ExtrasMenuQuickActionsCloseFunc)(gpointer user_data);

/*
 * Fills the dropdown's top-bar button container with the user's
 * quick-action buttons (see preferences/quick-actions-config.h for the
 * list and where it's stored), and keeps it up to date: whenever the
 * preferences editor saves a change, the container's buttons are
 * rebuilt on the spot.
 *
 * `box` is the GtkBox popover.c leaves empty for this purpose (may be
 * NULL, in which case nothing happens). Each button shows its
 * configured icon, uses its name as the tooltip and, when clicked,
 * runs its configured command. Buttons whose command is empty are
 * skipped. The live-update hookup is dropped automatically when `box`
 * is destroyed.
 *
 * close_func (may be NULL) is invoked with close_data before each
 * launch. Everything about the buttons' behavior lives in
 * quick-actions.c, so extras-menu.c only needs this one call from
 * construct().
 */
void extras_menu_quick_actions_attach(GtkWidget *box,
                                       ExtrasMenuQuickActionsCloseFunc close_func,
                                       gpointer close_data);

G_END_DECLS

#endif /* EXTRAS_MENU_QUICK_ACTIONS_H */