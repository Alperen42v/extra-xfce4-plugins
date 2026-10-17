/*
 * The Bluetooth pill and its device list: reacts to the Bluetooth backend,
 * keeps the toggle and the list in step with it, and handles clicks
 * (adapter toggle, chevron, connect/disconnect/forget). See
 * bluetooth-ui.h.
 */

#include "bluetooth/bluetooth-ui.h"
#include "network/network-ui.h"
#include "dropdown.h"

/* Opens/closes the Bluetooth device list, flips its chevron, and turns
 * scanning for nearby devices on/off to match -- scanning costs battery
 * and is only useful while someone is looking at the list, so it must
 * never be left running when the list isn't showing. */
void
extras_menu_bluetooth_ui_set_list_open(ExtrasMenuPlugin *plugin, gboolean open)
{
    if (plugin->bluetooth_revealer != NULL)
        gtk_revealer_set_reveal_child(GTK_REVEALER(plugin->bluetooth_revealer), open);

    if (plugin->bluetooth_expand_chevron != NULL)
    {
        gtk_image_set_from_icon_name(GTK_IMAGE(plugin->bluetooth_expand_chevron),
                                      open ? "pan-down-symbolic" : "pan-end-symbolic",
                                      GTK_ICON_SIZE_BUTTON);
    }

    if (plugin->bluetooth != NULL)
        extras_menu_bluetooth_set_discovering(plugin->bluetooth, open);
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

    /* The chevron is useless without an adapter, and a list left open
     * when the adapter disappears would have nothing to show. */
    if (plugin->bluetooth_expand_button != NULL)
        gtk_widget_set_sensitive(plugin->bluetooth_expand_button, available);
    if (!available)
        extras_menu_bluetooth_ui_set_list_open(plugin, FALSE);

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

    extras_menu_airplane_mode_bluetooth_changed(plugin->airplane_mode, available, powered);

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

/* --- Bluetooth device list ------------------------------------------------------ */

/* Sets the status label of the device row for `path` ("Connecting...",
 * "Pairing...", ...). NULL text puts back the row's resting status
 * ("Connected", or nothing), not an empty label -- a connected device
 * whose disconnect attempt failed must keep saying "Connected". */
static void
set_bluetooth_row_status(ExtrasMenuPlugin *plugin, const gchar *path, const gchar *text)
{
    if (plugin->bluetooth_row_by_path == NULL || path == NULL)
        return;

    GtkWidget *row = g_hash_table_lookup(plugin->bluetooth_row_by_path, path);
    if (row == NULL)
        return;

    GtkWidget *label = g_object_get_data(G_OBJECT(row), "extras-menu-status-label");
    const gchar *resting = g_object_get_data(G_OBJECT(row), "extras-menu-base-status");
    if (label == NULL)
        return;

    const gchar *shown = text != NULL ? text : (resting != NULL ? resting : "");
    gtk_label_set_text(GTK_LABEL(label), shown);
    gtk_widget_set_visible(label, shown[0] != '\0');
}

/* BlueZ describes a device's kind with an icon-name hint ("audio-headset",
 * "input-mouse", ...); the symbolic variant of it is used when the theme
 * has one, with the generic Bluetooth icon as the fallback. */
static gchar *
bluetooth_device_icon_name(const gchar *hint)
{
    if (hint != NULL && hint[0] != '\0')
    {
        gchar *name = g_strconcat(hint, "-symbolic", NULL);
        if (gtk_icon_theme_has_icon(gtk_icon_theme_get_default(), name))
            return name;
        g_free(name);
    }

    return g_strdup("bluetooth-symbolic");
}

/* One row of plain text in the list ("Bluetooth is turned off", ...);
 * it can't be clicked. */
static GtkWidget *
make_bluetooth_placeholder_row(const gchar *text)
{
    GtkWidget *label = gtk_label_new(text);
    gtk_label_set_xalign(GTK_LABEL(label), 0.0);
    gtk_label_set_line_wrap(GTK_LABEL(label), TRUE);
    gtk_widget_set_margin_start(label, 6);
    gtk_widget_set_margin_end(label, 6);
    gtk_widget_set_margin_top(label, 8);
    gtk_widget_set_margin_bottom(label, 8);
    gtk_style_context_add_class(gtk_widget_get_style_context(label), "dim-label");

    GtkWidget *row = gtk_list_box_row_new();
    gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row), FALSE);
    gtk_list_box_row_set_selectable(GTK_LIST_BOX_ROW(row), FALSE);
    gtk_container_add(GTK_CONTAINER(row), label);
    gtk_widget_show_all(row);
    return row;
}

/* Builds one row for a device: type icon, name, and a status label
 * ("Connected", or the running operation's "Connecting..."). What
 * on_bluetooth_row_activated() and the right-click menu need is stashed
 * as object data on the row, the same way the Wi-Fi rows do it. */
static GtkWidget *
make_bluetooth_row(ExtrasMenuPlugin *plugin, const ExtrasMenuBluetoothDevice *device)
{
    GtkWidget *row_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(row_box), 6);

    gchar *icon_name = bluetooth_device_icon_name(device->icon);
    gtk_box_pack_start(GTK_BOX(row_box),
                       gtk_image_new_from_icon_name(icon_name, GTK_ICON_SIZE_BUTTON),
                       FALSE, FALSE, 0);
    g_free(icon_name);

    GtkWidget *label = gtk_label_new(device->name);
    gtk_label_set_xalign(GTK_LABEL(label), 0.0);
    gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
    gtk_widget_set_hexpand(label, TRUE);
    gtk_box_pack_start(GTK_BOX(row_box), label, TRUE, TRUE, 0);

    const gchar *resting = device->connected ? "Connected" : "";
    gboolean busy_here = plugin->bluetooth_busy_path != NULL &&
                         g_strcmp0(plugin->bluetooth_busy_path, device->path) == 0;
    const gchar *shown = (busy_here && plugin->bluetooth_busy_text != NULL)
                             ? plugin->bluetooth_busy_text : resting;

    GtkWidget *status_label = gtk_label_new(shown);
    gtk_style_context_add_class(gtk_widget_get_style_context(status_label), "dim-label");
    gtk_widget_set_no_show_all(status_label, TRUE); /* so show_all() below doesn't force it visible */
    gtk_widget_set_visible(status_label, shown[0] != '\0');
    gtk_box_pack_start(GTK_BOX(row_box), status_label, FALSE, FALSE, 0);

    GtkWidget *row = gtk_list_box_row_new();
    gtk_container_add(GTK_CONTAINER(row), row_box);

    g_object_set_data_full(G_OBJECT(row), "extras-menu-bt-path", g_strdup(device->path), g_free);
    g_object_set_data(G_OBJECT(row), "extras-menu-bt-paired", GINT_TO_POINTER(device->paired));
    g_object_set_data(G_OBJECT(row), "extras-menu-bt-connected", GINT_TO_POINTER(device->connected));
    g_object_set_data(G_OBJECT(row), "extras-menu-status-label", status_label);
    g_object_set_data_full(G_OBJECT(row), "extras-menu-base-status", g_strdup(resting), g_free);

    gtk_widget_show_all(row);
    return row;
}

/* Called by the Bluetooth backend whenever the device list changes.
 * Clears and rebuilds the list from scratch each time, like the Wi-Fi
 * list does -- simpler than diffing, and the backend already folds
 * bursts of changes into one call.
 *
 * Which state the list is in comes from the cached adapter state (see
 * on_bluetooth_changed()), not from the backend directly: the list says
 * so when Bluetooth is off, and that is not the same as being empty. */
static void
on_bluetooth_devices_changed(const ExtrasMenuBluetoothDevice *devices, guint count,
                              gpointer user_data)
{
    ExtrasMenuPlugin *plugin = EXTRAS_MENU_PLUGIN(user_data);

    if (plugin->bluetooth_list_box == NULL)
        return;

    GList *existing_rows = gtk_container_get_children(GTK_CONTAINER(plugin->bluetooth_list_box));
    for (GList *l = existing_rows; l != NULL; l = l->next)
        gtk_widget_destroy(GTK_WIDGET(l->data));
    g_list_free(existing_rows);

    if (plugin->bluetooth_row_by_path != NULL)
        g_hash_table_remove_all(plugin->bluetooth_row_by_path);

    if (!plugin->bluetooth_last_available)
        return;

    if (!plugin->bluetooth_last_powered)
    {
        gtk_list_box_insert(GTK_LIST_BOX(plugin->bluetooth_list_box),
                            make_bluetooth_placeholder_row("Bluetooth is turned off."), -1);
        return;
    }

    if (count == 0)
    {
        gtk_list_box_insert(GTK_LIST_BOX(plugin->bluetooth_list_box),
                            make_bluetooth_placeholder_row("Searching for devices\xE2\x80\xA6"), -1); /* "…" */
        return;
    }

    for (guint i = 0; i < count; i++)
    {
        GtkWidget *row = make_bluetooth_row(plugin, &devices[i]);
        gtk_list_box_insert(GTK_LIST_BOX(plugin->bluetooth_list_box), row, -1);
        if (plugin->bluetooth_row_by_path != NULL)
            g_hash_table_insert(plugin->bluetooth_row_by_path, g_strdup(devices[i].path), row);
    }
}

/* Fired when a connect/disconnect request started by
 * on_bluetooth_row_activated() finishes. */
static void
on_bluetooth_operation_result(gboolean success, const gchar *error_message, gpointer user_data)
{
    ExtrasMenuPlugin *plugin = EXTRAS_MENU_PLUGIN(user_data);

    gchar *path = plugin->bluetooth_busy_path;
    const gchar *failure_title = plugin->bluetooth_busy_failure_title;
    plugin->bluetooth_busy_path = NULL;
    plugin->bluetooth_busy_text = NULL;
    plugin->bluetooth_busy_failure_title = NULL;

    /* On success the device's own Connected change arrives through the
     * list update; on failure nothing changed, so either way the row
     * goes back to its resting status. */
    set_bluetooth_row_status(plugin, path, NULL);
    g_free(path);

    if (!success)
    {
        extras_menu_dropdown_show_message(plugin, GTK_MESSAGE_ERROR,
                            failure_title != NULL ? failure_title : "The Bluetooth request failed.",
                            error_message != NULL ? error_message : "Unknown error");
    }
}

/* Fired when a device row is clicked: connects to the device (pairing it
 * first if it isn't paired yet), or disconnects it if it's connected.
 * One request at a time -- clicks while one is running are ignored. */
static void
on_bluetooth_row_activated(GtkListBox *list_box, GtkListBoxRow *row, gpointer user_data)
{
    ExtrasMenuPlugin *plugin = EXTRAS_MENU_PLUGIN(user_data);
    (void) list_box;

    const gchar *path = g_object_get_data(G_OBJECT(row), "extras-menu-bt-path");
    if (path == NULL || plugin->bluetooth == NULL || plugin->bluetooth_busy_path != NULL)
        return;

    gboolean paired = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(row), "extras-menu-bt-paired"));
    gboolean connected = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(row), "extras-menu-bt-connected"));

    /* Set before calling the backend: it may report a failure to start
     * synchronously, and on_bluetooth_operation_result() must find the
     * request registered by then. */
    plugin->bluetooth_busy_path = g_strdup(path);
    if (connected)
    {
        plugin->bluetooth_busy_text = "Disconnecting\xE2\x80\xA6";
        plugin->bluetooth_busy_failure_title = "Couldn't disconnect from the device.";
    }
    else if (paired)
    {
        plugin->bluetooth_busy_text = "Connecting\xE2\x80\xA6";
        plugin->bluetooth_busy_failure_title = "Couldn't connect to the device.";
    }
    else
    {
        plugin->bluetooth_busy_text = "Pairing\xE2\x80\xA6";
        plugin->bluetooth_busy_failure_title = "Couldn't pair with the device.";
    }

    set_bluetooth_row_status(plugin, plugin->bluetooth_busy_path, plugin->bluetooth_busy_text);

    if (connected)
    {
        extras_menu_bluetooth_disconnect_device(plugin->bluetooth, plugin->bluetooth_busy_path,
                                                 on_bluetooth_operation_result, plugin);
    }
    else
    {
        extras_menu_bluetooth_connect_device(plugin->bluetooth, plugin->bluetooth_busy_path,
                                              on_bluetooth_operation_result, plugin);
    }
}

static void
on_bluetooth_forget_result(gboolean success, const gchar *error_message, gpointer user_data)
{
    ExtrasMenuPlugin *plugin = EXTRAS_MENU_PLUGIN(user_data);

    if (!success)
    {
        extras_menu_dropdown_show_message(plugin, GTK_MESSAGE_ERROR, "Couldn't forget the device.",
                            error_message != NULL ? error_message : "Unknown error");
    }
}

static void
on_bluetooth_forget_menu_item_activated(GtkMenuItem *item, gpointer user_data)
{
    ExtrasMenuPlugin *plugin = EXTRAS_MENU_PLUGIN(user_data);
    const gchar *path = g_object_get_data(G_OBJECT(item), "extras-menu-bt-path");

    if (path != NULL && plugin->bluetooth != NULL)
    {
        extras_menu_bluetooth_remove_device(plugin->bluetooth, path,
                                             on_bluetooth_forget_result, plugin);
    }
}

/* Right-click on a paired device offers "Forget this device". Handled on
 * the list box rather than per row, and the menu gets its own copy of
 * the path, for the same reasons as the Wi-Fi list's menu (the list is
 * rebuilt on every update, which can happen while the menu is open). */
static gboolean
on_bluetooth_list_button_press(GtkWidget *list_box, GdkEventButton *event, gpointer user_data)
{
    ExtrasMenuPlugin *plugin = EXTRAS_MENU_PLUGIN(user_data);

    if (event->type != GDK_BUTTON_PRESS || event->button != GDK_BUTTON_SECONDARY)
        return GDK_EVENT_PROPAGATE;

    GtkListBoxRow *row = gtk_list_box_get_row_at_y(GTK_LIST_BOX(list_box), (gint) event->y);
    if (row == NULL)
        return GDK_EVENT_PROPAGATE;

    const gchar *path = g_object_get_data(G_OBJECT(row), "extras-menu-bt-path");
    gboolean paired = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(row), "extras-menu-bt-paired"));
    if (path == NULL || !paired)
        return GDK_EVENT_PROPAGATE;

    GtkWidget *menu = gtk_menu_new();

    GtkWidget *forget_item = gtk_menu_item_new_with_label("Forget this device");
    g_object_set_data_full(G_OBJECT(forget_item), "extras-menu-bt-path", g_strdup(path), g_free);
    g_signal_connect(forget_item, "activate",
                      G_CALLBACK(on_bluetooth_forget_menu_item_activated), plugin);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), forget_item);

    gtk_widget_show_all(menu);
    g_signal_connect(menu, "selection-done", G_CALLBACK(gtk_widget_destroy), NULL);
    g_signal_connect(menu, "selection-done", G_CALLBACK(extras_menu_dropdown_context_menu_done), plugin);
    gtk_menu_popup_at_pointer(GTK_MENU(menu), (GdkEvent *) event);

    return GDK_EVENT_STOP;
}

/* Fired when the small chevron on the Bluetooth pill is clicked: opens
 * or closes the device list. Opening it also starts scanning for nearby
 * devices (see extras_menu_bluetooth_ui_set_list_open()) and closes the Wi-Fi list, so
 * the dropdown never shows both at once. */
static void
on_bluetooth_expand_clicked(GtkButton *button, ExtrasMenuPlugin *plugin)
{
    (void) button;

    gboolean currently_open =
        plugin->bluetooth_revealer != NULL &&
        gtk_revealer_get_reveal_child(GTK_REVEALER(plugin->bluetooth_revealer));

    if (currently_open)
    {
        extras_menu_bluetooth_ui_set_list_open(plugin, FALSE);
        return;
    }

    if (!plugin->bluetooth_last_available)
        return;

    extras_menu_network_ui_set_list_open(plugin, FALSE);
    extras_menu_bluetooth_ui_set_list_open(plugin, TRUE);
}

/* Creates the device-path -> row lookup, starts the Bluetooth backend and
 * wires the pill and the device list to it. Needs the dropdown content to
 * exist. Catches up on any state the backend reported before the widgets
 * existed. */
void
extras_menu_bluetooth_ui_attach(ExtrasMenuPlugin *plugin)
{
    /* Same idea for the Bluetooth device list, keyed by device path. */
    plugin->bluetooth_row_by_path = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);

    /* --- Bluetooth backend: finds the first adapter async, reports
     * its Powered state (and every subsequent change) through
     * on_bluetooth_changed, and the device list through
     * on_bluetooth_devices_changed. The pill's main toggle turns the
     * adapter on/off; its chevron opens the device list, where clicking
     * a device connects (pairing first if needed) or disconnects. --- */
    plugin->bluetooth = extras_menu_bluetooth_new(on_bluetooth_changed,
                                                   on_bluetooth_devices_changed, plugin);

    if (plugin->bluetooth_expand_button != NULL)
    {
        /* Disabled until an adapter is confirmed, like the toggle. */
        gtk_widget_set_sensitive(plugin->bluetooth_expand_button, FALSE);
        g_signal_connect(plugin->bluetooth_expand_button, "clicked",
                          G_CALLBACK(on_bluetooth_expand_clicked), plugin);
    }

    if (plugin->bluetooth_list_box != NULL)
    {
        g_signal_connect(plugin->bluetooth_list_box, "row-activated",
                          G_CALLBACK(on_bluetooth_row_activated), plugin);

        gtk_widget_add_events(plugin->bluetooth_list_box, GDK_BUTTON_PRESS_MASK);
        g_signal_connect(plugin->bluetooth_list_box, "button-press-event",
                          G_CALLBACK(on_bluetooth_list_button_press), plugin);
    }

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
}

/* Stops the Bluetooth backend and releases what this module keeps on the
 * plugin. */
void
extras_menu_bluetooth_ui_free(ExtrasMenuPlugin *plugin)
{
    if (plugin->bluetooth != NULL)
    {
        extras_menu_bluetooth_free(plugin->bluetooth);
        plugin->bluetooth = NULL;
    }

    g_free(plugin->bluetooth_busy_path);
    plugin->bluetooth_busy_path = NULL;

    if (plugin->bluetooth_row_by_path != NULL)
    {
        g_hash_table_destroy(plugin->bluetooth_row_by_path);
        plugin->bluetooth_row_by_path = NULL;
    }
}