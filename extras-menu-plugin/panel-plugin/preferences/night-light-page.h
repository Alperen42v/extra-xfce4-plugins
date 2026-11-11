#ifndef EXTRAS_MENU_NIGHT_LIGHT_PAGE_H
#define EXTRAS_MENU_NIGHT_LIGHT_PAGE_H

#include <gtk/gtk.h>

G_BEGIN_DECLS

/* Builds the editor for the Night Light pill's settings: how warm the
 * screen gets (color temperature) and how long the move to that
 * temperature, and back, takes (transition time). "Reset to defaults"
 * puts both back to the shipped values.
 *
 * Changes are saved shortly after the slider stops moving (see
 * night-light-config.h), and the pill applies them at once. The page
 * owns all its state; just put the returned widget in a container and
 * destroy it when done. */
GtkWidget *extras_menu_night_light_page_new(void);

G_END_DECLS

#endif /* EXTRAS_MENU_NIGHT_LIGHT_PAGE_H */