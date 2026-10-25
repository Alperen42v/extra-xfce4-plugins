/*
 * Status shown by the panel button and by the top bar / sliders of the
 * dropdown: the three panel icons (network, volume, battery), the battery
 * badge, and the volume and brightness sliders with their backends.
 * See system-status.h.
 */

#include "system-status.h"

/* --- panel button icons ------------------------------------------------------
 *
 * The panel button shows three status icons (network, volume, battery)
 * instead of a chevron. Each update_panel_*() below is called from the
 * matching backend callback, next to the code that updates the
 * dropdown's own widgets. All of them tolerate the icon not existing yet. */

void
extras_menu_system_status_update_network_icon(ExtrasMenuPlugin *plugin)
{
    if (plugin->panel_network_icon == NULL)
        return;

    const gchar *icon_name;

    if (plugin->network_last_kind == EXTRAS_MENU_NETWORK_KIND_ETHERNET)
        icon_name = "network-wired-symbolic";
    else if (extras_menu_airplane_mode_is_active(plugin->airplane_mode))
        icon_name = "airplane-mode-symbolic"; /* aeroplane mode: no Wi-Fi to show */
    else if (plugin->network_wifi_has_enabled_state && !plugin->network_wifi_last_enabled)
        icon_name = "network-wireless-disabled-symbolic"; /* radio switched off */
    else if (plugin->network_last_kind == EXTRAS_MENU_NETWORK_KIND_WIFI)
        icon_name = "network-wireless-symbolic";
    else
        icon_name = "network-wireless-offline-symbolic";

    gtk_image_set_from_icon_name(GTK_IMAGE(plugin->panel_network_icon),
                                  icon_name, GTK_ICON_SIZE_BUTTON);
}

/* Speaker icon for a volume level; muted and 0% look the same to the
 * user, so both get the crossed-out icon. Shared by the dropdown's
 * volume row and the panel button. */
static const gchar *
volume_icon_name(guint volume_percent, gboolean muted)
{
    if (muted || volume_percent == 0)
        return "audio-volume-muted-symbolic";
    if (volume_percent < 34)
        return "audio-volume-low-symbolic";
    if (volume_percent < 67)
        return "audio-volume-medium-symbolic";
    return "audio-volume-high-symbolic";
}

static void
update_panel_volume_icon(ExtrasMenuPlugin *plugin, guint volume_percent, gboolean muted)
{
    if (plugin->panel_volume_icon == NULL)
        return;

    gtk_image_set_from_icon_name(GTK_IMAGE(plugin->panel_volume_icon),
                                  volume_icon_name(volume_percent, muted),
                                  GTK_ICON_SIZE_BUTTON);
}

/* With a battery: its level/charging icon. Without one (desktop, or
 * UPower not running): a power icon, so the panel button never ends up
 * with a gap where the battery would be. */
static void
update_panel_battery_icon(ExtrasMenuPlugin *plugin, gboolean present, guint percent,
                          ExtrasMenuBatteryState state)
{
    if (plugin->panel_battery_icon == NULL)
        return;

    if (!present)
    {
        gtk_image_set_from_icon_name(GTK_IMAGE(plugin->panel_battery_icon),
                                      "system-shutdown-symbolic", GTK_ICON_SIZE_BUTTON);
        return;
    }

    gchar *icon_name = extras_menu_battery_icon_name(percent, state);
    gtk_image_set_from_icon_name(GTK_IMAGE(plugin->panel_battery_icon),
                                  icon_name, GTK_ICON_SIZE_BUTTON);
    g_free(icon_name);
}

/* Keeps the three icons in a row on a horizontal panel and in a column
 * on a vertical one. */
static void
on_plugin_orientation_changed(XfcePanelPlugin *panel_plugin, GtkOrientation orientation,
                              ExtrasMenuPlugin *plugin)
{
    (void) panel_plugin;

    if (plugin->panel_icons_box != NULL)
        gtk_orientable_set_orientation(GTK_ORIENTABLE(plugin->panel_icons_box), orientation);
}

/* Builds the panel button: three status icons (network, volume, battery
 * or power) side by side inside a toggle button, added to the panel. */
void
extras_menu_system_status_build_panel_button(ExtrasMenuPlugin *plugin)
{
    XfcePanelPlugin *panel_plugin = XFCE_PANEL_PLUGIN(plugin);

    /* --- panel button: three status icons (network, volume, battery
     * or power) side by side. Starting icons are placeholders; the
     * backends replace them as soon as they report in. The power icon
     * is also what stays if no battery ever reports. --- */
    plugin->toggle_button = gtk_toggle_button_new();
    gtk_button_set_relief(GTK_BUTTON(plugin->toggle_button), GTK_RELIEF_NONE);

    plugin->panel_icons_box =
        gtk_box_new(xfce_panel_plugin_get_orientation(panel_plugin), 6);

    plugin->panel_network_icon =
        gtk_image_new_from_icon_name("network-wireless-offline-symbolic", GTK_ICON_SIZE_BUTTON);
    plugin->panel_volume_icon =
        gtk_image_new_from_icon_name("audio-volume-high-symbolic", GTK_ICON_SIZE_BUTTON);
    plugin->panel_battery_icon =
        gtk_image_new_from_icon_name("system-shutdown-symbolic", GTK_ICON_SIZE_BUTTON);

    gtk_box_pack_start(GTK_BOX(plugin->panel_icons_box), plugin->panel_network_icon, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(plugin->panel_icons_box), plugin->panel_volume_icon, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(plugin->panel_icons_box), plugin->panel_battery_icon, FALSE, FALSE, 0);
    gtk_container_add(GTK_CONTAINER(plugin->toggle_button), plugin->panel_icons_box);

    g_signal_connect(panel_plugin, "orientation-changed",
                      G_CALLBACK(on_plugin_orientation_changed), plugin);

    gtk_container_add(GTK_CONTAINER(panel_plugin), plugin->toggle_button);
    xfce_panel_plugin_add_action_widget(panel_plugin, plugin->toggle_button);
}

/* Called by the battery backend once it has a reading and on every
 * change after that. The badge is shown only while a battery is
 * actually present, so desktops without one never see it. The icon
 * switches to its "-charging" variant while plugged in and charging,
 * and the tooltip carries the state plus time estimate. */
static void
on_battery_changed(gboolean present, guint percent, ExtrasMenuBatteryState state,
                   gint64 seconds, gpointer user_data)
{
    ExtrasMenuPlugin *plugin = user_data;

    update_panel_battery_icon(plugin, present, percent, state);

    if (plugin->battery_label == NULL || plugin->battery_icon == NULL)
        return;

    /* The badge is the pill container holding the icon + label. */
    GtkWidget *badge = gtk_widget_get_parent(plugin->battery_label);
    if (badge == NULL)
        return;

    if (!present)
    {
        gtk_widget_hide(badge);
        return;
    }

    gchar *text = g_strdup_printf("%u%%", percent);
    gtk_label_set_text(GTK_LABEL(plugin->battery_label), text);
    g_free(text);

    gchar *icon_name = extras_menu_battery_icon_name(percent, state);
    gtk_image_set_from_icon_name(GTK_IMAGE(plugin->battery_icon), icon_name,
                                  GTK_ICON_SIZE_BUTTON);
    g_free(icon_name);

    gchar *tooltip = extras_menu_battery_describe(percent, state, seconds);
    gtk_widget_set_tooltip_text(badge, tooltip);
    g_free(tooltip);

    gtk_widget_show(badge);
}

/* Hides the battery badge until the UPower backend confirms a battery,
 * then starts the backend. Needs the dropdown content to exist. */
void
extras_menu_system_status_attach_battery(ExtrasMenuPlugin *plugin)
{
    /* --- battery badge: hidden until the UPower backend confirms a
     * battery exists. no_show_all keeps any later show_all from
     * revealing it behind the backend's back. --- */
    if (plugin->battery_label != NULL)
    {
        GtkWidget *badge = gtk_widget_get_parent(plugin->battery_label);
        gtk_widget_set_no_show_all(badge, TRUE);
        gtk_widget_hide(badge);
    }
    plugin->battery = extras_menu_battery_new(on_battery_changed, plugin);
}

/* The flat button around the volume row's icon (a click mutes), or NULL.
 * popover.c puts the icon inside it, so it is the icon's parent. */
static GtkWidget *
volume_mute_button(ExtrasMenuPlugin *plugin)
{
    if (plugin->volume_icon == NULL)
        return NULL;

    GtkWidget *parent = gtk_widget_get_parent(plugin->volume_icon);
    return (parent != NULL && GTK_IS_BUTTON(parent)) ? parent : NULL;
}

/* Called by the audio backend on connect and on every subsequent
 * volume/mute change, whether we caused it or something else did
 * (pavucontrol, media keys, ...). Updates the slider to match. */
static void
on_audio_changed(guint volume_percent, gboolean muted, gpointer user_data)
{
    ExtrasMenuPlugin *plugin = EXTRAS_MENU_PLUGIN(user_data);

    update_panel_volume_icon(plugin, volume_percent, muted);

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

    /* Muted or 0% gets the crossed-out speaker; otherwise the icon
     * scales by level (low/medium/high) -- see volume_icon_name(). */
    if (plugin->volume_icon != NULL)
    {
        gtk_image_set_from_icon_name(GTK_IMAGE(plugin->volume_icon),
                                      volume_icon_name(volume_percent, muted),
                                      GTK_ICON_SIZE_BUTTON);
    }

    /* The icon is the mute button: its tooltip says what a click does. */
    GtkWidget *mute_button = volume_mute_button(plugin);
    if (mute_button != NULL)
        gtk_widget_set_tooltip_text(mute_button, muted ? "Unmute" : "Mute");
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

/* The user clicked the volume icon: mute, or unmute if already muted.
 * The new state comes back through on_audio_changed() like any other
 * change, which is what updates the icons. */
static void
on_volume_mute_clicked(GtkButton *button, ExtrasMenuPlugin *plugin)
{
    (void) button;
    extras_menu_audio_toggle_mute(plugin->audio);
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

/* Starts the audio and brightness backends and connects the two sliders.
 * Needs the dropdown content to exist. */
void
extras_menu_system_status_attach_sliders(ExtrasMenuPlugin *plugin)
{
    /* --- audio backend: connects asynchronously, reports state (and
     * every subsequent change) through on_audio_changed --- */
    plugin->audio = extras_menu_audio_new(on_audio_changed, plugin);

    if (plugin->volume_scale != NULL)
    {
        g_signal_connect(plugin->volume_scale, "value-changed",
                          G_CALLBACK(on_volume_scale_changed), plugin);
    }

    GtkWidget *mute_button = volume_mute_button(plugin);
    if (mute_button != NULL)
    {
        gtk_widget_set_tooltip_text(mute_button, "Mute");
        g_signal_connect(mute_button, "clicked",
                          G_CALLBACK(on_volume_mute_clicked), plugin);
    }

    /* --- brightness backend: reads the current value async on start,
     * reports it (and every value we set afterwards) through
     * on_brightness_changed --- */
    plugin->brightness = extras_menu_brightness_new(on_brightness_changed, plugin);

    if (plugin->brightness_scale != NULL)
    {
        /* Never let the slider reach 0%: many panels switch the
         * backlight fully off there, leaving a black screen. */
        gtk_range_set_range(GTK_RANGE(plugin->brightness_scale),
                             EXTRAS_MENU_BRIGHTNESS_MIN_PERCENT, 100);
        g_signal_connect(plugin->brightness_scale, "value-changed",
                          G_CALLBACK(on_brightness_scale_changed), plugin);
    }
}

/* Stops the battery, audio and brightness backends. */
void
extras_menu_system_status_free(ExtrasMenuPlugin *plugin)
{
    if (plugin->battery != NULL)
    {
        extras_menu_battery_free(plugin->battery);
        plugin->battery = NULL;
    }

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
}