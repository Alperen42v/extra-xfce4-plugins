/*
 * The dropdown window: opening, closing and positioning it, closing it
 * when the user clicks away, and keeping track of the dialogs it opens.
 * See dropdown.h for what other modules can call.
 */

#include "dropdown.h"
#include "network/network-ui.h"
#include "bluetooth/bluetooth-ui.h"

/* --- dialogs ---------------------------------------------------------------
 *
 * Every dialog the dropdown opens (password prompt, errors, Ethernet
 * info, network details) is non-blocking: it is shown, and its
 * "response" handler destroys it. gtk_dialog_run() is deliberately not
 * used anywhere -- it spins a nested main loop inside whatever D-Bus or
 * GTK callback called it, so other callbacks (a rescan rebuilding the
 * network list, a backend update, ...) would run while that callback is
 * still half-finished on the stack.
 *
 * Opening a dialog moves keyboard focus away from the dropdown, which
 * would normally trigger on_dropdown_focus_out() and close the dropdown
 * -- leaving the dialog parented to a hidden window. extras_menu_dropdown_track_dialog()
 * counts open dialogs so the focus-out handler can ignore focus moving
 * to one of them, and gives focus back to the dropdown once the last
 * one is gone so click-away-to-close works again. */

static void
on_tracked_dialog_destroyed(GtkWidget *dialog, gpointer user_data)
{
    ExtrasMenuPlugin *plugin = EXTRAS_MENU_PLUGIN(user_data);
    (void) dialog;

    if (plugin->open_dialog_count > 0)
        plugin->open_dialog_count--;

    if (plugin->open_dialog_count == 0 && plugin->popover != NULL &&
        gtk_widget_get_visible(plugin->popover))
    {
        gtk_window_present(GTK_WINDOW(plugin->popover));
    }
}

void
extras_menu_dropdown_track_dialog(ExtrasMenuPlugin *plugin, GtkWidget *dialog)
{
    plugin->open_dialog_count++;
    g_signal_connect(dialog, "destroy", G_CALLBACK(on_tracked_dialog_destroyed), plugin);
}

/* Response handler for dialogs that only need to go away when answered. */
void
extras_menu_dropdown_dialog_response_destroy(GtkDialog *dialog, gint response_id, gpointer user_data)
{
    (void) response_id;
    (void) user_data;
    gtk_widget_destroy(GTK_WIDGET(dialog));
}

/* Shows a simple non-blocking message dialog with an OK button.
 * secondary may be NULL. */
void
extras_menu_dropdown_show_message(ExtrasMenuPlugin *plugin, GtkMessageType type,
                    const gchar *primary, const gchar *secondary)
{
    GtkWidget *dialog = gtk_message_dialog_new(
        GTK_WINDOW(plugin->popover), GTK_DIALOG_DESTROY_WITH_PARENT,
        type, GTK_BUTTONS_OK, "%s", primary);

    if (secondary != NULL)
    {
        gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(dialog),
                                                  "%s", secondary);
    }

    extras_menu_dropdown_track_dialog(plugin, dialog);
    g_signal_connect(dialog, "response", G_CALLBACK(extras_menu_dropdown_dialog_response_destroy), NULL);
    gtk_widget_show(dialog);
}

/* --- click-away-to-close -------------------------------------------------------
 *
 * The dropdown is a GTK_WINDOW_POPUP, which the window manager never
 * gives keyboard focus to, so "close when focus leaves" alone can't
 * notice clicks on the desktop, other windows or other panel plugins.
 * Like GTK's own menus, the dropdown therefore grabs the pointer while
 * it is open. With owner_events set, clicks on this process's own
 * windows (the panel button, our dialogs, context menus) still go to
 * those windows as usual; every other click is reported to the dropdown
 * instead, where on_dropdown_button_press() closes it if it landed
 * outside. Only pointing devices are grabbed, never the keyboard. */

static void
grab_dropdown_input(ExtrasMenuPlugin *plugin)
{
    if (plugin->popover == NULL || !gtk_widget_get_visible(plugin->popover))
        return;

    GdkWindow *window = gtk_widget_get_window(plugin->popover);
    if (window == NULL)
        return;

    GdkSeat *seat = gdk_display_get_default_seat(gtk_widget_get_display(plugin->popover));
    GdkGrabStatus status = gdk_seat_grab(seat, window, GDK_SEAT_CAPABILITY_ALL_POINTING,
                                          TRUE, NULL, NULL, NULL, NULL);
    if (status != GDK_GRAB_SUCCESS)
        g_debug("extras-menu: pointer grab failed (%d)", (gint) status);
}

static void
release_dropdown_input(ExtrasMenuPlugin *plugin)
{
    if (plugin->popover == NULL)
        return;

    gdk_seat_ungrab(gdk_display_get_default_seat(gtk_widget_get_display(plugin->popover)));
}

/* A right-click context menu takes over the pointer grab while it is
 * open, and it is gone for good once the menu closes. Take it back
 * afterwards (from an idle, so the menu has finished releasing its own
 * grab first), otherwise click-away would stop working after the
 * first context menu. */
static gboolean
regrab_dropdown_input_idle(gpointer user_data)
{
    grab_dropdown_input(user_data);
    return G_SOURCE_REMOVE;
}

void
extras_menu_dropdown_context_menu_done(GtkMenuShell *menu, gpointer user_data)
{
    (void) menu;
    g_idle_add(regrab_dropdown_input_idle, user_data);
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
    {
        /* The bottom edge is what stays put: remember it, so that
         * on_dropdown_check_resize() can keep it there while a list
         * opens or closes and the window changes height. */
        plugin->dropdown_anchor_bottom = origin_y + button_alloc.y;
        plugin->dropdown_anchored_bottom = TRUE;
        y = plugin->dropdown_anchor_bottom - natural_size.height;
    }
    else
    {
        plugin->dropdown_anchored_bottom = FALSE;
        y = origin_y + button_alloc.y + button_alloc.height;
    }

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

/* With a bottom panel the dropdown grows upward: when the Wi-Fi or
 * Bluetooth list opens (or closes) the window changes height, and GTK
 * keeps its top-left corner where it was -- so it would grow downward,
 * over the panel. Instead the window's top edge is kept at
 * (anchored bottom edge - height). The revealers animate, so this
 * happens on every frame of the animation and the window slides up (or
 * back down) smoothly with the list.
 *
 * ORDER MATTERS, or the window flickers. GTK resizes the window with
 * its top-left corner fixed, and only learns that the resize happened
 * when the X server's answer comes back; moving the window at that
 * point (in "size-allocate") leaves a gap, one round trip long, in which
 * the compositor can draw the window at its new size but still at the
 * old position. So the move is made first, in "check-resize": that runs
 * right before GTK sends its resize request, so both requests go out
 * together and the window never exists at a wrong position. For that the
 * new height has to be known beforehand; it is asked from the widgets,
 * the same way GTK is about to do. "size-allocate" stays as a safety net
 * that corrects the position if that prediction was ever off by a pixel.
 *
 * None of this does anything for other panel positions, or before the
 * window has been positioned (dropdown_anchored_bottom is FALSE then). */

/* Puts the dropdown's top edge where a window `height` pixels tall
 * must start for its bottom edge to sit on the anchor. */
static void
move_dropdown_to_anchor(ExtrasMenuPlugin *plugin, GtkWidget *widget, gint height)
{
    gint x = 0, y = 0;
    gtk_window_get_position(GTK_WINDOW(widget), &x, &y);

    gint new_y = plugin->dropdown_anchor_bottom - height;

    /* A very tall list must not push the top of the dropdown off the
     * top of the screen. */
    GdkWindow *window = gtk_widget_get_window(widget);
    if (window != NULL)
    {
        GdkMonitor *monitor =
            gdk_display_get_monitor_at_window(gtk_widget_get_display(widget), window);
        if (monitor != NULL)
        {
            GdkRectangle monitor_geom;
            gdk_monitor_get_geometry(monitor, &monitor_geom);
            if (new_y < monitor_geom.y)
                new_y = monitor_geom.y;
        }
    }

    if (new_y != y)
        gtk_window_move(GTK_WINDOW(widget), x, new_y);
}

/* Runs just before GTK resizes the window (a "check-resize" handler
 * connected normally runs before the window's own, which is what sends
 * the resize request). If the window is about to change height, move it
 * to where the new height needs it to be. */
static void
on_dropdown_check_resize(GtkContainer *container, ExtrasMenuPlugin *plugin)
{
    GtkWidget *widget = GTK_WIDGET(container);

    if (!plugin->dropdown_anchored_bottom || !gtk_widget_get_mapped(widget))
        return;

    gint width = gtk_widget_get_allocated_width(widget);
    gint current_height = gtk_widget_get_allocated_height(widget);
    gint new_height = current_height;

    gtk_widget_get_preferred_height_for_width(widget, width, NULL, &new_height);

    if (new_height != current_height)
        move_dropdown_to_anchor(plugin, widget, new_height);
}

/* Safety net: after the resize has really happened, make sure the
 * window sits where its actual height needs it to. Normally the move
 * made by on_dropdown_check_resize() already put it there and this does
 * nothing. */
static void
on_dropdown_size_allocate(GtkWidget *widget, GdkRectangle *allocation,
                          ExtrasMenuPlugin *plugin)
{
    if (!plugin->dropdown_anchored_bottom || !gtk_widget_get_visible(widget))
        return;

    move_dropdown_to_anchor(plugin, widget, allocation->height);
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
    }
    else
    {
        gtk_widget_hide(plugin->popover);
    }
}

/* The dropdown was hidden (by any route: clicking away, the panel button,
 * a quick action). Both lists are collapsed so the dropdown always
 * opens in its compact form next time, instead of coming back with a
 * list left open. Closing the Bluetooth list also stops its scan, which
 * must not keep running behind a window nobody can see. */
static void
on_dropdown_hidden(GtkWidget *widget, ExtrasMenuPlugin *plugin)
{
    (void) widget;
    release_dropdown_input(plugin);

    /* Collapsing the lists below changes the window's size; with no
     * anchor, that can't move a window that is no longer shown. It is
     * set again by position_dropdown_below_button() on the next open. */
    plugin->dropdown_anchored_bottom = FALSE;

    extras_menu_network_ui_set_list_open(plugin, FALSE);
    extras_menu_bluetooth_ui_set_list_open(plugin, FALSE);
}

/* Hides the dropdown and resets the toggle button to match. Shared by the
 * focus-out handler below and by the quick-action buttons (which close the
 * dropdown before launching their program). Takes a gpointer so it can
 * double as an ExtrasMenuQuickActionsCloseFunc. */
void
extras_menu_dropdown_close(gpointer user_data)
{
    ExtrasMenuPlugin *plugin = user_data;

    gtk_widget_hide(plugin->popover);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(plugin->toggle_button), FALSE);
}

/* The dropdown's X window is now mapped, so the pointer can be grabbed
 * (a grab needs a viewable window, which it is not yet right after
 * gtk_window_present()). */
static gboolean
on_dropdown_map_event(GtkWidget *widget, GdkEvent *event, ExtrasMenuPlugin *plugin)
{
    (void) widget;
    (void) event;

    grab_dropdown_input(plugin);
    return FALSE;
}

/* With the pointer grabbed (see grab_dropdown_input()), a click that
 * lands outside the dropdown -- on the desktop, another window, another
 * panel plugin -- is delivered here. Compared in root coordinates so it
 * doesn't matter which child window the event was reported against.
 * Ignored while one of our own dialogs is open, same as the focus-out
 * handler. */
static gboolean
on_dropdown_button_press(GtkWidget *widget, GdkEventButton *event, ExtrasMenuPlugin *plugin)
{
    if (plugin->open_dialog_count > 0)
        return GDK_EVENT_PROPAGATE;

    GdkWindow *window = gtk_widget_get_window(widget);
    if (window == NULL)
        return GDK_EVENT_PROPAGATE;

    gint origin_x = 0, origin_y = 0;
    gdk_window_get_origin(window, &origin_x, &origin_y);

    gint width = gtk_widget_get_allocated_width(widget);
    gint height = gtk_widget_get_allocated_height(widget);

    if (event->x_root >= origin_x && event->x_root < origin_x + width &&
        event->y_root >= origin_y && event->y_root < origin_y + height)
        return GDK_EVENT_PROPAGATE; /* inside: a normal click on the dropdown */

    extras_menu_dropdown_close(plugin);
    return GDK_EVENT_STOP;
}

/* Since we're using a plain GtkWindow instead of a GtkPopover, we lose
 * the automatic "close when focus leaves" behaviour popovers get for
 * free. This reimplements it: whenever the dropdown window loses
 * keyboard focus (the user clicked elsewhere, or switched windows),
 * close it. */
static gboolean
on_dropdown_focus_out(GtkWidget *widget, GdkEventFocus *event, ExtrasMenuPlugin *plugin)
{
    (void) widget;
    (void) event;

    /* Focus moving to one of our own dialogs (password prompt, error,
     * details, ...) is not "the user clicked away" -- closing here
     * would hide the window those dialogs are parented to. Focus is
     * handed back to the dropdown when the last dialog closes (see
     * on_tracked_dialog_destroyed()). */
    if (plugin->open_dialog_count > 0)
        return FALSE;

    extras_menu_dropdown_close(plugin);

    return FALSE;
}

/* Builds the dropdown window and wires up everything that makes it open, close
 * and stay on top. Needs plugin->toggle_button to exist already. The content
 * is added to the window afterwards by the caller. */
void
extras_menu_dropdown_create(ExtrasMenuPlugin *plugin)
{
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

    /* Without this, the window keeps the screen's default (opaque)
     * visual, so its true X11 shape stays a plain rectangle no matter
     * what border-radius we draw in CSS -- the corner pixels CSS
     * "cuts off" just fall through to that rectangle's normal (black)
     * background instead of the desktop showing through. An RGBA
     * visual gives the window a real alpha channel, so combined with
     * the transparent window background below, the rounded corners
     * are genuinely transparent rather than painted black. Falls back
     * silently (square-cornered but otherwise fine) on the rare
     * X11 setup with no RGBA visual available/no compositor. */
    {
        GdkScreen *screen = gtk_widget_get_screen(plugin->popover);
        GdkVisual *rgba_visual = gdk_screen_get_rgba_visual(screen);
        if (rgba_visual != NULL)
            gtk_widget_set_visual(plugin->popover, rgba_visual);
    }

    /* Frame + drop shadow so the window doesn't look like a bare
     * rectangle floating over the desktop -- GtkPopover normally gives
     * us this via its own CSS node, which we no longer have. Also sets
     * the window's own background to transparent: some GTK themes
     * paint an opaque "window" background by default even with an
     * RGBA visual set, which would still show as black corners despite
     * the visual change above -- explicit transparent background here
     * closes that gap regardless of the active theme's own rules. */
    {
        GtkStyleContext *ctx = gtk_widget_get_style_context(plugin->popover);
        gtk_style_context_add_class(ctx, "background");

        GtkCssProvider *frame_provider = gtk_css_provider_new();
        gtk_css_provider_load_from_data(
            frame_provider,
            "window { "
            "  background-color: transparent; "
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
    g_signal_connect(plugin->popover, "hide",
                      G_CALLBACK(on_dropdown_hidden), plugin);

    /* Bottom panel: keep the dropdown's bottom edge next to the button
     * while lists open and close. See on_dropdown_check_resize(). */
    g_signal_connect(plugin->popover, "check-resize",
                      G_CALLBACK(on_dropdown_check_resize), plugin);
    g_signal_connect(plugin->popover, "size-allocate",
                      G_CALLBACK(on_dropdown_size_allocate), plugin);

    /* Click-away-to-close: grab the pointer once mapped, and close on
     * any click outside the window. See grab_dropdown_input(). */
    gtk_widget_add_events(plugin->popover, GDK_BUTTON_PRESS_MASK);
    g_signal_connect(plugin->popover, "map-event",
                      G_CALLBACK(on_dropdown_map_event), plugin);
    g_signal_connect(plugin->popover, "button-press-event",
                      G_CALLBACK(on_dropdown_button_press), plugin);
}