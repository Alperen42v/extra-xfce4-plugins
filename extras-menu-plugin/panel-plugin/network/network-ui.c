/*
 * The network pill and its Wi-Fi list: reacts to the network backend,
 * keeps the pill's label/icon and the list in step with it, and handles
 * clicks (radio toggle, chevron, connecting to a row). The password prompt
 * and the right-click menu live in network-dialogs.c. See network-ui.h.
 */

#include "network/network-ui.h"
#include "network/network-dialogs.h"
#include "bluetooth/bluetooth-ui.h"
#include "dropdown.h"
#include "system-status.h"

/* Opens/closes the Wi-Fi list revealer and keeps the chevron's
 * direction in step with it (pan-down while open, pan-end while
 * closed). Both widgets may not exist yet early in startup. */
void
extras_menu_network_ui_set_list_open(ExtrasMenuPlugin *plugin, gboolean open)
{
    if (plugin->network_revealer != NULL)
        gtk_revealer_set_reveal_child(GTK_REVEALER(plugin->network_revealer), open);

    if (plugin->network_expand_chevron != NULL)
    {
        gtk_image_set_from_icon_name(GTK_IMAGE(plugin->network_expand_chevron),
                                      open ? "pan-down-symbolic" : "pan-end-symbolic",
                                      GTK_ICON_SIZE_BUTTON);
    }
}

/* Applies a network status (kind + IP) to the pill's label/icon.
 * Shared by on_network_status_changed() (the normal path) and the end
 * of construct() (to catch the case where the backend's first
 * callback arrived before the widgets existed -- see
 * network_has_status in extras-menu.h for why that can happen). */
static void
apply_network_status(ExtrasMenuPlugin *plugin, ExtrasMenuNetworkKind kind)
{
    if (plugin->network_pill_label == NULL || plugin->network_pill_icon == NULL)
        return;

    switch (kind)
    {
        case EXTRAS_MENU_NETWORK_KIND_ETHERNET:
            gtk_label_set_text(GTK_LABEL(plugin->network_pill_label), "Ethernet");
            gtk_image_set_from_icon_name(GTK_IMAGE(plugin->network_pill_icon),
                                          "network-wired-symbolic", GTK_ICON_SIZE_BUTTON);

            /* The Wi-Fi list has no meaning in Ethernet mode (the
             * chevron shows an info dialog instead), so if it was open
             * when the cable came up, close it and flip the chevron
             * back rather than leaving it stuck open. */
            extras_menu_network_ui_set_list_open(plugin, FALSE);
            break;

        case EXTRAS_MENU_NETWORK_KIND_WIFI:
            gtk_label_set_text(GTK_LABEL(plugin->network_pill_label), "Wi-Fi");
            gtk_image_set_from_icon_name(GTK_IMAGE(plugin->network_pill_icon),
                                          "network-wireless-symbolic", GTK_ICON_SIZE_BUTTON);
            break;

        case EXTRAS_MENU_NETWORK_KIND_NONE:
        default:
            gtk_label_set_text(GTK_LABEL(plugin->network_pill_label), "Wi-Fi");
            gtk_image_set_from_icon_name(GTK_IMAGE(plugin->network_pill_icon),
                                          "network-wireless-offline-symbolic", GTK_ICON_SIZE_BUTTON);
            break;
    }
}

/* Called by the network backend whenever the active connection kind
 * (Wi-Fi/Ethernet/none) or its IP (Ethernet only) changes. For now
 * this only keeps the pill's label and icon in sync with reality --
 * clicking it still does nothing (that's the next step: a Wi-Fi list
 * revealer, or an Ethernet info view).
 *
 * Always caches the latest status on the plugin first -- see
 * apply_bluetooth_state()/on_bluetooth_changed() for why (GDBus can
 * invoke this before the widgets exist yet); construct() re-applies
 * the cached status once they do. */
static void
on_network_status_changed(ExtrasMenuNetworkKind kind, const gchar *ip_address, gpointer user_data)
{
    ExtrasMenuPlugin *plugin = EXTRAS_MENU_PLUGIN(user_data);

    plugin->network_last_kind = kind;
    g_free(plugin->network_last_ip_address);
    plugin->network_last_ip_address = ip_address != NULL ? g_strdup(ip_address) : NULL;
    plugin->network_has_status = TRUE;

    extras_menu_system_status_update_network_icon(plugin);
    apply_network_status(plugin, kind);
}

/* Called by the network backend once we know the Wi-Fi radio's
 * enabled state, and again whenever it changes -- whether we caused it
 * via on_network_toggle_clicked() below or something else did (a
 * hardware kill switch, another app, nmcli). Keeps network_toggle's
 * checked state in sync with reality. */
static void
on_wifi_enabled_changed(gboolean enabled, gpointer user_data)
{
    ExtrasMenuPlugin *plugin = EXTRAS_MENU_PLUGIN(user_data);

    plugin->network_wifi_last_enabled = enabled;
    plugin->network_wifi_has_enabled_state = TRUE;

    extras_menu_airplane_mode_wifi_changed(plugin->airplane_mode, enabled);

    extras_menu_system_status_update_network_icon(plugin);

    if (plugin->network_toggle == NULL)
        return;

    /* See on_audio_changed() for why this flag exists -- same
     * feedback-loop concern, this time against NetworkManager's
     * WirelessEnabled. */
    plugin->updating_wifi_enabled_from_backend = TRUE;
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(plugin->network_toggle), enabled);
    plugin->updating_wifi_enabled_from_backend = FALSE;
}

/* Fired when the main part of the network pill (icon + label) is
 * clicked -- turns the Wi-Fi radio itself on/off, independent of any
 * specific connection. Does nothing if the radio state hasn't been
 * heard from the backend yet (network_wifi_has_enabled_state FALSE),
 * since we wouldn't know what "on/off" even means at that point. */
static void
on_network_toggle_clicked(GtkToggleButton *button, ExtrasMenuPlugin *plugin)
{
    if (plugin->updating_wifi_enabled_from_backend)
        return;

    extras_menu_network_set_wifi_enabled(plugin->network, gtk_toggle_button_get_active(button));
}

/* Fired when the small chevron button on the network pill is clicked.
 * An open list is always closed first. Otherwise: with Ethernet as the
 * active connection, a small info dialog with the IP address is shown
 * (Ethernet has nothing to "choose" the way Wi-Fi networks do); with
 * the Wi-Fi radio off, a short dialog says so; in every other case --
 * connected to Wi-Fi *or not connected at all* -- the Wi-Fi list is
 * opened, since being disconnected is exactly when one needs it. */
static void
on_network_expand_clicked(GtkButton *button, ExtrasMenuPlugin *plugin)
{
    (void) button;

    gboolean currently_open =
        plugin->network_revealer != NULL &&
        gtk_revealer_get_reveal_child(GTK_REVEALER(plugin->network_revealer));

    /* An open list can always be closed, whatever the connection kind
     * is right now -- otherwise a list left open when the kind changed
     * (Wi-Fi radio switched off, cable plugged in, ...) could never be
     * dismissed, since the branches below would only show a dialog. */
    if (currently_open)
    {
        extras_menu_network_ui_set_list_open(plugin, FALSE);
        return;
    }

    /* Ethernet is the active connection: nothing to pick between on a
     * wired connection, so show a simple info dialog instead of a list. */
    if (plugin->network_last_kind == EXTRAS_MENU_NETWORK_KIND_ETHERNET)
    {
        gchar *details = g_strdup_printf(
            "IP address: %s",
            plugin->network_last_ip_address != NULL ? plugin->network_last_ip_address
                                                      : "Not yet assigned");
        extras_menu_dropdown_show_message(plugin, GTK_MESSAGE_INFO, "Connected via Ethernet", details);
        g_free(details);
        return;
    }

    /* Everything else: show the Wi-Fi list as long as the radio is on.
     * This deliberately includes "not connected to anything" (kind
     * NONE) -- that is exactly when the user needs the list, to pick a
     * network to connect to. If the radio state hasn't been reported by
     * the backend yet, assume it's on rather than locking the list
     * away. */
    gboolean wifi_on = !plugin->network_wifi_has_enabled_state ||
                       plugin->network_wifi_last_enabled;

    if (!wifi_on)
    {
        extras_menu_dropdown_show_message(plugin, GTK_MESSAGE_INFO, "Wi-Fi is turned off",
                            "Turn Wi-Fi on to see available networks.");
        return;
    }

    if (plugin->network_revealer == NULL)
        return;

    /* Only one of the two lists is open at a time. */
    extras_menu_bluetooth_ui_set_list_open(plugin, FALSE);
    extras_menu_network_ui_set_list_open(plugin, TRUE);

    /* Ask for a fresh scan each time the list is opened, so it's
     * not showing stale results from whenever the dropdown last
     * happened to scan. */
    extras_menu_network_rescan(plugin->network);
}

/* Fired once extras_menu_network_connect() (below) finishes, whether
 * it succeeded or not.
 *
 * On failure: if the network we just tried is secured and we don't
 * yet have a password for it in this attempt (i.e. this was the
 * initial no-password try from on_network_row_activated(), looking
 * for a saved profile), fall back to prompting for a password rather
 * than just showing an error -- this is what lets
 * on_network_row_activated() try silently first and only bother the
 * user if that didn't work. If the network is open, or we already
 * tried with a user-provided password and that failed too, or it's an
 * enterprise network (a password can't help there), show the error
 * instead of looping back into another password prompt.
 *
 * pending_connect_active stays set across the password fallback and is
 * cleared on every final outcome (success, or the error being shown). */
void
extras_menu_network_ui_connect_result(gboolean success, const gchar *error_message, gpointer user_data)
{
    ExtrasMenuPlugin *plugin = EXTRAS_MENU_PLUGIN(user_data);

    if (success)
    {
        /* The row's label is set to "Connected" here rather than left
         * to the backend's next list refresh: that refresh does come
         * (NetworkManager reports the new active AP), but not always
         * immediately, and leaving the row stuck on "Connecting..." in
         * the meantime looks like the attempt hung. */
        plugin->pending_connect_active = FALSE;
        extras_menu_network_ui_set_pill_connecting(plugin, FALSE);
        extras_menu_network_ui_set_row_status(plugin, plugin->pending_connect_ssid, "Connected");
        return;
    }

    /* Enterprise (802.1X) networks are excluded from the password
     * fallback: they can't be set up with a password alone, so the
     * backend refuses to create a profile for one and says why. Asking
     * for a password anyway would only lead to the same refusal after
     * the user typed it in -- show the explanation straight away. */
    if (plugin->pending_connect_secured &&
        !plugin->pending_connect_password_was_tried &&
        plugin->pending_connect_security != EXTRAS_MENU_AP_SECURITY_ENTERPRISE)
    {
        /* Still mid-attempt (about to prompt for a password), so the
         * spinner stays running -- only the row's "Connecting..." text
         * is cleared, since the user is about to be asked for input
         * rather than actively waiting on the network. */
        extras_menu_network_ui_set_row_status(plugin, plugin->pending_connect_ssid, NULL);

        plugin->pending_connect_password_was_tried = TRUE;
        extras_menu_network_dialogs_prompt_password(plugin, plugin->pending_connect_ssid,
                                     GTK_WINDOW(plugin->popover));
        return;
    }

    plugin->pending_connect_active = FALSE;
    extras_menu_network_ui_set_pill_connecting(plugin, FALSE);
    extras_menu_network_ui_set_row_status(plugin, plugin->pending_connect_ssid, NULL);

    extras_menu_dropdown_show_message(plugin, GTK_MESSAGE_ERROR,
                        "Couldn't connect to the network.",
                        error_message != NULL ? error_message : "Unknown error");
}

/* Updates a network row's status label (found via
 * plugin->network_row_by_ssid) to either "Connecting...", "Connected",
 * or blank/hidden (idle -- neither connecting nor the active network).
 * text may be NULL for the idle case. Safe to call for an SSID that
 * isn't currently in the list (e.g. it scrolled out during a rescan
 * mid-attempt) -- becomes a no-op rather than crashing. */
void
extras_menu_network_ui_set_row_status(ExtrasMenuPlugin *plugin, const gchar *ssid, const gchar *text)
{
    if (plugin->network_row_by_ssid == NULL || ssid == NULL)
        return;

    GtkWidget *row = g_hash_table_lookup(plugin->network_row_by_ssid, ssid);
    if (row == NULL)
        return;

    GtkWidget *status_label = g_object_get_data(G_OBJECT(row), "extras-menu-status-label");
    if (status_label == NULL)
        return;

    gtk_label_set_text(GTK_LABEL(status_label), text != NULL ? text : "");
    gtk_widget_set_visible(status_label, text != NULL);
}

/* Shows/hides the spinner in place of the network pill's leading icon
 * -- used to indicate a connection attempt is in progress. Safe to
 * call even if the icon stack/spinner widgets don't exist yet (no-op),
 * matching the defensive style used elsewhere for widgets that might
 * not be built yet when a backend callback fires early. */
void
extras_menu_network_ui_set_pill_connecting(ExtrasMenuPlugin *plugin, gboolean connecting)
{
    if (plugin->network_icon_stack == NULL || plugin->network_spinner == NULL)
        return;

    if (connecting)
    {
        gtk_spinner_start(GTK_SPINNER(plugin->network_spinner));
        gtk_stack_set_visible_child_name(GTK_STACK(plugin->network_icon_stack), "spinner");
    }
    else
    {
        gtk_stack_set_visible_child_name(GTK_STACK(plugin->network_icon_stack), "icon");
        gtk_spinner_stop(GTK_SPINNER(plugin->network_spinner));
    }
}

/* Fired when the user clicks a network row in the Wi-Fi list. Open
 * networks connect immediately. Secured networks also try to connect
 * immediately first -- extras_menu_network_connect() itself looks for
 * an already-saved profile (which includes the password) before
 * asking us for one, so a network we've connected to before (or are
 * currently on) reconnects without any prompt. Only if that attempt
 * fails (no saved profile existed, so NetworkManager had nothing to
 * authenticate with) do we fall back to prompting for a password --
 * see extras_menu_network_ui_connect_result() above, which is where that fallback
 * actually happens. */
static void
on_network_row_activated(GtkListBox *list_box, GtkListBoxRow *row, gpointer user_data)
{
    ExtrasMenuPlugin *plugin = EXTRAS_MENU_PLUGIN(user_data);
    (void) list_box;

    const gchar *ssid = g_object_get_data(G_OBJECT(row), "extras-menu-ssid");
    gboolean secured = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(row), "extras-menu-secured"));

    if (ssid == NULL)
        return;

    /* One attempt at a time. A second one started on top of a running
     * one would overwrite pending_connect_ssid (the first attempt's
     * result would then be applied to the wrong row) and a double-click
     * could create the same profile twice. The pill's spinner already
     * shows that something is in progress. */
    if (plugin->pending_connect_active)
        return;

    ExtrasMenuApSecurity security =
        (ExtrasMenuApSecurity) GPOINTER_TO_INT(g_object_get_data(G_OBJECT(row), "extras-menu-security"));

    /* Stash whether this network is secured (and how) on the plugin,
     * keyed by the SSID we're attempting -- extras_menu_network_ui_connect_result() needs
     * this to decide whether a failure should fall back to a password
     * prompt (secured network, no saved profile) or just show an error
     * (open network, enterprise network, something else went wrong). */
    g_free(plugin->pending_connect_ssid);
    plugin->pending_connect_ssid = g_strdup(ssid);
    plugin->pending_connect_secured = secured;
    plugin->pending_connect_security = security;
    plugin->pending_connect_password_was_tried = FALSE;
    plugin->pending_connect_prompting = FALSE;
    plugin->pending_connect_active = TRUE;

    extras_menu_network_ui_set_row_status(plugin, ssid, "Connecting\xE2\x80\xA6"); /* "Connecting…" */
    extras_menu_network_ui_set_pill_connecting(plugin, TRUE);

    extras_menu_network_connect(plugin->network, ssid, NULL, secured, extras_menu_network_ui_connect_result, plugin);
}

/* Builds one GtkListBoxRow for an access point: signal-strength icon,
 * SSID, a lock icon if secured, and a status label ("Connected" /
 * "Connecting…"). The SSID and secured flag are stashed as object data
 * so on_network_row_activated() can read them back without needing a
 * parallel data structure to look rows up in, along with a fuller
 * ExtrasMenuNetworkApDetails copy for the right-click menu's details dialog. */
static GtkWidget *
make_network_row(const ExtrasMenuAccessPoint *ap)
{
    GtkWidget *row_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(row_box), 6);

    const gchar *strength_icon;
    if (ap->strength < 25)
        strength_icon = "network-wireless-signal-weak-symbolic";
    else if (ap->strength < 50)
        strength_icon = "network-wireless-signal-ok-symbolic";
    else if (ap->strength < 75)
        strength_icon = "network-wireless-signal-good-symbolic";
    else
        strength_icon = "network-wireless-signal-excellent-symbolic";

    GtkWidget *signal_icon = gtk_image_new_from_icon_name(strength_icon, GTK_ICON_SIZE_BUTTON);
    gtk_box_pack_start(GTK_BOX(row_box), signal_icon, FALSE, FALSE, 0);

    GtkWidget *label = gtk_label_new(ap->ssid);
    gtk_label_set_xalign(GTK_LABEL(label), 0.0);
    gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
    gtk_widget_set_hexpand(label, TRUE);
    gtk_box_pack_start(GTK_BOX(row_box), label, TRUE, TRUE, 0);

    if (ap->secured)
    {
        GtkWidget *lock_icon = gtk_image_new_from_icon_name("network-wireless-encrypted-symbolic",
                                                              GTK_ICON_SIZE_BUTTON);
        gtk_box_pack_start(GTK_BOX(row_box), lock_icon, FALSE, FALSE, 0);
    }

    /* Status label ("Connecting..."/"Connected"), always created (even
     * for rows that start neither) so on_network_row_activated() and
     * extras_menu_network_ui_connect_result() can find and update it later via
     * g_object_get_data() without having to rebuild the row -- only
     * its text and visibility change as the connection attempt
     * progresses. */
    GtkWidget *status_label = gtk_label_new(ap->is_active ? "Connected" : "");
    gtk_style_context_add_class(gtk_widget_get_style_context(status_label), "dim-label");
    gtk_widget_set_no_show_all(status_label, TRUE); /* so show_all() below doesn't force it visible */
    gtk_widget_set_visible(status_label, ap->is_active);
    gtk_box_pack_start(GTK_BOX(row_box), status_label, FALSE, FALSE, 0);

    GtkWidget *row = gtk_list_box_row_new();
    gtk_container_add(GTK_CONTAINER(row), row_box);

    g_object_set_data_full(G_OBJECT(row), "extras-menu-ssid", g_strdup(ap->ssid), g_free);
    g_object_set_data(G_OBJECT(row), "extras-menu-secured", GINT_TO_POINTER(ap->secured));
    g_object_set_data(G_OBJECT(row), "extras-menu-security", GINT_TO_POINTER((gint) ap->security));
    g_object_set_data(G_OBJECT(row), "extras-menu-status-label", status_label);
    g_object_set_data_full(G_OBJECT(row), "extras-menu-ap-details",
                            extras_menu_network_ap_details_new(ap), extras_menu_network_ap_details_free);

    gtk_widget_show_all(row);
    return row;
}

/* Called by the network backend whenever the visible Wi-Fi network
 * list changes (initial scan, periodic rescans, a network
 * appearing/disappearing, or the active network changing). Clears and
 * rebuilds the list box from scratch each time -- simpler than
 * diffing, and scan updates are infrequent enough that this is cheap. */
static void
on_network_list_changed(gboolean wifi_available, const ExtrasMenuAccessPoint *aps,
                         guint count, gpointer user_data)
{
    ExtrasMenuPlugin *plugin = EXTRAS_MENU_PLUGIN(user_data);

    if (plugin->network_list_box == NULL)
        return;

    GList *existing_rows = gtk_container_get_children(GTK_CONTAINER(plugin->network_list_box));
    for (GList *l = existing_rows; l != NULL; l = l->next)
        gtk_widget_destroy(GTK_WIDGET(l->data));
    g_list_free(existing_rows);

    if (plugin->network_row_by_ssid != NULL)
        g_hash_table_remove_all(plugin->network_row_by_ssid);

    if (!wifi_available)
        return;

    for (guint i = 0; i < count; i++)
    {
        GtkWidget *row = make_network_row(&aps[i]);
        gtk_list_box_insert(GTK_LIST_BOX(plugin->network_list_box), row, -1);
        if (plugin->network_row_by_ssid != NULL)
            g_hash_table_insert(plugin->network_row_by_ssid, g_strdup(aps[i].ssid), row);
    }

    /* The rows above are brand new, so the one for the network being
     * connected to has lost its "Connecting..." text -- and may even
     * say "Connected" already, since NetworkManager reports the AP as
     * active while the attempt is still running. Put the right text
     * back, except while the password dialog is open (then the row
     * shows nothing, as set by extras_menu_network_ui_connect_result()). */
    if (plugin->pending_connect_active && !plugin->pending_connect_prompting)
        extras_menu_network_ui_set_row_status(plugin, plugin->pending_connect_ssid, "Connecting\xE2\x80\xA6"); /* "Connecting…" */
}

/* Creates the SSID -> row lookup, starts the network backend and wires the
 * pill and the list to it. Needs the dropdown content to exist. */
void
extras_menu_network_ui_attach(ExtrasMenuPlugin *plugin)
{
    /* SSID -> GtkListBoxRow lookup for the network list, used by
     * on_network_row_activated()/extras_menu_network_ui_connect_result() to update a
     * specific row's status label without a linear search. */
    plugin->network_row_by_ssid = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);

    /* --- network backend: finds the Wi-Fi/Ethernet devices async,
     * reports the active connection kind (and Ethernet IP) through
     * on_network_status_changed, the Wi-Fi radio's own on/off state
     * through on_wifi_enabled_changed, and the visible Wi-Fi network
     * list through on_network_list_changed. The pill's label/icon
     * track the connection kind; clicking the main toggle turns Wi-Fi
     * on/off, clicking the chevron reveals the Wi-Fi list (or shows an
     * Ethernet info dialog); clicking a network row connects to it. --- */
    plugin->network = extras_menu_network_new(on_network_list_changed,
                                               on_network_status_changed,
                                               on_wifi_enabled_changed, plugin);

    /* Catch up on any status the backend already reported before
     * these widgets existed -- see network_has_status/
     * network_wifi_has_enabled_state in extras-menu.h for why that can
     * happen. */
    if (plugin->network_has_status)
        apply_network_status(plugin, plugin->network_last_kind);
    if (plugin->network_wifi_has_enabled_state && plugin->network_toggle != NULL)
    {
        plugin->updating_wifi_enabled_from_backend = TRUE;
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(plugin->network_toggle),
                                      plugin->network_wifi_last_enabled);
        plugin->updating_wifi_enabled_from_backend = FALSE;
    }

    if (plugin->network_toggle != NULL)
    {
        g_signal_connect(plugin->network_toggle, "toggled",
                          G_CALLBACK(on_network_toggle_clicked), plugin);
    }

    if (plugin->network_expand_button != NULL)
    {
        g_signal_connect(plugin->network_expand_button, "clicked",
                          G_CALLBACK(on_network_expand_clicked), plugin);
    }

    if (plugin->network_list_box != NULL)
    {
        g_signal_connect(plugin->network_list_box, "row-activated",
                          G_CALLBACK(on_network_row_activated), plugin);

        /* Right-click context menu (forget / details). Handled on the
         * list box rather than per row -- see
         * extras_menu_network_dialogs_on_list_button_press() for why. */
        gtk_widget_add_events(plugin->network_list_box, GDK_BUTTON_PRESS_MASK);
        g_signal_connect(plugin->network_list_box, "button-press-event",
                          G_CALLBACK(extras_menu_network_dialogs_on_list_button_press), plugin);
    }
}

/* Stops the network backend and releases what this module keeps on the
 * plugin. */
void
extras_menu_network_ui_free(ExtrasMenuPlugin *plugin)
{
    if (plugin->network != NULL)
    {
        extras_menu_network_free(plugin->network);
        plugin->network = NULL;
    }

    g_free(plugin->network_last_ip_address);
    plugin->network_last_ip_address = NULL;

    g_free(plugin->pending_connect_ssid);
    plugin->pending_connect_ssid = NULL;

    if (plugin->network_row_by_ssid != NULL)
    {
        g_hash_table_destroy(plugin->network_row_by_ssid);
        plugin->network_row_by_ssid = NULL;
    }
}