/*
 * Preferences dialog for the Caffeine plugin.
 *
 * Kept in its own file on purpose: caffeine.c handles the actual
 * inhibit/uninhibit + icon drawing and should stay focused on that.
 * Everything about settings storage (xfconf) and the properties UI
 * lives here instead.
 *
 * English-only UI strings for now, more preferences to come later.
 */

#include <xfconf/xfconf.h>

#include "caffeine-prefs.h"

#define XFCONF_PROP_LOCK_CYCLE_MODE      "/lock-cycle-mode"     /* int, CaffeineLockCycleMode */
#define XFCONF_PROP_LOCK_CYCLE_CUSTOM_MIN "/lock-cycle-custom-minutes" /* uint, >= 1 */

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
} PrefsWidgets;

static void
on_custom_radio_toggled (GtkToggleButton *radio, gpointer user_data)
{
    PrefsWidgets *w = (PrefsWidgets *) user_data;
    gtk_widget_set_sensitive (w->spin_custom, gtk_toggle_button_get_active (radio));
}

gboolean
caffeine_show_preferences (XfcePanelPlugin *plugin, CaffeineSettings *settings,
                            const gchar *channel_name)
{
    GtkWidget    *dialog;
    GtkWidget    *content_area;
    GtkWidget    *vbox;
    GtkWidget    *label;
    GtkWidget    *custom_hbox;
    PrefsWidgets  w = { 0 };
    gboolean      accepted = FALSE;
    gint          response;

    dialog = gtk_dialog_new_with_buttons ("Caffeine Preferences",
                                           GTK_WINDOW (gtk_widget_get_toplevel (GTK_WIDGET (plugin))),
                                           GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
                                           "_Cancel", GTK_RESPONSE_CANCEL,
                                           "_OK", GTK_RESPONSE_OK,
                                           NULL);
    gtk_window_set_resizable (GTK_WINDOW (dialog), FALSE);
    gtk_container_set_border_width (GTK_CONTAINER (dialog), 6);

    content_area = gtk_dialog_get_content_area (GTK_DIALOG (dialog));

    vbox = gtk_box_new (GTK_ORIENTATION_VERTICAL, 6);
    gtk_container_set_border_width (GTK_CONTAINER (vbox), 8);
    gtk_container_add (GTK_CONTAINER (content_area), vbox);

    label = gtk_label_new ("While Caffeine is on, automatically lock the\nscreen and blank the monitor every:");
    gtk_label_set_xalign (GTK_LABEL (label), 0.0);
    gtk_box_pack_start (GTK_BOX (vbox), label, FALSE, FALSE, 0);

    w.radio_never = gtk_radio_button_new_with_label (NULL, "Never (stay awake indefinitely)");
    gtk_box_pack_start (GTK_BOX (vbox), w.radio_never, FALSE, FALSE, 0);

    w.radio_15 = gtk_radio_button_new_with_label_from_widget (
        GTK_RADIO_BUTTON (w.radio_never), "15 minutes");
    gtk_box_pack_start (GTK_BOX (vbox), w.radio_15, FALSE, FALSE, 0);

    w.radio_30 = gtk_radio_button_new_with_label_from_widget (
        GTK_RADIO_BUTTON (w.radio_never), "30 minutes");
    gtk_box_pack_start (GTK_BOX (vbox), w.radio_30, FALSE, FALSE, 0);

    w.radio_60 = gtk_radio_button_new_with_label_from_widget (
        GTK_RADIO_BUTTON (w.radio_never), "60 minutes");
    gtk_box_pack_start (GTK_BOX (vbox), w.radio_60, FALSE, FALSE, 0);

    w.radio_custom = gtk_radio_button_new_with_label_from_widget (
        GTK_RADIO_BUTTON (w.radio_never), "Custom:");
    gtk_box_pack_start (GTK_BOX (vbox), w.radio_custom, FALSE, FALSE, 0);

    custom_hbox = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_box_pack_start (GTK_BOX (vbox), custom_hbox, FALSE, FALSE, 0);
    gtk_widget_set_margin_start (custom_hbox, 24);

    w.spin_custom = gtk_spin_button_new_with_range (1, 999, 1);
    gtk_box_pack_start (GTK_BOX (custom_hbox), w.spin_custom, FALSE, FALSE, 0);
    gtk_box_pack_start (GTK_BOX (custom_hbox), gtk_label_new ("minutes"), FALSE, FALSE, 0);

    g_signal_connect (w.radio_custom, "toggled", G_CALLBACK (on_custom_radio_toggled), &w);

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

    gtk_widget_show_all (dialog);

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

        caffeine_settings_save (settings, channel_name);
        accepted = TRUE;
    }

    gtk_widget_destroy (dialog);

    return accepted;
}