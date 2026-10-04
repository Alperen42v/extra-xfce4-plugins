#ifndef EXTRAS_MENU_HOME_PAGE_H
#define EXTRAS_MENU_HOME_PAGE_H

#include <gtk/gtk.h>

G_BEGIN_DECLS

/* Builds the preferences dialog's landing page: what Extras Menu is,
 * which version is installed, a pointer to the sections in the
 * sidebar, and at the bottom the licence (GPL-3.0) and a link to the
 * project's GitHub page. Purely informational -- there is nothing to configure here,
 * so it has no state and nothing to clean up beyond the widget. */
GtkWidget *extras_menu_home_page_new(void);

G_END_DECLS

#endif /* EXTRAS_MENU_HOME_PAGE_H */