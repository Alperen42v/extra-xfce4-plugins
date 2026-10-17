/*
 * Dialogs and menus of the Wi-Fi list: the password prompt, the
 * right-click menu (forget / details) and the network details dialog.
 * See network-dialogs.h.
 */

#include "network/network-dialogs.h"
#include "network/network-ui.h"
#include "dropdown.h"

ExtrasMenuNetworkApDetails *
extras_menu_network_ap_details_new(const ExtrasMenuAccessPoint *ap)
{
    ExtrasMenuNetworkApDetails *details = g_new0(ExtrasMenuNetworkApDetails, 1);
    details->ssid = g_strdup(ap->ssid);
    details->bssid = ap->bssid != NULL ? g_strdup(ap->bssid) : NULL;
    details->strength = ap->strength;
    details->secured = ap->secured;
    details->is_active = ap->is_active;
    details->security = ap->security;
    details->frequency = ap->frequency;
    details->max_bitrate = ap->max_bitrate;
    return details;
}

void
extras_menu_network_ap_details_free(gpointer data)
{
    ExtrasMenuNetworkApDetails *details = data;
    if (details == NULL)
        return;

    g_free(details->ssid);
    g_free(details->bssid);
    g_free(details);
}

/* Independent deep copy of a row's details. The right-click menu gives
 * each of its items its own copy rather than pointing at the row's:
 * on_network_list_changed() destroys and rebuilds every row on each
 * scan update, which can happen while the menu is still open, and a
 * borrowed pointer would then dangle. */
static ExtrasMenuNetworkApDetails *
ap_details_copy(const ExtrasMenuNetworkApDetails *src)
{
    ExtrasMenuNetworkApDetails *copy = g_new0(ExtrasMenuNetworkApDetails, 1);
    copy->ssid = g_strdup(src->ssid);
    copy->bssid = src->bssid != NULL ? g_strdup(src->bssid) : NULL;
    copy->strength = src->strength;
    copy->secured = src->secured;
    copy->is_active = src->is_active;
    copy->security = src->security;
    copy->frequency = src->frequency;
    copy->max_bitrate = src->max_bitrate;
    return copy;
}

/* What the password dialog's response handler needs once the user
 * answers. Owned by the dialog (freed with it), so it lives exactly as
 * long as the dialog does. */
typedef struct
{
    ExtrasMenuPlugin *plugin;
    gchar *ssid;
    GtkWidget *entry;
} PasswordPromptCtx;

static void
password_prompt_ctx_free(gpointer data)
{
    PasswordPromptCtx *ctx = data;
    g_free(ctx->ssid);
    g_free(ctx);
}

static void
on_password_dialog_response(GtkDialog *dialog, gint response_id, gpointer user_data)
{
    PasswordPromptCtx *ctx = user_data;
    ExtrasMenuPlugin *plugin = ctx->plugin;

    plugin->pending_connect_prompting = FALSE;

    if (response_id == GTK_RESPONSE_OK)
    {
        /* extras_menu_network_connect() copies the password, so it is
         * fine for the entry (and this dialog) to go away right after. */
        const gchar *password = gtk_entry_get_text(GTK_ENTRY(ctx->entry));
        extras_menu_network_ui_set_row_status(plugin, ctx->ssid, "Connecting\xE2\x80\xA6"); /* "Connecting…" */
        extras_menu_network_connect(plugin->network, ctx->ssid, password, TRUE,
                                     extras_menu_network_ui_connect_result, plugin);
    }
    else
    {
        /* Cancelled (or closed with Escape / the window button) -- stop
         * the spinner that on_network_row_activated() started,
         * otherwise it would keep spinning forever with no attempt
         * actually in flight, and let the user pick a network again. */
        plugin->pending_connect_active = FALSE;
        extras_menu_network_ui_set_pill_connecting(plugin, FALSE);
        extras_menu_network_ui_set_row_status(plugin, ctx->ssid, NULL);
    }

    gtk_widget_destroy(GTK_WIDGET(dialog));
}

/* Prompts for a Wi-Fi password with a small modal dialog and returns
 * immediately; the connect attempt (or the cancel handling) happens in
 * on_password_dialog_response() once the user answers. parent_window
 * anchors the password dialog. */
void
extras_menu_network_dialogs_prompt_password(ExtrasMenuPlugin *plugin, const gchar *ssid, GtkWindow *parent_window)
{
    GtkWidget *dialog = gtk_dialog_new_with_buttons(
        "Wi-Fi Password",
        parent_window,
        GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
        "_Cancel", GTK_RESPONSE_CANCEL,
        "C_onnect", GTK_RESPONSE_OK,
        NULL);

    gtk_dialog_set_default_response(GTK_DIALOG(dialog), GTK_RESPONSE_OK);
    gtk_container_set_border_width(GTK_CONTAINER(dialog), 12);

    GtkWidget *content_area = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(box), 8);
    gtk_container_add(GTK_CONTAINER(content_area), box);

    gchar *prompt_text = g_strdup_printf("Enter the password for \u201c%s\u201d:", ssid);
    GtkWidget *label = gtk_label_new(prompt_text);
    g_free(prompt_text);
    gtk_label_set_xalign(GTK_LABEL(label), 0.0);
    gtk_box_pack_start(GTK_BOX(box), label, FALSE, FALSE, 0);

    GtkWidget *entry = gtk_entry_new();
    gtk_entry_set_visibility(GTK_ENTRY(entry), FALSE); /* mask the password */
    gtk_entry_set_activates_default(GTK_ENTRY(entry), TRUE); /* Enter submits */
    gtk_box_pack_start(GTK_BOX(box), entry, FALSE, FALSE, 0);

    PasswordPromptCtx *ctx = g_new0(PasswordPromptCtx, 1);
    ctx->plugin = plugin;
    ctx->ssid = g_strdup(ssid);
    ctx->entry = entry;
    g_object_set_data_full(G_OBJECT(dialog), "extras-menu-password-ctx",
                            ctx, password_prompt_ctx_free);

    plugin->pending_connect_prompting = TRUE;
    extras_menu_dropdown_track_dialog(plugin, dialog);
    g_signal_connect(dialog, "response", G_CALLBACK(on_password_dialog_response), ctx);

    gtk_widget_show_all(dialog);
}

/* Colour used for a security scheme's badge in the details dialog,
 * roughly "newer/stronger is cooler-toned": WPA3 purple, WPA2 green,
 * older WPA orange, WEP and Open red (both effectively unprotected by
 * modern standards). Returned as a hex string for Pango markup.
 * Deliberately fixed colours rather than theme ones -- the point of
 * the badge is that WPA2 looks the same shade everywhere, the way a
 * status colour should. */
static const gchar *
security_badge_color(ExtrasMenuApSecurity security)
{
    switch (security)
    {
        case EXTRAS_MENU_AP_SECURITY_WPA3:
        case EXTRAS_MENU_AP_SECURITY_WPA2_WPA3:
            return "#9b59b6"; /* purple */
        case EXTRAS_MENU_AP_SECURITY_WPA2:
            return "#27ae60"; /* green */
        case EXTRAS_MENU_AP_SECURITY_WPA:
        case EXTRAS_MENU_AP_SECURITY_WPA_WPA2:
            return "#e67e22"; /* orange */
        case EXTRAS_MENU_AP_SECURITY_ENTERPRISE:
            return "#2980b9"; /* blue */
        case EXTRAS_MENU_AP_SECURITY_OWE:
            return "#16a085"; /* teal */
        case EXTRAS_MENU_AP_SECURITY_WEP:
        case EXTRAS_MENU_AP_SECURITY_OPEN:
            return "#e74c3c"; /* red */
        case EXTRAS_MENU_AP_SECURITY_UNKNOWN:
        default:
            return "#7f8c8d"; /* grey */
    }
}

/* Converts an AP's frequency in MHz to the band + channel a user would
 * recognise ("2.4 GHz, channel 6"). Channel numbering differs between
 * bands, hence the separate arithmetic; returns NULL for frequencies
 * outside the ranges we know how to label rather than guessing. */
static gchar *
describe_frequency(guint32 frequency_mhz)
{
    if (frequency_mhz == 0)
        return NULL;

    if (frequency_mhz >= 2412 && frequency_mhz <= 2484)
    {
        gint channel = (frequency_mhz == 2484) ? 14
                                                : (gint) ((frequency_mhz - 2407) / 5);
        return g_strdup_printf("2.4 GHz, channel %d (%u MHz)", channel, frequency_mhz);
    }

    if (frequency_mhz >= 5160 && frequency_mhz <= 5885)
    {
        gint channel = (gint) ((frequency_mhz - 5000) / 5);
        return g_strdup_printf("5 GHz, channel %d (%u MHz)", channel, frequency_mhz);
    }

    if (frequency_mhz >= 5955 && frequency_mhz <= 7115)
    {
        gint channel = (gint) ((frequency_mhz - 5950) / 5);
        return g_strdup_printf("6 GHz, channel %d (%u MHz)", channel, frequency_mhz);
    }

    return g_strdup_printf("%u MHz", frequency_mhz);
}

/* Adds one "Label: value" line to the details dialog's grid. */
static void
add_detail_row(GtkWidget *grid, gint row, const gchar *name, const gchar *value_markup)
{
    GtkWidget *name_label = gtk_label_new(name);
    gtk_label_set_xalign(GTK_LABEL(name_label), 0.0);
    gtk_style_context_add_class(gtk_widget_get_style_context(name_label), "dim-label");
    gtk_grid_attach(GTK_GRID(grid), name_label, 0, row, 1, 1);

    GtkWidget *value_label = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(value_label), value_markup);
    gtk_label_set_xalign(GTK_LABEL(value_label), 0.0);
    gtk_label_set_selectable(GTK_LABEL(value_label), TRUE); /* so the BSSID etc. can be copied */
    gtk_grid_attach(GTK_GRID(grid), value_label, 1, row, 1, 1);
}

static void
show_network_details_dialog(ExtrasMenuPlugin *plugin, const ExtrasMenuNetworkApDetails *details)
{
    GtkWidget *dialog = gtk_dialog_new_with_buttons(
        details->ssid,
        GTK_WINDOW(plugin->popover),
        GTK_DIALOG_DESTROY_WITH_PARENT,
        "_Close", GTK_RESPONSE_CLOSE,
        NULL);

    gtk_window_set_resizable(GTK_WINDOW(dialog), FALSE);
    gtk_container_set_border_width(GTK_CONTAINER(dialog), 12);

    GtkWidget *content_area = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 8);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 16);
    gtk_container_set_border_width(GTK_CONTAINER(grid), 8);
    gtk_container_add(GTK_CONTAINER(content_area), grid);

    gint row = 0;

    gchar *ssid_markup = g_markup_printf_escaped("<b>%s</b>", details->ssid);
    add_detail_row(grid, row++, "Network", ssid_markup);
    g_free(ssid_markup);

    gchar *security_markup = g_markup_printf_escaped(
        "<span foreground=\"%s\"><b>%s</b></span>",
        security_badge_color(details->security),
        extras_menu_ap_security_to_string(details->security));
    add_detail_row(grid, row++, "Security", security_markup);
    g_free(security_markup);

    gchar *status_markup = g_markup_printf_escaped(
        "%s", details->is_active ? "Connected" : "Not connected");
    add_detail_row(grid, row++, "Status", status_markup);
    g_free(status_markup);

    gchar *strength_markup = g_markup_printf_escaped("%d%%", details->strength);
    add_detail_row(grid, row++, "Signal", strength_markup);
    g_free(strength_markup);

    gchar *frequency_text = describe_frequency(details->frequency);
    if (frequency_text != NULL)
    {
        gchar *frequency_markup = g_markup_printf_escaped("%s", frequency_text);
        add_detail_row(grid, row++, "Band", frequency_markup);
        g_free(frequency_markup);
        g_free(frequency_text);
    }

    if (details->max_bitrate > 0)
    {
        /* NM reports this in kb/s; Mb/s is what people expect to see. */
        gchar *bitrate_markup = g_markup_printf_escaped("%u Mb/s", details->max_bitrate / 1000);
        add_detail_row(grid, row++, "Max rate", bitrate_markup);
        g_free(bitrate_markup);
    }

    if (details->bssid != NULL && details->bssid[0] != '\0')
    {
        gchar *bssid_markup = g_markup_printf_escaped("<tt>%s</tt>", details->bssid);
        add_detail_row(grid, row++, "BSSID", bssid_markup);
        g_free(bssid_markup);
    }

    /* Everything shown above was copied into labels already, so
     * nothing here needs `details` to outlive this call. */
    extras_menu_dropdown_track_dialog(plugin, dialog);
    g_signal_connect(dialog, "response", G_CALLBACK(extras_menu_dropdown_dialog_response_destroy), NULL);
    gtk_widget_show_all(dialog);
}

/* Fired once extras_menu_network_forget() finishes. Only surfaces
 * failures -- a successful forget needs no confirmation dialog, since
 * the list refresh that follows is visible feedback in itself. */
static void
on_forget_result(gboolean success, const gchar *error_message, gpointer user_data)
{
    ExtrasMenuPlugin *plugin = EXTRAS_MENU_PLUGIN(user_data);

    if (success)
        return;

    extras_menu_dropdown_show_message(plugin, GTK_MESSAGE_ERROR,
                        "Couldn't forget the network.",
                        error_message != NULL ? error_message : "Unknown error");
}

static void
on_forget_menu_item_activated(GtkMenuItem *item, gpointer user_data)
{
    ExtrasMenuPlugin *plugin = EXTRAS_MENU_PLUGIN(user_data);
    const ExtrasMenuNetworkApDetails *details = g_object_get_data(G_OBJECT(item), "extras-menu-ap-details");

    if (details == NULL)
        return;

    extras_menu_network_forget(plugin->network, details->ssid, on_forget_result, plugin);
}

static void
on_details_menu_item_activated(GtkMenuItem *item, gpointer user_data)
{
    ExtrasMenuPlugin *plugin = EXTRAS_MENU_PLUGIN(user_data);
    const ExtrasMenuNetworkApDetails *details = g_object_get_data(G_OBJECT(item), "extras-menu-ap-details");

    if (details == NULL)
        return;

    show_network_details_dialog(plugin, details);
}

/* Right-click on a network row opens a small context menu: forget the
 * network (delete its saved profile/password) or show its details.
 * Left-clicks are left alone so the row's normal activate-to-connect
 * behaviour still works.
 *
 * Connected to the GtkListBox rather than to each row: rows don't
 * receive button events of their own (the list box handles input for
 * the whole list), so a per-row handler never fires. The clicked row
 * is found from the event's y coordinate instead. */
gboolean
extras_menu_network_dialogs_on_list_button_press(GtkWidget *list_box, GdkEventButton *event, gpointer user_data)
{
    ExtrasMenuPlugin *plugin = EXTRAS_MENU_PLUGIN(user_data);

    if (event->type != GDK_BUTTON_PRESS || event->button != GDK_BUTTON_SECONDARY)
        return GDK_EVENT_PROPAGATE;

    GtkListBoxRow *row = gtk_list_box_get_row_at_y(GTK_LIST_BOX(list_box), (gint) event->y);
    if (row == NULL)
        return GDK_EVENT_PROPAGATE;

    ExtrasMenuNetworkApDetails *details = g_object_get_data(G_OBJECT(row), "extras-menu-ap-details");
    if (details == NULL)
        return GDK_EVENT_PROPAGATE;

    GtkWidget *menu = gtk_menu_new();

    GtkWidget *forget_item = gtk_menu_item_new_with_label("Forget this network");
    g_object_set_data_full(G_OBJECT(forget_item), "extras-menu-ap-details",
                            ap_details_copy(details), extras_menu_network_ap_details_free);
    g_signal_connect(forget_item, "activate",
                      G_CALLBACK(on_forget_menu_item_activated), plugin);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), forget_item);

    GtkWidget *details_item = gtk_menu_item_new_with_label("Network details\xE2\x80\xA6"); /* "Network details…" */
    g_object_set_data_full(G_OBJECT(details_item), "extras-menu-ap-details",
                            ap_details_copy(details), extras_menu_network_ap_details_free);
    g_signal_connect(details_item, "activate",
                      G_CALLBACK(on_details_menu_item_activated), plugin);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), details_item);

    gtk_widget_show_all(menu);

    /* A menu popped up this way is floating and not owned by anything,
     * so it would leak once dismissed -- tie its lifetime to being
     * dismissed instead. */
    g_signal_connect(menu, "selection-done", G_CALLBACK(gtk_widget_destroy), NULL);
    g_signal_connect(menu, "selection-done", G_CALLBACK(extras_menu_dropdown_context_menu_done), plugin);

    gtk_menu_popup_at_pointer(GTK_MENU(menu), (GdkEvent *) event);

    return GDK_EVENT_STOP;
}