#include "extras-menu.h"
#include "dropdown.h"
#include "dropdown-style.h"
#include "system-status.h"
#include "popover.h"
#include "preferences/preferences.h"
#include "preferences/quick-actions-config.h"
#include "preferences/pill-layout-config.h"
#include "preferences/slider-layout-config.h"
#include "quick-actions.h"
#include "network/network-ui.h"
#include "bluetooth/bluetooth-ui.h"

#include <gio/gio.h>

/*
 * The plugin itself: type registration, instance setup, and construct(),
 * which puts the pieces together. The pieces live in their own files:
 *
 *   dropdown.c               the dropdown window, click-away, dialog tracking
 *   system-status.c          panel button icons, battery badge, sliders
 *   network/network-ui.c     network pill and Wi-Fi list
 *   network/network-dialogs.c  password prompt, network menu and details
 *   bluetooth/bluetooth-ui.c Bluetooth pill and device list
 *   popover.c                builds the dropdown's widgets
 *
 * What stays here is what belongs to no single one of them: the
 * Aeroplane Mode glue (it switches both Wi-Fi and Bluetooth) and the
 * handlers for the preferences editor's saved settings.
 */

/* The quick-actions config was saved (button list or shape): make the
 * buttons' shape match the saved setting. Cheap and idempotent, so it
 * doesn't matter that it also runs for plain list edits. */
static void
on_quick_actions_config_changed(gpointer user_data)
{
    ExtrasMenuPlugin *plugin = user_data;

    extras_menu_popover_set_quick_action_shape(plugin->quick_actions_box,
                                                extras_menu_quick_actions_config_get_shape());
}

/* The saved pill layout or pill shape changed: show, hide, move and
 * restyle the dropdown's pills to match. (The dropdown is built in the
 * saved layout and shape to begin with, so this is only for changes made
 * afterwards. Both live in the same config file and share one listener;
 * applying either again when only the other changed is harmless.) */
static void
on_pill_layout_changed(gpointer user_data)
{
    ExtrasMenuPlugin *plugin = user_data;

    if (plugin->popover == NULL)
        return;

    GPtrArray *layout = extras_menu_pill_layout_load();
    GtkWidget *content = gtk_bin_get_child(GTK_BIN(plugin->popover));
    extras_menu_popover_set_pill_order(content, layout);
    extras_menu_popover_set_pill_shape(content, extras_menu_pill_layout_get_shape());

    /* A list can only be opened with the chevron of its own pill. If
     * that pill was just removed while its list was open, close the list
     * too (which also stops Bluetooth scanning) -- otherwise it would
     * stay open with nothing left to close it. */
    if (!extras_menu_pill_layout_contains(layout, EXTRAS_MENU_PILL_NETWORK))
        extras_menu_network_ui_set_list_open(plugin, FALSE);
    if (!extras_menu_pill_layout_contains(layout, EXTRAS_MENU_PILL_BLUETOOTH))
        extras_menu_bluetooth_ui_set_list_open(plugin, FALSE);

    g_ptr_array_unref(layout);
}

/* The saved slider layout changed: reorder, show and hide the volume
 * and brightness rows to match. (The dropdown is built in the saved
 * layout to begin with, so this is only for changes made afterwards.) */
static void
on_slider_layout_changed(gpointer user_data)
{
    ExtrasMenuPlugin *plugin = user_data;

    if (plugin->popover == NULL)
        return;

    GPtrArray *layout = extras_menu_slider_layout_load();
    extras_menu_popover_set_slider_order(gtk_bin_get_child(GTK_BIN(plugin->popover)), layout);
    g_ptr_array_unref(layout);
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

/* --- Aeroplane Mode ------------------------------------------------------------- */

/* How pills/airplane-mode.c switches the two radios: through the same
 * backends the Wi-Fi and Bluetooth pills use, so those pills follow
 * along through their usual callbacks. */
static void
airplane_set_wifi(gboolean enabled, gpointer user_data)
{
    ExtrasMenuPlugin *plugin = EXTRAS_MENU_PLUGIN(user_data);

    extras_menu_network_set_wifi_enabled(plugin->network, enabled);
}

static void
airplane_set_bluetooth(gboolean powered, gpointer user_data)
{
    ExtrasMenuPlugin *plugin = EXTRAS_MENU_PLUGIN(user_data);

    extras_menu_bluetooth_set_powered(plugin->bluetooth, powered);
}

/* Aeroplane mode switched on or off: the panel button's network icon
 * shows an aeroplane while it is on (see extras_menu_system_status_update_network_icon()). */
static void
on_airplane_mode_changed(gboolean active, gpointer user_data)
{
    (void) active;
    extras_menu_system_status_update_network_icon(EXTRAS_MENU_PLUGIN(user_data));
}

/* Dark Mode can't switch (a theme has no dark or light version
 * installed): tell the user, in a dialog that belongs to the dropdown so
 * that it doesn't close behind it. */
static void
on_dark_mode_message(const gchar *primary, const gchar *secondary, gpointer user_data)
{
    extras_menu_dropdown_show_message(EXTRAS_MENU_PLUGIN(user_data), GTK_MESSAGE_WARNING,
                                       primary, secondary);
}

/* Caffeine couldn't be turned on (no service accepted the request): tell
 * the user, in a dialog that belongs to the dropdown. */
static void
on_caffeine_message(const gchar *primary, const gchar *secondary, gpointer user_data)
{
    extras_menu_dropdown_show_message(EXTRAS_MENU_PLUGIN(user_data), GTK_MESSAGE_WARNING,
                                       primary, secondary);
}

static void
on_plugin_free_data(XfcePanelPlugin *panel_plugin, ExtrasMenuPlugin *plugin)
{
    (void) panel_plugin;

    extras_menu_dropdown_style_free(plugin);

    /* The dropdown may still be open: don't leave the panel held up. */
    extras_menu_dropdown_release_panel(plugin);

    /* Before the backends it switches go away. */
    extras_menu_airplane_mode_free(plugin->airplane_mode);
    plugin->airplane_mode = NULL;

    extras_menu_microphone_free(plugin->microphone);
    plugin->microphone = NULL;

    extras_menu_power_profile_free(plugin->power_profile);
    plugin->power_profile = NULL;

    extras_menu_dark_mode_free(plugin->dark_mode);
    plugin->dark_mode = NULL;

    /* Lets the screensaver and power manager go if Caffeine is on. */
    extras_menu_caffeine_free(plugin->caffeine);
    plugin->caffeine = NULL;

    /* Puts the screen's colors back if Night Light is on. */
    extras_menu_night_light_free(plugin->night_light);
    plugin->night_light = NULL;

    if (plugin->slider_layout_listener != 0)
    {
        extras_menu_slider_layout_remove_listener(plugin->slider_layout_listener);
        plugin->slider_layout_listener = 0;
    }

    if (plugin->pill_layout_listener != 0)
    {
        extras_menu_pill_layout_remove_listener(plugin->pill_layout_listener);
        plugin->pill_layout_listener = 0;
    }

    if (plugin->quick_actions_config_listener != 0)
    {
        extras_menu_quick_actions_config_remove_listener(plugin->quick_actions_config_listener);
        plugin->quick_actions_config_listener = 0;
    }

    extras_menu_system_status_free(plugin);
    extras_menu_bluetooth_ui_free(plugin);
    extras_menu_network_ui_free(plugin);

    /* Last: the backends above must be stopped before the widgets they
     * feed are destroyed. */
    extras_menu_dropdown_destroy(plugin);
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
    /* GObject hands out a zero-filled instance, so every pointer is NULL,
     * every flag FALSE and every counter 0 already; listing them all
     * again here only made it easy to forget one when a field was added.
     * Only the fields whose "empty" value isn't zero are set. Widgets
     * are built in construct(), once the plugin has a panel to attach
     * to. */
    plugin->network_last_kind = EXTRAS_MENU_NETWORK_KIND_NONE;
    plugin->pending_connect_security = EXTRAS_MENU_AP_SECURITY_UNKNOWN;
}

static void
extras_menu_plugin_construct(XfcePanelPlugin *panel_plugin)
{
    ExtrasMenuPlugin *plugin = EXTRAS_MENU_PLUGIN(panel_plugin);

    /* The panel button first: the dropdown is positioned and toggled by it. */
    extras_menu_system_status_build_panel_button(plugin);

    /* The dropdown window, built once and reused on every open. */
    extras_menu_dropdown_create(plugin);

    GtkWidget *content = extras_menu_popover_content_new(&plugin->battery_label,
                                                           &plugin->battery_icon,
                                                           &plugin->quick_actions_box,
                                                           &plugin->volume_scale,
                                                           &plugin->volume_icon,
                                                           &plugin->brightness_scale,
                                                           &plugin->bluetooth_toggle,
                                                           &plugin->bluetooth_expand_button,
                                                           &plugin->bluetooth_expand_chevron,
                                                           &plugin->bluetooth_revealer,
                                                           &plugin->bluetooth_list_box,
                                                           &plugin->network_toggle,
                                                           &plugin->network_pill_label,
                                                           &plugin->network_pill_icon,
                                                           &plugin->network_expand_button,
                                                           &plugin->network_expand_chevron,
                                                           &plugin->network_icon_stack,
                                                           &plugin->network_spinner,
                                                           &plugin->network_revealer,
                                                           &plugin->network_list_box);
    gtk_container_add(GTK_CONTAINER(plugin->popover), content);

    /* The dropdown's background: the panel's own color and transparency.
     * Needs the content and its two lists, so only now. */
    extras_menu_dropdown_style_attach(plugin);

    /* --- battery badge and backend --- */
    extras_menu_system_status_attach_battery(plugin);

    extras_menu_quick_actions_attach(plugin->quick_actions_box,
                                      extras_menu_dropdown_close, plugin);

    /* Button shape (square/circle): apply the saved one now, and again
     * whenever the preferences editor saves. */
    on_quick_actions_config_changed(plugin);
    plugin->quick_actions_config_listener =
        extras_menu_quick_actions_config_add_listener(on_quick_actions_config_changed, plugin);

    /* Pill order: the content above was already built in the saved
     * layout; this keeps it in step with later changes. */
    plugin->pill_layout_listener =
        extras_menu_pill_layout_add_listener(on_pill_layout_changed, plugin);

    /* Slider rows: likewise built in the saved layout, kept in step with
     * later changes. */
    plugin->slider_layout_listener =
        extras_menu_slider_layout_add_listener(on_slider_layout_changed, plugin);

    /* --- volume and brightness sliders and their backends --- */
    extras_menu_system_status_attach_sliders(plugin);

    /* --- microphone slider: level, mute click, and its backend. It
     * finds its own widgets in the content, so it needs no field or
     * out-param of its own here. --- */
    GtkWidget *mic_scale = NULL;
    GtkWidget *mic_button = NULL;
    GtkWidget *mic_icon = NULL;
    if (extras_menu_popover_get_microphone(content, &mic_scale, &mic_button, &mic_icon))
        plugin->microphone = extras_menu_microphone_new(mic_scale, mic_button, mic_icon);

    /* --- Bluetooth pill and device list, and their backend --- */
    extras_menu_bluetooth_ui_attach(plugin);

    /* --- network pill and Wi-Fi list, and their backend --- */
    extras_menu_network_ui_attach(plugin);

    /* --- Aeroplane Mode pill: turns Wi-Fi and Bluetooth off together.
     * Created after both backends so it can switch them; the state they
     * already reported before this point is handed over below. --- */
    GtkWidget *airplane_pill = extras_menu_popover_get_pill(content, EXTRAS_MENU_PILL_AIRPLANE);
    if (airplane_pill != NULL)
    {
        plugin->airplane_mode = extras_menu_airplane_mode_new(airplane_pill,
                                                               airplane_set_wifi,
                                                               airplane_set_bluetooth,
                                                               on_airplane_mode_changed, plugin);

        if (plugin->network_wifi_has_enabled_state)
        {
            extras_menu_airplane_mode_wifi_changed(plugin->airplane_mode,
                                                    plugin->network_wifi_last_enabled);
        }
        if (plugin->bluetooth_has_state)
        {
            extras_menu_airplane_mode_bluetooth_changed(plugin->airplane_mode,
                                                         plugin->bluetooth_last_available,
                                                         plugin->bluetooth_last_powered);
        }
    }

    /* --- Balanced pill: shows and cycles the power profile. It talks to
     * power-profiles-daemon on its own, so it needs nothing from the
     * backends above. --- */
    GtkWidget *balanced_pill = extras_menu_popover_get_pill(content, EXTRAS_MENU_PILL_BALANCED);
    if (balanced_pill != NULL)
        plugin->power_profile = extras_menu_power_profile_new(balanced_pill);

    /* --- Dark Mode pill: lit while the GTK theme is a dark one, and
     * switches between the theme's dark and light version. It reads and
     * writes Xfce's theme setting itself, so it needs nothing from the
     * backends above. --- */
    GtkWidget *dark_mode_pill = extras_menu_popover_get_pill(content, EXTRAS_MENU_PILL_DARK_MODE);
    if (dark_mode_pill != NULL)
        plugin->dark_mode = extras_menu_dark_mode_new(dark_mode_pill, on_dark_mode_message, plugin);

    /* --- Night Light pill: warms the screen's colors and back again.
     * It talks to the X server on its own and reads its temperature
     * and transition time from the saved settings, so it needs nothing
     * from the backends above. --- */
    GtkWidget *night_light_pill = extras_menu_popover_get_pill(content, EXTRAS_MENU_PILL_NIGHT_LIGHT);
    if (night_light_pill != NULL)
        plugin->night_light = extras_menu_night_light_new(night_light_pill);

    /* --- Caffeine pill: keeps the screen on and the computer awake. It
     * talks to the session's screensaver and power manager on its own and
     * reads how long to stay on from the saved settings, so it needs
     * nothing from the backends above. --- */
    GtkWidget *caffeine_pill = extras_menu_popover_get_pill(content, EXTRAS_MENU_PILL_CAFFEINE);
    if (caffeine_pill != NULL)
        plugin->caffeine = extras_menu_caffeine_new(caffeine_pill, on_caffeine_message, plugin);

    g_signal_connect(panel_plugin, "free-data",
                      G_CALLBACK(on_plugin_free_data), plugin);

    /* --- "Properties..." entry in the plugin's right-click panel menu,
     * opening the preferences dialog --- */
    xfce_panel_plugin_menu_show_configure(panel_plugin);
    g_signal_connect(panel_plugin, "configure-plugin",
                      G_CALLBACK(on_plugin_configure), plugin);

    gtk_widget_show_all(plugin->toggle_button);
}