#ifndef EXTRAS_MENU_NETWORK_UI_H
#define EXTRAS_MENU_NETWORK_UI_H

#include "extras-menu.h"

G_BEGIN_DECLS

/* Creates the SSID -> row lookup, starts the network backend and connects
 * the network pill (radio toggle, chevron) and its Wi-Fi list to it. Needs
 * the dropdown content to exist. Catches up on anything the backend
 * reported before the widgets existed. */
void extras_menu_network_ui_attach(ExtrasMenuPlugin *plugin);

/* Stops the network backend and releases what this module keeps on the
 * plugin. */
void extras_menu_network_ui_free(ExtrasMenuPlugin *plugin);

/* Opens/closes the Wi-Fi list and keeps the chevron's direction in step
 * with it. Safe to call before the widgets exist. */
void extras_menu_network_ui_set_list_open(ExtrasMenuPlugin *plugin, gboolean open);

/* Sets a network row's status label ("Connecting...", "Connected", or
 * NULL for none). A no-op for an SSID that isn't in the list. */
void extras_menu_network_ui_set_row_status(ExtrasMenuPlugin *plugin, const gchar *ssid,
                                            const gchar *text);

/* Shows/hides the spinner in place of the pill's icon while a connection
 * attempt is running. */
void extras_menu_network_ui_set_pill_connecting(ExtrasMenuPlugin *plugin, gboolean connecting);

/* Result callback for extras_menu_network_connect() (user_data is the
 * ExtrasMenuPlugin). On failure it falls back to the password prompt for
 * a secured network, or shows the error. */
void extras_menu_network_ui_connect_result(gboolean success, const gchar *error_message,
                                            gpointer user_data);

G_END_DECLS

#endif /* EXTRAS_MENU_NETWORK_UI_H */