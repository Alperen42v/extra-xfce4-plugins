/*
 * Preferences dialog for the Caffeine plugin.
 * Settings storage (xfconf) and the properties UI live here, separate
 * from caffeine.c's inhibit/drawing logic.
 */

#include <xfconf/xfconf.h>

#include "caffeine-prefs.h"

#define XFCONF_PROP_LOCK_CYCLE_MODE      "/lock-cycle-mode"     /* int, CaffeineLockCycleMode */
#define XFCONF_PROP_LOCK_CYCLE_CUSTOM_MIN "/lock-cycle-custom-minutes" /* uint, >= 1 */
#define XFCONF_PROP_ICON_SOURCE          "/icon-source"         /* int, CaffeineIconSource */
#define XFCONF_PROP_ICON_THEME           "/icon-theme"          /* int, CaffeineIconTheme */
#define XFCONF_PROP_SCREEN_OFF_ENABLED   "/screen-off-enabled"  /* bool */
#define XFCONF_PROP_SCREEN_OFF_MODE      "/screen-off-mode"     /* int, CaffeineScreenOffMode */
#define XFCONF_PROP_SCREEN_OFF_CUSTOM_MIN "/screen-off-custom-minutes" /* uint, >= 1 */

/* ---------------------------------------------------------------------- */
/* Settings resolution                                                    */
/* ---------------------------------------------------------------------- */

guint
caffeine_settings_get_interval_minutes (const CaffeineSettings *settings)
{
    if (settings == NULL)
        return 0;

    switch (settings->mode)
    {
        case CAFFEINE_LOCK_CYCLE_NEVER:
            return 0;

        case CAFFEINE_LOCK_CYCLE_CUSTOM:
            return settings->custom_minutes > 0 ? settings->custom_minutes : 0;

        case CAFFEINE_LOCK_CYCLE_15MIN:
        case CAFFEINE_LOCK_CYCLE_30MIN:
        case CAFFEINE_LOCK_CYCLE_60MIN:
        default:
            return (guint) settings->mode;
    }
}

guint
caffeine_settings_get_screen_off_interval_minutes (const CaffeineSettings *settings)
{
    if (settings == NULL || !settings->screen_off_enabled)
        return 0;

    switch (settings->screen_off_mode)
    {
        case CAFFEINE_SCREEN_OFF_CUSTOM:
            return settings->screen_off_custom_minutes > 0 ? settings->screen_off_custom_minutes : 0;

        case CAFFEINE_SCREEN_OFF_5MIN:
        case CAFFEINE_SCREEN_OFF_10MIN:
        case CAFFEINE_SCREEN_OFF_15MIN:
        case CAFFEINE_SCREEN_OFF_30MIN:
            return (guint) settings->screen_off_mode;

        case CAFFEINE_SCREEN_OFF_NEVER:
        default:
            return 0;
    }
}

/* ---------------------------------------------------------------------- */
/* xfconf load/save                                                       */
/* ---------------------------------------------------------------------- */

void
caffeine_settings_load (CaffeineSettings *settings, const gchar *channel_name)
{
    XfconfChannel *channel;
    gint           mode_val;

    /* sane defaults if xfconf isn't available or nothing was saved yet */
    settings->mode = CAFFEINE_LOCK_CYCLE_NEVER;
    settings->custom_minutes = 60;
    settings->icon_source = CAFFEINE_ICON_SOURCE_SYSTEM;
    settings->icon_theme = CAFFEINE_ICON_THEME_AUTO;
    settings->screen_off_enabled = FALSE;
    settings->screen_off_mode = CAFFEINE_SCREEN_OFF_15MIN;
    settings->screen_off_custom_minutes = 15;

    if (!xfconf_init (NULL))
    {
        g_warning ("Caffeine: xfconf_init failed, using default settings");
        return;
    }

    channel = xfconf_channel_get (channel_name);
    if (channel == NULL)
        return;

    mode_val = xfconf_channel_get_int (channel, XFCONF_PROP_LOCK_CYCLE_MODE,
                                        (gint) CAFFEINE_LOCK_CYCLE_NEVER);
    settings->mode = (CaffeineLockCycleMode) mode_val;

    settings->custom_minutes = xfconf_channel_get_uint (channel, XFCONF_PROP_LOCK_CYCLE_CUSTOM_MIN, 60);
    if (settings->custom_minutes == 0)
        settings->custom_minutes = 60;

    settings->icon_source = (CaffeineIconSource) xfconf_channel_get_int (
        channel, XFCONF_PROP_ICON_SOURCE, (gint) CAFFEINE_ICON_SOURCE_SYSTEM);
    if (settings->icon_source != CAFFEINE_ICON_SOURCE_PLUGIN)
        settings->icon_source = CAFFEINE_ICON_SOURCE_SYSTEM; /* ignore unknown values */

    settings->icon_theme = (CaffeineIconTheme) xfconf_channel_get_int (
        channel, XFCONF_PROP_ICON_THEME, (gint) CAFFEINE_ICON_THEME_AUTO);

    settings->screen_off_enabled = xfconf_channel_get_bool (channel, XFCONF_PROP_SCREEN_OFF_ENABLED, FALSE);

    settings->screen_off_mode = (CaffeineScreenOffMode) xfconf_channel_get_int (
        channel, XFCONF_PROP_SCREEN_OFF_MODE, (gint) CAFFEINE_SCREEN_OFF_15MIN);

    settings->screen_off_custom_minutes =
        xfconf_channel_get_uint (channel, XFCONF_PROP_SCREEN_OFF_CUSTOM_MIN, 15);
    if (settings->screen_off_custom_minutes == 0)
        settings->screen_off_custom_minutes = 15;
}

void
caffeine_settings_save (const CaffeineSettings *settings, const gchar *channel_name)
{
    XfconfChannel *channel;

    if (!xfconf_init (NULL))
    {
        g_warning ("Caffeine: xfconf_init failed, settings not saved");
        return;
    }

    channel = xfconf_channel_get (channel_name);
    if (channel == NULL)
        return;

    xfconf_channel_set_int (channel, XFCONF_PROP_LOCK_CYCLE_MODE, (gint) settings->mode);
    xfconf_channel_set_uint (channel, XFCONF_PROP_LOCK_CYCLE_CUSTOM_MIN, settings->custom_minutes);
    xfconf_channel_set_int (channel, XFCONF_PROP_ICON_SOURCE, (gint) settings->icon_source);
    xfconf_channel_set_int (channel, XFCONF_PROP_ICON_THEME, (gint) settings->icon_theme);
    xfconf_channel_set_bool (channel, XFCONF_PROP_SCREEN_OFF_ENABLED, settings->screen_off_enabled);
    xfconf_channel_set_int (channel, XFCONF_PROP_SCREEN_OFF_MODE, (gint) settings->screen_off_mode);
    xfconf_channel_set_uint (channel, XFCONF_PROP_SCREEN_OFF_CUSTOM_MIN, settings->screen_off_custom_minutes);
}

/* ---------------------------------------------------------------------- */
/* Preferences dialog                                                     */
/* ---------------------------------------------------------------------- */

typedef struct
{
    GtkWidget *radio_never;
    GtkWidget *radio_15;
    GtkWidget *radio_30;
    GtkWidget *radio_60;
    GtkWidget *radio_custom;
    GtkWidget *spin_custom;

    GtkWidget *radio_source_system;
    GtkWidget *radio_source_plugin;
    GtkWidget *theme_box;             /* light/dark radios, only relevant for Caffeine's own icons */

    GtkWidget *radio_theme_auto;
    GtkWidget *radio_theme_light;
    GtkWidget *radio_theme_dark;

    GtkWidget *check_screen_off_enabled;
    GtkWidget *screen_off_box;        /* container holding the radios below, sensitivity-linked to the checkbox */
    GtkWidget *radio_screen_off_5;
    GtkWidget *radio_screen_off_10;
    GtkWidget *radio_screen_off_15;
    GtkWidget *radio_screen_off_30;
    GtkWidget *radio_screen_off_custom;
    GtkWidget *spin_screen_off_custom;
} PrefsWidgets;

/* Shown on hover over the icon source radios */
#define ICON_SOURCE_SYSTEM_TOOLTIP_TEXT \
    N_("Uses the Caffeine icons of your current icon theme. If the theme " \
       "doesn't provide them, Caffeine's own icons are used instead.")
#define ICON_SOURCE_PLUGIN_TOOLTIP_TEXT \
    N_("Always uses the icons bundled with Caffeine (or your own PNGs in " \
       "~/.config/xfce4-caffeine-plugin/icons).")

/* Shown on hover over the icon theme radios */
#define ICON_THEME_TOOLTIP_TEXT \
    N_("It's recommended to pick the theme that matches your system.")

static void
on_custom_radio_toggled (GtkToggleButton *radio, gpointer user_data)
{
    PrefsWidgets *w = (PrefsWidgets *) user_data;
    gtk_widget_set_sensitive (w->spin_custom, gtk_toggle_button_get_active (radio));
}

static void
on_source_plugin_toggled (GtkToggleButton *radio, gpointer user_data)
{
    PrefsWidgets *w = (PrefsWidgets *) user_data;
    gtk_widget_set_sensitive (w->theme_box, gtk_toggle_button_get_active (radio));
}

static void
on_screen_off_custom_radio_toggled (GtkToggleButton *radio, gpointer user_data)
{
    PrefsWidgets *w = (PrefsWidgets *) user_data;
    gtk_widget_set_sensitive (w->spin_screen_off_custom, gtk_toggle_button_get_active (radio));
}

static void
on_screen_off_enabled_toggled (GtkToggleButton *check, gpointer user_data)
{
    PrefsWidgets *w = (PrefsWidgets *) user_data;
    gboolean      enabled = gtk_toggle_button_get_active (check);

    gtk_widget_set_sensitive (w->screen_off_box, enabled);
}

/* Tallest the scrollable content area may grow before it starts to
 * scroll: 70% of the primary monitor's work area, so the whole dialog
 * (title bar and buttons included) always fits on screen. */
static gint
get_max_content_height (void)
{
    GdkDisplay  *display = gdk_display_get_default ();
    GdkMonitor  *monitor = NULL;
    GdkRectangle workarea;

    if (display != NULL)
    {
        monitor = gdk_display_get_primary_monitor (display);
        if (monitor == NULL && gdk_display_get_n_monitors (display) > 0)
            monitor = gdk_display_get_monitor (display, 0);
    }

    if (monitor == NULL)
        return 500; /* sane fallback if no monitor info is available */

    gdk_monitor_get_workarea (monitor, &workarea);
    return MAX (300, (workarea.height * 7) / 10);
}

gboolean
caffeine_show_preferences (XfcePanelPlugin *plugin, CaffeineSettings *settings,
                            const gchar *channel_name)
{
    GtkWidget    *dialog;
    GtkWidget    *content_area;
    GtkWidget    *vbox;
    GtkWidget    *scrolled;
    GtkWidget    *label;
    GtkWidget    *custom_hbox;
    PrefsWidgets  w = { 0 };
    gboolean      accepted = FALSE;
    gint          response;

    dialog = gtk_dialog_new_with_buttons (_("Caffeine Preferences"),
                                           GTK_WINDOW (gtk_widget_get_toplevel (GTK_WIDGET (plugin))),
                                           GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
                                           _("_Cancel"), GTK_RESPONSE_CANCEL,
                                           _("_OK"), GTK_RESPONSE_OK,
                                           NULL);
    gtk_window_set_resizable (GTK_WINDOW (dialog), TRUE);
    gtk_container_set_border_width (GTK_CONTAINER (dialog), 6);

    content_area = gtk_dialog_get_content_area (GTK_DIALOG (dialog));

    vbox = gtk_box_new (GTK_ORIENTATION_VERTICAL, 6);
    gtk_container_set_border_width (GTK_CONTAINER (vbox), 8);

    /* all options live in a scrolled window: the dialog grows to fit its
     * content, but stops at get_max_content_height() and scrolls instead */
    scrolled = gtk_scrolled_window_new (NULL, NULL);
    gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scrolled),
                                     GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_shadow_type (GTK_SCROLLED_WINDOW (scrolled), GTK_SHADOW_NONE);
    gtk_scrolled_window_set_propagate_natural_width (GTK_SCROLLED_WINDOW (scrolled), TRUE);
    gtk_scrolled_window_set_propagate_natural_height (GTK_SCROLLED_WINDOW (scrolled), TRUE);
    gtk_scrolled_window_set_max_content_height (GTK_SCROLLED_WINDOW (scrolled),
                                                 get_max_content_height ());
    gtk_container_add (GTK_CONTAINER (scrolled), vbox);
    gtk_box_pack_start (GTK_BOX (content_area), scrolled, TRUE, TRUE, 0);

    label = gtk_label_new (_("While Caffeine is on, automatically lock the\nscreen and blank the monitor every:"));
    gtk_label_set_xalign (GTK_LABEL (label), 0.0);
    gtk_box_pack_start (GTK_BOX (vbox), label, FALSE, FALSE, 0);

    w.radio_never = gtk_radio_button_new_with_label (NULL, _("Never (stay awake indefinitely)"));
    gtk_box_pack_start (GTK_BOX (vbox), w.radio_never, FALSE, FALSE, 0);

    w.radio_15 = gtk_radio_button_new_with_label_from_widget (
        GTK_RADIO_BUTTON (w.radio_never), _("15 minutes"));
    gtk_box_pack_start (GTK_BOX (vbox), w.radio_15, FALSE, FALSE, 0);

    w.radio_30 = gtk_radio_button_new_with_label_from_widget (
        GTK_RADIO_BUTTON (w.radio_never), _("30 minutes"));
    gtk_box_pack_start (GTK_BOX (vbox), w.radio_30, FALSE, FALSE, 0);

    w.radio_60 = gtk_radio_button_new_with_label_from_widget (
        GTK_RADIO_BUTTON (w.radio_never), _("60 minutes"));
    gtk_box_pack_start (GTK_BOX (vbox), w.radio_60, FALSE, FALSE, 0);

    w.radio_custom = gtk_radio_button_new_with_label_from_widget (
        GTK_RADIO_BUTTON (w.radio_never), _("Custom:"));
    gtk_box_pack_start (GTK_BOX (vbox), w.radio_custom, FALSE, FALSE, 0);

    custom_hbox = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_box_pack_start (GTK_BOX (vbox), custom_hbox, FALSE, FALSE, 0);
    gtk_widget_set_margin_start (custom_hbox, 24);

    w.spin_custom = gtk_spin_button_new_with_range (1, 999, 1);
    gtk_box_pack_start (GTK_BOX (custom_hbox), w.spin_custom, FALSE, FALSE, 0);
    gtk_box_pack_start (GTK_BOX (custom_hbox), gtk_label_new (_("minutes")), FALSE, FALSE, 0);

    g_signal_connect (w.radio_custom, "toggled", G_CALLBACK (on_custom_radio_toggled), &w);

    /* separator + icon theme section */
    gtk_box_pack_start (GTK_BOX (vbox), gtk_separator_new (GTK_ORIENTATION_HORIZONTAL), FALSE, FALSE, 4);

    label = gtk_label_new (_("Panel icons:"));
    gtk_label_set_xalign (GTK_LABEL (label), 0.0);
    gtk_box_pack_start (GTK_BOX (vbox), label, FALSE, FALSE, 0);

    w.radio_source_system = gtk_radio_button_new_with_label (NULL, _("Follow system icon theme (default)"));
    gtk_widget_set_tooltip_text (w.radio_source_system, _(ICON_SOURCE_SYSTEM_TOOLTIP_TEXT));
    gtk_box_pack_start (GTK_BOX (vbox), w.radio_source_system, FALSE, FALSE, 0);

    w.radio_source_plugin = gtk_radio_button_new_with_label_from_widget (
        GTK_RADIO_BUTTON (w.radio_source_system), _("Caffeine's own icons"));
    gtk_widget_set_tooltip_text (w.radio_source_plugin, _(ICON_SOURCE_PLUGIN_TOOLTIP_TEXT));
    gtk_box_pack_start (GTK_BOX (vbox), w.radio_source_plugin, FALSE, FALSE, 0);

    /* light/dark variant choice - only applies to Caffeine's own icons */
    w.theme_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 4);
    gtk_widget_set_margin_start (w.theme_box, 24);
    gtk_box_pack_start (GTK_BOX (vbox), w.theme_box, FALSE, FALSE, 0);

    label = gtk_label_new (_("Icon variant:"));
    gtk_label_set_xalign (GTK_LABEL (label), 0.0);
    gtk_box_pack_start (GTK_BOX (w.theme_box), label, FALSE, FALSE, 0);

    w.radio_theme_auto = gtk_radio_button_new_with_label (NULL, _("Auto (match system theme)"));
    gtk_widget_set_tooltip_text (w.radio_theme_auto, _(ICON_THEME_TOOLTIP_TEXT));
    gtk_box_pack_start (GTK_BOX (w.theme_box), w.radio_theme_auto, FALSE, FALSE, 0);

    w.radio_theme_light = gtk_radio_button_new_with_label_from_widget (
        GTK_RADIO_BUTTON (w.radio_theme_auto), _("Light system theme"));
    gtk_widget_set_tooltip_text (w.radio_theme_light, _(ICON_THEME_TOOLTIP_TEXT));
    gtk_box_pack_start (GTK_BOX (w.theme_box), w.radio_theme_light, FALSE, FALSE, 0);

    w.radio_theme_dark = gtk_radio_button_new_with_label_from_widget (
        GTK_RADIO_BUTTON (w.radio_theme_auto), _("Dark system theme"));
    gtk_widget_set_tooltip_text (w.radio_theme_dark, _(ICON_THEME_TOOLTIP_TEXT));
    gtk_box_pack_start (GTK_BOX (w.theme_box), w.radio_theme_dark, FALSE, FALSE, 0);

    g_signal_connect (w.radio_source_plugin, "toggled", G_CALLBACK (on_source_plugin_toggled), &w);

    /* separator + screen-off-only section */
    gtk_box_pack_start (GTK_BOX (vbox), gtk_separator_new (GTK_ORIENTATION_HORIZONTAL), FALSE, FALSE, 4);

    w.check_screen_off_enabled = gtk_check_button_new_with_label (
        _("Turn off screen after (does not lock the session)"));
    gtk_box_pack_start (GTK_BOX (vbox), w.check_screen_off_enabled, FALSE, FALSE, 0);

    w.screen_off_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 4);
    gtk_widget_set_margin_start (w.screen_off_box, 24);
    gtk_box_pack_start (GTK_BOX (vbox), w.screen_off_box, FALSE, FALSE, 0);

    w.radio_screen_off_5 = gtk_radio_button_new_with_label (NULL, _("5 minutes"));
    gtk_box_pack_start (GTK_BOX (w.screen_off_box), w.radio_screen_off_5, FALSE, FALSE, 0);

    w.radio_screen_off_10 = gtk_radio_button_new_with_label_from_widget (
        GTK_RADIO_BUTTON (w.radio_screen_off_5), _("10 minutes"));
    gtk_box_pack_start (GTK_BOX (w.screen_off_box), w.radio_screen_off_10, FALSE, FALSE, 0);

    w.radio_screen_off_15 = gtk_radio_button_new_with_label_from_widget (
        GTK_RADIO_BUTTON (w.radio_screen_off_5), _("15 minutes"));
    gtk_box_pack_start (GTK_BOX (w.screen_off_box), w.radio_screen_off_15, FALSE, FALSE, 0);

    w.radio_screen_off_30 = gtk_radio_button_new_with_label_from_widget (
        GTK_RADIO_BUTTON (w.radio_screen_off_5), _("30 minutes"));
    gtk_box_pack_start (GTK_BOX (w.screen_off_box), w.radio_screen_off_30, FALSE, FALSE, 0);

    w.radio_screen_off_custom = gtk_radio_button_new_with_label_from_widget (
        GTK_RADIO_BUTTON (w.radio_screen_off_5), _("Custom:"));
    gtk_box_pack_start (GTK_BOX (w.screen_off_box), w.radio_screen_off_custom, FALSE, FALSE, 0);

    {
        GtkWidget *screen_off_custom_hbox = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
        gtk_box_pack_start (GTK_BOX (w.screen_off_box), screen_off_custom_hbox, FALSE, FALSE, 0);
        gtk_widget_set_margin_start (screen_off_custom_hbox, 24);

        w.spin_screen_off_custom = gtk_spin_button_new_with_range (1, 999, 1);
        gtk_box_pack_start (GTK_BOX (screen_off_custom_hbox), w.spin_screen_off_custom, FALSE, FALSE, 0);
        gtk_box_pack_start (GTK_BOX (screen_off_custom_hbox), gtk_label_new (_("minutes")), FALSE, FALSE, 0);
    }

    g_signal_connect (w.radio_screen_off_custom, "toggled",
                       G_CALLBACK (on_screen_off_custom_radio_toggled), &w);
    g_signal_connect (w.check_screen_off_enabled, "toggled",
                       G_CALLBACK (on_screen_off_enabled_toggled), &w);

    /* reflect current settings in the UI */
    switch (settings->mode)
    {
        case CAFFEINE_LOCK_CYCLE_15MIN:
            gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (w.radio_15), TRUE);
            break;
        case CAFFEINE_LOCK_CYCLE_30MIN:
            gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (w.radio_30), TRUE);
            break;
        case CAFFEINE_LOCK_CYCLE_60MIN:
            gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (w.radio_60), TRUE);
            break;
        case CAFFEINE_LOCK_CYCLE_CUSTOM:
            gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (w.radio_custom), TRUE);
            break;
        case CAFFEINE_LOCK_CYCLE_NEVER:
        default:
            gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (w.radio_never), TRUE);
            break;
    }
    gtk_spin_button_set_value (GTK_SPIN_BUTTON (w.spin_custom), settings->custom_minutes);
    gtk_widget_set_sensitive (w.spin_custom,
        gtk_toggle_button_get_active (GTK_TOGGLE_BUTTON (w.radio_custom)));

    gtk_toggle_button_set_active (
        GTK_TOGGLE_BUTTON (settings->icon_source == CAFFEINE_ICON_SOURCE_PLUGIN
                            ? w.radio_source_plugin : w.radio_source_system),
        TRUE);
    gtk_widget_set_sensitive (w.theme_box,
        gtk_toggle_button_get_active (GTK_TOGGLE_BUTTON (w.radio_source_plugin)));

    switch (settings->icon_theme)
    {
        case CAFFEINE_ICON_THEME_LIGHT:
            gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (w.radio_theme_light), TRUE);
            break;
        case CAFFEINE_ICON_THEME_DARK:
            gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (w.radio_theme_dark), TRUE);
            break;
        case CAFFEINE_ICON_THEME_AUTO:
        default:
            gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (w.radio_theme_auto), TRUE);
            break;
    }

    gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (w.check_screen_off_enabled),
                                   settings->screen_off_enabled);

    switch (settings->screen_off_mode)
    {
        case CAFFEINE_SCREEN_OFF_5MIN:
            gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (w.radio_screen_off_5), TRUE);
            break;
        case CAFFEINE_SCREEN_OFF_10MIN:
            gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (w.radio_screen_off_10), TRUE);
            break;
        case CAFFEINE_SCREEN_OFF_30MIN:
            gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (w.radio_screen_off_30), TRUE);
            break;
        case CAFFEINE_SCREEN_OFF_CUSTOM:
            gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (w.radio_screen_off_custom), TRUE);
            break;
        case CAFFEINE_SCREEN_OFF_15MIN:
        default:
            gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (w.radio_screen_off_15), TRUE);
            break;
    }
    gtk_spin_button_set_value (GTK_SPIN_BUTTON (w.spin_screen_off_custom), settings->screen_off_custom_minutes);
    gtk_widget_set_sensitive (w.spin_screen_off_custom,
        gtk_toggle_button_get_active (GTK_TOGGLE_BUTTON (w.radio_screen_off_custom)));

    gtk_widget_show_all (dialog);

    /* set after show_all(), which only affects visibility not sensitivity */
    gtk_widget_set_sensitive (w.screen_off_box, settings->screen_off_enabled);

    response = gtk_dialog_run (GTK_DIALOG (dialog));

    if (response == GTK_RESPONSE_OK)
    {
        if (gtk_toggle_button_get_active (GTK_TOGGLE_BUTTON (w.radio_15)))
            settings->mode = CAFFEINE_LOCK_CYCLE_15MIN;
        else if (gtk_toggle_button_get_active (GTK_TOGGLE_BUTTON (w.radio_30)))
            settings->mode = CAFFEINE_LOCK_CYCLE_30MIN;
        else if (gtk_toggle_button_get_active (GTK_TOGGLE_BUTTON (w.radio_60)))
            settings->mode = CAFFEINE_LOCK_CYCLE_60MIN;
        else if (gtk_toggle_button_get_active (GTK_TOGGLE_BUTTON (w.radio_custom)))
            settings->mode = CAFFEINE_LOCK_CYCLE_CUSTOM;
        else
            settings->mode = CAFFEINE_LOCK_CYCLE_NEVER;

        settings->custom_minutes =
            (guint) gtk_spin_button_get_value_as_int (GTK_SPIN_BUTTON (w.spin_custom));

        settings->icon_source =
            gtk_toggle_button_get_active (GTK_TOGGLE_BUTTON (w.radio_source_plugin))
                ? CAFFEINE_ICON_SOURCE_PLUGIN : CAFFEINE_ICON_SOURCE_SYSTEM;

        if (gtk_toggle_button_get_active (GTK_TOGGLE_BUTTON (w.radio_theme_light)))
            settings->icon_theme = CAFFEINE_ICON_THEME_LIGHT;
        else if (gtk_toggle_button_get_active (GTK_TOGGLE_BUTTON (w.radio_theme_dark)))
            settings->icon_theme = CAFFEINE_ICON_THEME_DARK;
        else
            settings->icon_theme = CAFFEINE_ICON_THEME_AUTO;

        settings->screen_off_enabled =
            gtk_toggle_button_get_active (GTK_TOGGLE_BUTTON (w.check_screen_off_enabled));

        if (gtk_toggle_button_get_active (GTK_TOGGLE_BUTTON (w.radio_screen_off_5)))
            settings->screen_off_mode = CAFFEINE_SCREEN_OFF_5MIN;
        else if (gtk_toggle_button_get_active (GTK_TOGGLE_BUTTON (w.radio_screen_off_10)))
            settings->screen_off_mode = CAFFEINE_SCREEN_OFF_10MIN;
        else if (gtk_toggle_button_get_active (GTK_TOGGLE_BUTTON (w.radio_screen_off_30)))
            settings->screen_off_mode = CAFFEINE_SCREEN_OFF_30MIN;
        else if (gtk_toggle_button_get_active (GTK_TOGGLE_BUTTON (w.radio_screen_off_custom)))
            settings->screen_off_mode = CAFFEINE_SCREEN_OFF_CUSTOM;
        else
            settings->screen_off_mode = CAFFEINE_SCREEN_OFF_15MIN;

        settings->screen_off_custom_minutes =
            (guint) gtk_spin_button_get_value_as_int (GTK_SPIN_BUTTON (w.spin_screen_off_custom));

        caffeine_settings_save (settings, channel_name);
        accepted = TRUE;
    }

    gtk_widget_destroy (dialog);

    return accepted;
}