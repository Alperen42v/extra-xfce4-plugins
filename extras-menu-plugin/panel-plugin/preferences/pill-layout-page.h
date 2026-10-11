#ifndef EXTRAS_MENU_PILL_LAYOUT_PAGE_H
#define EXTRAS_MENU_PILL_LAYOUT_PAGE_H

#include <gtk/gtk.h>

G_BEGIN_DECLS

/* Builds the editor for where the dropdown's big buttons ("pills") sit.
 * It shows the same grid as the dropdown, one cell per pill the menu
 * shows. Drag a pill onto another to swap them, or use the arrows in a
 * cell to swap it with its neighbour in that direction. A pill's remove
 * button takes it out of the menu; "Add pill..." opens a separate window
 * listing the pills that aren't in the menu, to put them back (see
 * pill-layout-config.h for the catalog); "Reset to defaults" shows every
 * pill again in the shipped order. A "Pill shape" choice above the grid
 * switches all the pills between the shipped soft corners and GNOME-style
 * round ends; it applies to every pill and isn't touched by the reset.
 *
 * Every change is saved at once (see pill-layout-config.h) and the
 * dropdown rearranges itself immediately. The page owns all its state;
 * just put the returned widget in a container and destroy it when
 * done. */
GtkWidget *extras_menu_pill_layout_page_new(void);

G_END_DECLS

#endif /* EXTRAS_MENU_PILL_LAYOUT_PAGE_H */