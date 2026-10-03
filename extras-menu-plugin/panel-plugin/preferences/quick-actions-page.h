#ifndef EXTRAS_MENU_QUICK_ACTIONS_PAGE_H
#define EXTRAS_MENU_QUICK_ACTIONS_PAGE_H

#include <gtk/gtk.h>

G_BEGIN_DECLS

/* Builds the editor for the top bar's quick-action buttons: one row per
 * button (icon, name, command, move up/down, remove), an "Add button"
 * control capped at EXTRAS_MENU_QUICK_ACTIONS_MAX rows, and a reset to
 * the default buttons.
 *
 * Edits are applied live: a moment after the last change they're saved
 * (see quick-actions-config.h) and the dropdown rebuilds its buttons.
 * Anything still pending when the page is destroyed is saved then, so
 * closing the dialog never loses an edit. The page owns all its state;
 * just put the returned widget in a container and destroy it when done. */
GtkWidget *extras_menu_quick_actions_page_new(void);

G_END_DECLS

#endif /* EXTRAS_MENU_QUICK_ACTIONS_PAGE_H */