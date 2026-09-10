#include "extras-menu.h"
#include "popover.h"
#include "preferences.h"
#include "network.h"

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

    apply_network_status(plugin, kind);
}

/* Fired when the "Wi-Fi"/"Ethernet" pill is clicked. Behavior depends
 * on the current connection kind: in Wi-Fi mode, toggles the network
 * list revealer open/closed; in Ethernet mode (or no connection),
 * there's no list to show, so a small info dialog with the IP address
 * is shown instead (Ethernet has nothing to "choose" the way Wi-Fi
 * networks do). */
static void
on_network_pill_clicked(GtkButton *button, ExtrasMenuPlugin *plugin)
{
    (void) button;

    if (plugin->network_last_kind == EXTRAS_MENU_NETWORK_KIND_WIFI)
    {
        if (plugin->network_revealer == NULL)
            return;

        gboolean currently_open = gtk_revealer_get_reveal_child(GTK_REVEALER(plugin->network_revealer));
        gtk_revealer_set_reveal_child(GTK_REVEALER(plugin->network_revealer), !currently_open);

        /* Ask for a fresh scan each time the list is opened, so it's
         * not showing stale results from whenever the dropdown last
         * happened to scan. */
        if (!currently_open)
            extras_menu_network_rescan(plugin->network);

        return;
    }

    /* Ethernet or no connection: show a simple info dialog instead of
     * a list -- nothing to pick between on a wired connection. */
    GtkWidget *dialog = gtk_message_dialog_new(
        GTK_WINDOW(plugin->popover), GTK_DIALOG_DESTROY_WITH_PARENT,
        GTK_MESSAGE_INFO, GTK_BUTTONS_OK,
        plugin->network_last_kind == EXTRAS_MENU_NETWORK_KIND_ETHERNET
            ? "Connected via Ethernet"
            : "Not connected");

    if (plugin->network_last_kind == EXTRAS_MENU_NETWORK_KIND_ETHERNET)
    {
        gtk_message_dialog_format_secondary_text(
            GTK_MESSAGE_DIALOG(dialog), "IP address: %s",
            plugin->network_last_ip_address != NULL ? plugin->network_last_ip_address
                                                      : "Not yet assigned");
    }

    gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);
}

/* Fired once extras_menu_network_connect() (below) finishes, whether
 * it succeeded or not. On failure, shows the error in a simple message
 * dialog -- good enough for now; a more polished inline error in the
 * list row is a possible future improvement. */
static void
on_connect_result(gboolean success, const gchar *error_message, gpointer user_data)
{
    GtkWindow *parent = GTK_WINDOW(user_data);

    if (success)
        return;

    GtkWidget *dialog = gtk_message_dialog_new(
        parent, GTK_DIALOG_DESTROY_WITH_PARENT,
        GTK_MESSAGE_ERROR, GTK_BUTTONS_OK,
        "Couldn't connect to the network.");
    gtk_message_dialog_format_secondary_text(
        GTK_MESSAGE_DIALOG(dialog), "%s",
        error_message != NULL ? error_message : "Unknown error");
    gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);
}

/* Prompts for a Wi-Fi password with a small modal dialog, then
 * attempts to connect. parent_window anchors both the password dialog
 * and any resulting error dialog. */
static void
prompt_password_and_connect(ExtrasMenuPlugin *plugin, const gchar *ssid, GtkWindow *parent_window)
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

    gtk_widget_show_all(dialog);

    gint response = gtk_dialog_run(GTK_DIALOG(dialog));
    if (response == GTK_RESPONSE_OK)
    {
        const gchar *password = gtk_entry_get_text(GTK_ENTRY(entry));
        extras_menu_network_connect(plugin->network, ssid, password,
                                     on_connect_result, parent_window);
    }

    gtk_widget_destroy(dialog);
}

/* Fired when the user clicks a network row in the Wi-Fi list. Open
 * networks connect immediately; secured ones prompt for a password
 * first. Already-active networks are not re-clicked in practice since
 * they're visually marked instead of being made clickable-looking, but
 * clicking one anyway would simply reconnect harmlessly. */
static void
on_network_row_activated(GtkListBox *list_box, GtkListBoxRow *row, gpointer user_data)
{
    ExtrasMenuPlugin *plugin = EXTRAS_MENU_PLUGIN(user_data);
    (void) list_box;

    const gchar *ssid = g_object_get_data(G_OBJECT(row), "extras-menu-ssid");
    gboolean secured = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(row), "extras-menu-secured"));

    if (ssid == NULL)
        return;

    GtkWindow *parent_window = GTK_WINDOW(plugin->popover);

    if (secured)
        prompt_password_and_connect(plugin, ssid, parent_window);
    else
        extras_menu_network_connect(plugin->network, ssid, NULL, on_connect_result, parent_window);
}

/* Builds one GtkListBoxRow for an access point: signal-strength icon,
 * SSID, a lock icon if secured, and a checkmark if this is the network
 * we're currently connected to. The SSID and secured flag are stashed
 * as object data so on_network_row_activated() (above) can read them
 * back without needing a parallel data structure to look rows up in. */
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

    if (ap->is_active)
    {
        GtkWidget *check_icon = gtk_image_new_from_icon_name("object-select-symbolic",
                                                               GTK_ICON_SIZE_BUTTON);
        gtk_box_pack_start(GTK_BOX(row_box), check_icon, FALSE, FALSE, 0);
    }

    GtkWidget *row = gtk_list_box_row_new();
    gtk_container_add(GTK_CONTAINER(row), row_box);

    g_object_set_data_full(G_OBJECT(row), "extras-menu-ssid", g_strdup(ap->ssid), g_free);
    g_object_set_data(G_OBJECT(row), "extras-menu-secured", GINT_TO_POINTER(ap->secured));

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

    if (!wifi_available)
        return;

    for (guint i = 0; i < count; i++)
    {
        GtkWidget *row = make_network_row(&aps[i]);
        gtk_list_box_insert(GTK_LIST_BOX(plugin->network_list_box), row, -1);
    }
}

static void extras_menu_plugin_construct(XfcePanelPlugin *panel_plugin);

/* Registers the plugin with the panel. This macro expands to define
 * extras_menu_plugin_get_type(), extras_menu_plugin_register_type(),
 * and the module entry point xfce_panel_module_init(); it expects us
 * to provide extras_menu_plugin_init() and extras_menu_plugin_class_init()
 * ourselves below (their prototypes are generated by this macro, not
 * their bodies). Do NOT also call G_DEFINE_TYPE here -- that generates
 * the same symbols a second time and fails to compile. */
XFCE_PANEL_DEFINE_PLUGIN(ExtrasMenuPlugin, extras_menu_plugin)

static void
extras_menu_plugin_class_init(ExtrasMenuPluginClass *klass)
{
    XfcePanelPluginClass *plugin_class = XFCE_PANEL_PLUGIN_CLASS(klass);
    plugin_class->construct = extras_menu_plugin_construct;
}

static void
extras_menu_plugin_init(ExtrasMenuPlugin *plugin)
{
    /* Widgets are built in construct(), once the plugin has a panel to
     * attach to; nothing to do here yet. */
    plugin->toggle_button = NULL;
    plugin->button_icon = NULL;
    plugin->popover = NULL;
    plugin->volume_scale = NULL;
    plugin->volume_icon = NULL;
    plugin->audio = NULL;
    plugin->updating_volume_from_backend = FALSE;
    plugin->brightness_scale = NULL;
    plugin->brightness = NULL;
    plugin->updating_brightness_from_backend = FALSE;
    plugin->bluetooth_toggle = NULL;
    plugin->bluetooth = NULL;
    plugin->updating_bluetooth_from_backend = FALSE;
    plugin->bluetooth_last_available = FALSE;
    plugin->bluetooth_last_powered = FALSE;
    plugin->bluetooth_has_state = FALSE;
    plugin->network_pill_button = NULL;
    plugin->network_pill_label = NULL;
    plugin->network_pill_icon = NULL;
    plugin->network_revealer = NULL;
    plugin->network_list_box = NULL;
    plugin->network = NULL;
    plugin->network_last_kind = EXTRAS_MENU_NETWORK_KIND_NONE;
    plugin->network_last_ip_address = NULL;
    plugin->network_has_status = FALSE;
}

/* Moves the dropdown window so it sits next to the panel button,
 * horizontally centered on it, and on whichever side there's actually
 * room: below the button for a top (or left/right) panel, above the
 * button for a bottom panel. Called every time we're about to show
 * the window, since the button's on-screen position -- and the
 * panel's screen position -- can change (panel moved, screen
 * resolution changed, ...). */
static void
position_dropdown_below_button(ExtrasMenuPlugin *plugin)
{
    GdkWindow *button_window = gtk_widget_get_window(plugin->toggle_button);
    if (button_window == NULL)
        return;

    gint origin_x = 0, origin_y = 0;
    gdk_window_get_origin(button_window, &origin_x, &origin_y);

    GtkAllocation button_alloc;
    gtk_widget_get_allocation(plugin->toggle_button, &button_alloc);

    /* Ask the dropdown for its natural size so we can center it under
     * the button rather than align it to one edge -- this way the
     * dropdown stays centered on the trigger icon regardless of where
     * along the panel the plugin is placed. */
    GtkRequisition natural_size;
    gtk_widget_get_preferred_size(plugin->popover, NULL, &natural_size);

    gint button_center_x = origin_x + button_alloc.x + (button_alloc.width / 2);
    gint x = button_center_x - (natural_size.width / 2);

    /* If the panel sits at the bottom of the screen, there's no room
     * to open downward -- flip and open the dropdown upward instead,
     * so its bottom edge sits just above the button. For a top (or
     * any other) panel position, keep opening downward as before. */
    XfceScreenPosition screen_position =
        xfce_panel_plugin_get_screen_position(XFCE_PANEL_PLUGIN(plugin));

    gint y;
    if (xfce_screen_position_is_bottom(screen_position))
        y = origin_y + button_alloc.y - natural_size.height;
    else
        y = origin_y + button_alloc.y + button_alloc.height;

    /* Keep the dropdown fully on-screen horizontally: centering can
     * push it past either edge for buttons near the left or right end
     * of the panel, so clamp against the monitor's actual bounds
     * rather than just the left edge. */
    GdkDisplay *display = gdk_window_get_display(button_window);
    GdkMonitor *monitor = gdk_display_get_monitor_at_window(display, button_window);
    GdkRectangle monitor_geom;
    gdk_monitor_get_geometry(monitor, &monitor_geom);

    if (x < monitor_geom.x)
        x = monitor_geom.x;
    else if (x + natural_size.width > monitor_geom.x + monitor_geom.width)
        x = monitor_geom.x + monitor_geom.width - natural_size.width;

    gtk_window_move(GTK_WINDOW(plugin->popover), x, y);
}

/* Toggle the dropdown open/closed when the panel button is clicked.
 * Implemented as a plain GtkWindow (see extras-menu.h for why) rather
 * than a GtkPopover, so opening it means: position it under the
 * button, then show it and force it above other windows including the
 * always-on-top panel. */
static void
on_toggle_button_clicked(GtkToggleButton *button, ExtrasMenuPlugin *plugin)
{
    if (gtk_toggle_button_get_active(button))
    {
        position_dropdown_below_button(plugin);
        gtk_window_present(GTK_WINDOW(plugin->popover));
        gtk_window_set_keep_above(GTK_WINDOW(plugin->popover), TRUE);
        gtk_image_set_from_icon_name(GTK_IMAGE(plugin->button_icon),
                                      "pan-up-symbolic", GTK_ICON_SIZE_BUTTON);
    }
    else
    {
        gtk_widget_hide(plugin->popover);
        gtk_image_set_from_icon_name(GTK_IMAGE(plugin->button_icon),
                                      "pan-down-symbolic", GTK_ICON_SIZE_BUTTON);
    }
}

/* Since we're using a plain GtkWindow instead of a GtkPopover, we lose
 * the automatic "close when focus leaves" behaviour popovers get for
 * free. This reimplements it: whenever the dropdown window loses
 * keyboard focus (the user clicked elsewhere, or switched windows),
 * hide it and reset the toggle button to match. */
static gboolean
on_dropdown_focus_out(GtkWidget *widget, GdkEventFocus *event, ExtrasMenuPlugin *plugin)
{
    (void) widget;
    (void) event;

    gtk_widget_hide(plugin->popover);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(plugin->toggle_button), FALSE);
    gtk_image_set_from_icon_name(GTK_IMAGE(plugin->button_icon),
                                  "pan-down-symbolic", GTK_ICON_SIZE_BUTTON);

    return FALSE;
}

/* Called by the audio backend on connect and on every subsequent
 * volume/mute change, whether we caused it or something else did
 * (pavucontrol, media keys, ...). Updates the slider to match. */
static void
on_audio_changed(guint volume_percent, gboolean muted, gpointer user_data)
{
    ExtrasMenuPlugin *plugin = EXTRAS_MENU_PLUGIN(user_data);

    if (plugin->volume_scale == NULL)
        return;

    /* Setting the GtkRange's value below fires "value-changed" on the
     * slider; this flag tells our own handler (further down) that the
     * change originated from the backend, not from the user dragging
     * the slider, so it must not be echoed straight back to
     * PulseAudio -- that would just be a harmless but wasteful loop,
     * and could fight with the user's own drag in edge cases. */
    plugin->updating_volume_from_backend = TRUE;
    gtk_range_set_value(GTK_RANGE(plugin->volume_scale), (gdouble) volume_percent);
    plugin->updating_volume_from_backend = FALSE;

    /* Swap the speaker icon to its muted variant whenever the sink is
     * either explicitly muted or simply at 0% -- both look the same
     * to the user, so both get the crossed-out icon. Otherwise scale
     * the icon by level, matching what most desktop volume sliders do
     * (low/medium/high), purely as a visual nicety. */
    if (plugin->volume_icon != NULL)
    {
        const gchar *icon_name;

        if (muted || volume_percent == 0)
            icon_name = "audio-volume-muted-symbolic";
        else if (volume_percent < 34)
            icon_name = "audio-volume-low-symbolic";
        else if (volume_percent < 67)
            icon_name = "audio-volume-medium-symbolic";
        else
            icon_name = "audio-volume-high-symbolic";

        gtk_image_set_from_icon_name(GTK_IMAGE(plugin->volume_icon),
                                      icon_name, GTK_ICON_SIZE_BUTTON);
    }
}

/* Called when the user drags the slider (or otherwise changes its
 * value through the UI). Pushes the new value out to PulseAudio,
 * unless the change actually came from on_audio_changed() above. */
static void
on_volume_scale_changed(GtkRange *range, ExtrasMenuPlugin *plugin)
{
    if (plugin->updating_volume_from_backend)
        return;

    guint percent = (guint) gtk_range_get_value(range);
    extras_menu_audio_set_volume(plugin->audio, percent);
}

/* Same idea as on_audio_changed(), but for screen brightness via
 * brightnessctl. See brightness.h -- there's no live external-change
 * notification here (brightnessctl has no subscribe mechanism), so
 * this only fires on initial read and after our own set() calls. */
static void
on_brightness_changed(guint percent, gpointer user_data)
{
    ExtrasMenuPlugin *plugin = EXTRAS_MENU_PLUGIN(user_data);

    if (plugin->brightness_scale == NULL)
        return;

    plugin->updating_brightness_from_backend = TRUE;
    gtk_range_set_value(GTK_RANGE(plugin->brightness_scale), (gdouble) percent);
    plugin->updating_brightness_from_backend = FALSE;
}

/* Mirrors on_volume_scale_changed(), for the brightness slider. */
static void
on_brightness_scale_changed(GtkRange *range, ExtrasMenuPlugin *plugin)
{
    if (plugin->updating_brightness_from_backend)
        return;

    guint percent = (guint) gtk_range_get_value(range);
    extras_menu_brightness_set(plugin->brightness, percent);
}

/* Applies a Bluetooth state (available/powered) to the toggle widget.
 * Shared by on_bluetooth_changed() (the normal path) and the
 * end of construct() (to catch the case where the backend's first
 * callback arrived before the widget existed -- see
 * bluetooth_has_state in extras-menu.h for why that can happen). */
static void
apply_bluetooth_state(ExtrasMenuPlugin *plugin, gboolean available, gboolean powered)
{
    if (plugin->bluetooth_toggle == NULL)
        return;

    gtk_widget_set_sensitive(plugin->bluetooth_toggle, available);

    /* See on_audio_changed() for why this flag exists -- same
     * feedback-loop concern, this time against BlueZ. Explicitly force
     * the checked state to FALSE when unavailable (rather than leaving
     * whatever the button's initial/previous state was) -- a toggle
     * showing "on" while insensitive would be actively misleading. */
    plugin->updating_bluetooth_from_backend = TRUE;
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(plugin->bluetooth_toggle),
                                  available ? powered : FALSE);
    plugin->updating_bluetooth_from_backend = FALSE;
}

/* Called by the Bluetooth backend once we know the adapter's Powered
 * state, and again on every subsequent change (from us or elsewhere --
 * bluetoothctl, rfkill, a hardware switch). If no adapter is available
 * at all, the toggle is disabled outright rather than left in a
 * misleading on/off state.
 *
 * Always caches the latest state on the plugin first: GDBus has been
 * observed to invoke this callback before bluetooth_toggle has been
 * assigned yet in some cases (i.e. before extras_menu_bluetooth_new()
 * has returned to construct()), in which case apply_bluetooth_state()
 * below is a no-op and the state would otherwise be silently lost --
 * construct() re-applies the cached state once the widget exists. */
static void
on_bluetooth_changed(gboolean available, gboolean powered, gpointer user_data)
{
    ExtrasMenuPlugin *plugin = EXTRAS_MENU_PLUGIN(user_data);

    plugin->bluetooth_last_available = available;
    plugin->bluetooth_last_powered = powered;
    plugin->bluetooth_has_state = TRUE;

    apply_bluetooth_state(plugin, available, powered);
}

/* Called when the user clicks the Bluetooth pill. Pushes the new
 * power state out to BlueZ, unless the change actually came from
 * on_bluetooth_changed() above. */
static void
on_bluetooth_toggle_clicked(GtkToggleButton *button, ExtrasMenuPlugin *plugin)
{
    if (plugin->updating_bluetooth_from_backend)
        return;

    extras_menu_bluetooth_set_powered(plugin->bluetooth,
                                       gtk_toggle_button_get_active(button));
}

/* Fired when the user picks "Properties..." from the plugin's
 * right-click panel menu (enabled via
 * xfce_panel_plugin_menu_show_configure() in construct() below). */
static void
on_plugin_configure(XfcePanelPlugin *panel_plugin, ExtrasMenuPlugin *plugin)
{
    (void) plugin;
    extras_menu_preferences_show(panel_plugin);
}

static void
on_plugin_free_data(XfcePanelPlugin *panel_plugin, ExtrasMenuPlugin *plugin)
{
    (void) panel_plugin;

    if (plugin->audio != NULL)
    {
        extras_menu_audio_free(plugin->audio);
        plugin->audio = NULL;
    }

    if (plugin->brightness != NULL)
    {
        extras_menu_brightness_free(plugin->brightness);
        plugin->brightness = NULL;
    }

    if (plugin->bluetooth != NULL)
    {
        extras_menu_bluetooth_free(plugin->bluetooth);
        plugin->bluetooth = NULL;
    }

    if (plugin->network != NULL)
    {
        extras_menu_network_free(plugin->network);
        plugin->network = NULL;
    }

    g_free(plugin->network_last_ip_address);
    plugin->network_last_ip_address = NULL;
}

static void
extras_menu_plugin_construct(XfcePanelPlugin *panel_plugin)
{
    ExtrasMenuPlugin *plugin = EXTRAS_MENU_PLUGIN(panel_plugin);

    /* --- panel button (the "^" chevron trigger) --- */
    plugin->toggle_button = gtk_toggle_button_new();
    gtk_button_set_relief(GTK_BUTTON(plugin->toggle_button), GTK_RELIEF_NONE);

    plugin->button_icon = gtk_image_new_from_icon_name("pan-down-symbolic",
                                                         GTK_ICON_SIZE_BUTTON);
    gtk_container_add(GTK_CONTAINER(plugin->toggle_button), plugin->button_icon);

    gtk_container_add(GTK_CONTAINER(panel_plugin), plugin->toggle_button);
    xfce_panel_plugin_add_action_widget(panel_plugin, plugin->toggle_button);

    /* --- dropdown window, built once and reused on every open ---
     * GTK_WINDOW_POPUP gives us an undecorated, non-taskbar window
     * (no titlebar, no border) similar in spirit to a popover, but as
     * a real top-level window we fully control the position and
     * stacking of -- see extras-menu.h for why we moved away from
     * GtkPopover. */
    plugin->popover = gtk_window_new(GTK_WINDOW_POPUP);
    gtk_window_set_type_hint(GTK_WINDOW(plugin->popover), GDK_WINDOW_TYPE_HINT_POPUP_MENU);
    gtk_window_set_decorated(GTK_WINDOW(plugin->popover), FALSE);
    gtk_window_set_skip_taskbar_hint(GTK_WINDOW(plugin->popover), TRUE);
    gtk_window_set_skip_pager_hint(GTK_WINDOW(plugin->popover), TRUE);
    gtk_window_set_resizable(GTK_WINDOW(plugin->popover), FALSE);

    GtkWidget *content = extras_menu_popover_content_new(&plugin->volume_scale,
                                                           &plugin->volume_icon,
                                                           &plugin->brightness_scale,
                                                           &plugin->bluetooth_toggle,
                                                           &plugin->network_pill_button,
                                                           &plugin->network_pill_label,
                                                           &plugin->network_pill_icon,
                                                           &plugin->network_revealer,
                                                           &plugin->network_list_box);
    gtk_container_add(GTK_CONTAINER(plugin->popover), content);

    /* Frame + drop shadow so the window doesn't look like a bare
     * rectangle floating over the desktop -- GtkPopover normally gives
     * us this via its own CSS node, which we no longer have. */
    {
        GtkStyleContext *ctx = gtk_widget_get_style_context(plugin->popover);
        gtk_style_context_add_class(ctx, "background");

        GtkCssProvider *frame_provider = gtk_css_provider_new();
        gtk_css_provider_load_from_data(
            frame_provider,
            "window { "
            "  border-radius: 12px; "
            "  box-shadow: 0 4px 16px rgba(0, 0, 0, 0.35); "
            "}",
            -1, NULL);
        gtk_style_context_add_provider(ctx, GTK_STYLE_PROVIDER(frame_provider),
                                        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
        g_object_unref(frame_provider);
    }

    g_signal_connect(plugin->toggle_button, "clicked",
                      G_CALLBACK(on_toggle_button_clicked), plugin);
    g_signal_connect(plugin->popover, "focus-out-event",
                      G_CALLBACK(on_dropdown_focus_out), plugin);

    /* --- audio backend: connects asynchronously, reports state (and
     * every subsequent change) through on_audio_changed --- */
    plugin->audio = extras_menu_audio_new(on_audio_changed, plugin);

    if (plugin->volume_scale != NULL)
    {
        g_signal_connect(plugin->volume_scale, "value-changed",
                          G_CALLBACK(on_volume_scale_changed), plugin);
    }

    /* --- brightness backend: reads the current value async on start,
     * reports it (and every value we set afterwards) through
     * on_brightness_changed --- */
    plugin->brightness = extras_menu_brightness_new(on_brightness_changed, plugin);

    if (plugin->brightness_scale != NULL)
    {
        g_signal_connect(plugin->brightness_scale, "value-changed",
                          G_CALLBACK(on_brightness_scale_changed), plugin);
    }

    /* --- Bluetooth backend: finds the first adapter async, reports
     * its Powered state (and every subsequent change) through
     * on_bluetooth_changed. Device list / pairing is a follow-up --
     * for now this only drives the adapter on/off toggle. --- */
    plugin->bluetooth = extras_menu_bluetooth_new(on_bluetooth_changed, plugin);

    if (plugin->bluetooth_toggle != NULL)
    {
        /* Disabled until the backend confirms an adapter actually
         * exists (see on_bluetooth_changed) -- avoids a toggle that
         * looks clickable but silently does nothing on machines
         * without Bluetooth hardware. */
        gtk_widget_set_sensitive(plugin->bluetooth_toggle, FALSE);
        g_signal_connect(plugin->bluetooth_toggle, "toggled",
                          G_CALLBACK(on_bluetooth_toggle_clicked), plugin);

        /* Catch up on any state the backend already reported before
         * this widget existed -- see bluetooth_has_state in
         * extras-menu.h for why that can happen. */
        if (plugin->bluetooth_has_state)
        {
            apply_bluetooth_state(plugin, plugin->bluetooth_last_available,
                                   plugin->bluetooth_last_powered);
        }
    }

    /* --- network backend: finds the Wi-Fi/Ethernet devices async,
     * reports the active connection kind (and Ethernet IP) through
     * on_network_status_changed, and the visible Wi-Fi network list
     * through on_network_list_changed. The pill's label/icon track the
     * connection kind, clicking it reveals the Wi-Fi list (or shows an
     * Ethernet info dialog), and clicking a network row connects to
     * it. --- */
    plugin->network = extras_menu_network_new(on_network_list_changed,
                                               on_network_status_changed, plugin);

    /* Catch up on any status the backend already reported before
     * these widgets existed -- see network_has_status in
     * extras-menu.h for why that can happen. */
    if (plugin->network_has_status)
        apply_network_status(plugin, plugin->network_last_kind);

    if (plugin->network_pill_button != NULL)
    {
        g_signal_connect(plugin->network_pill_button, "clicked",
                          G_CALLBACK(on_network_pill_clicked), plugin);
    }

    if (plugin->network_list_box != NULL)
    {
        g_signal_connect(plugin->network_list_box, "row-activated",
                          G_CALLBACK(on_network_row_activated), plugin);
    }

    g_signal_connect(panel_plugin, "free-data",
                      G_CALLBACK(on_plugin_free_data), plugin);

    /* --- "Properties..." entry in the plugin's right-click panel menu,
     * opening the (currently minimal) preferences dialog --- */
    xfce_panel_plugin_menu_show_configure(panel_plugin);
    g_signal_connect(panel_plugin, "configure-plugin",
                      G_CALLBACK(on_plugin_configure), plugin);

    gtk_widget_show_all(plugin->toggle_button);
}