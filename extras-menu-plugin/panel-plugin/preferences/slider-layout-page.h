#ifndef EXTRAS_MENU_SLIDER_LAYOUT_PAGE_H
#define EXTRAS_MENU_SLIDER_LAYOUT_PAGE_H

#include <gtk/gtk.h>

G_BEGIN_DECLS

/* Builds the editor for the dropdown's sliders (volume, brightness):
 * one row per slider the menu shows, in the same top-to-bottom order as
 * the dropdown. The arrows in a row move that slider up or down, its
 * remove button takes it out of the menu, and "Add slider..." opens a
 * separate window listing the sliders that aren't in the menu, to put
 * them back (see slider-layout-config.h for the catalog). "Reset to
 * defaults" shows every slider again in the shipped order.
 *
 * Every change is saved at once and the dropdown rearranges itself
 * immediately. The page owns all its state; just put the returned
 * widget in a container and destroy it when done. */
GtkWidget *extras_menu_slider_layout_page_new(void);

G_END_DECLS

#endif /* EXTRAS_MENU_SLIDER_LAYOUT_PAGE_H */