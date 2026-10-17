#ifndef EXTRAS_MENU_NETWORK_DIALOGS_H
#define EXTRAS_MENU_NETWORK_DIALOGS_H

#include "extras-menu.h"
#include "network/network.h"

G_BEGIN_DECLS

/* An owned copy of the parts of an ExtrasMenuAccessPoint we want to
 * keep around after the backend's callback returns (its own strings
 * are only valid for the duration of that call), stashed on each row
 * so the right-click menu and details dialog can read them later. */
typedef struct
{
    gchar *ssid;
    gchar *bssid;
    gint8 strength;
    gboolean secured;
    gboolean is_active;
    ExtrasMenuApSecurity security;
    guint32 frequency;
    guint32 max_bitrate;
} ExtrasMenuNetworkApDetails;

/* Makes an owned copy of what a row needs to remember about an access
 * point (the backend's own strings die when its callback returns). Free
 * with extras_menu_network_ap_details_free(), which can be used directly
 * as a GDestroyNotify. */
ExtrasMenuNetworkApDetails *extras_menu_network_ap_details_new(const ExtrasMenuAccessPoint *ap);
void extras_menu_network_ap_details_free(gpointer data);

/* Prompts for a Wi-Fi password with a small modal dialog and returns
 * immediately; the connect attempt (or the cancel handling) happens once
 * the user answers. parent_window anchors the dialog. */
void extras_menu_network_dialogs_prompt_password(ExtrasMenuPlugin *plugin, const gchar *ssid,
                                                  GtkWindow *parent_window);

/* "button-press-event" handler for the Wi-Fi list box: a right-click on a
 * row opens a menu to forget the network or show its details. user_data
 * is the ExtrasMenuPlugin. Each row must carry an
 * ExtrasMenuNetworkApDetails as object data "extras-menu-ap-details". */
gboolean extras_menu_network_dialogs_on_list_button_press(GtkWidget *list_box,
                                                           GdkEventButton *event,
                                                           gpointer user_data);

G_END_DECLS

#endif /* EXTRAS_MENU_NETWORK_DIALOGS_H */