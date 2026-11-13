#ifndef EXTRAS_MENU_CAFFEINE_PAGE_H
#define EXTRAS_MENU_CAFFEINE_PAGE_H

#include <gtk/gtk.h>

G_BEGIN_DECLS

/* Builds the editor for how long the Caffeine pill stays on: until it is
 * turned off (the default), or for a fixed time (15 minutes to 8 hours,
 * or a number of minutes of the user's own). The choice is saved at once
 * (see caffeine-config.h), and a Caffeine pill that is already on takes
 * the new time from that moment. The page owns all its state; just put
 * the returned widget in a container and destroy it when done. */
GtkWidget *extras_menu_caffeine_page_new(void);

G_END_DECLS

#endif /* EXTRAS_MENU_CAFFEINE_PAGE_H */
