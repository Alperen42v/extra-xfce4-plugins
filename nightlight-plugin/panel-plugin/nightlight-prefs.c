/*
 * nightlight-prefs.c - Preferences dialog for the Night Light plugin
 *
 * Uses NightlightPlugin from nightlight.h and calls back into
 * strength_to_kelvin / start_transition / save_config / scheduler_tick.
 */

#include "nightlight.h"

typedef struct
{
    NightlightPlugin *nl;
    GtkWidget *chk_schedule;
    GtkWidget *scale_strength;
    GtkWidget *scale_transition;
    GtkWidget *radio_24h;
    GtkWidget *radio_12h;
    GtkWidget *spin_start_h;
    GtkWidget *spin_start_m;
    GtkWidget *combo_start_ampm;
    GtkWidget *spin_end_h;
    GtkWidget *spin_end_m;
    GtkWidget *combo_end_ampm;
} SettingsWidgets;

static void
update_hour_spin_ranges (SettingsWidgets *w)
{
    gboolean h24 = gtk_toggle_button_get_active (GTK_TOGGLE_BUTTON (w->radio_24h));

    gtk_spin_button_set_range (GTK_SPIN_BUTTON (w->spin_start_h), h24 ? 0 : 1, h24 ? 23 : 12);
    gtk_spin_button_set_range (GTK_SPIN_BUTTON (w->spin_end_h), h24 ? 0 : 1, h24 ? 23 : 12);

    /* The AM/PM combo is irrelevant in 24h mode: instead of hiding it we grey
     * it out, since gtk_widget_set_visible() would get reset by the later
     * gtk_widget_show_all() call. */
    gtk_widget_set_sensitive (w->combo_start_ampm, !h24);
    gtk_widget_set_sensitive (w->combo_end_ampm, !h24);
}

static void
h24_to_display (gint hour24, gboolean use_24h, gint *display_hour, gint *ampm /* 0=AM,1=PM */)
{
    if (use_24h)
    {
        *display_hour = hour24;
        *ampm = 0;
        return;
    }
    *ampm = (hour24 >= 12) ? 1 : 0;
    gint h12 = hour24 % 12;
    if (h12 == 0) h12 = 12;
    *display_hour = h12;
}

static gint
display_to_h24 (gint display_hour, gboolean use_24h, gint ampm)
{
    if (use_24h)
        return CLAMP (display_hour, 0, 23);

    gint h = display_hour % 12;
    if (ampm == 1) /* PM */
        h += 12;
    return h;
}

static void
on_format_toggled (GtkToggleButton *btn, gpointer data)
{
    SettingsWidgets *w = data;
    NightlightPlugin *nl = w->nl;
    gboolean use_24h = gtk_toggle_button_get_active (GTK_TOGGLE_BUTTON (w->radio_24h));
    gint dh, ampm;

    (void) btn;
    update_hour_spin_ranges (w);

    h24_to_display (nl->start_hour, use_24h, &dh, &ampm);
    gtk_spin_button_set_value (GTK_SPIN_BUTTON (w->spin_start_h), dh);
    gtk_combo_box_set_active (GTK_COMBO_BOX (w->combo_start_ampm), ampm);

    h24_to_display (nl->end_hour, use_24h, &dh, &ampm);
    gtk_spin_button_set_value (GTK_SPIN_BUTTON (w->spin_end_h), dh);
    gtk_combo_box_set_active (GTK_COMBO_BOX (w->combo_end_ampm), ampm);
}

static void
apply_settings_from_widgets (SettingsWidgets *w)
{
    NightlightPlugin *nl = w->nl;
    gboolean use_24h = gtk_toggle_button_get_active (GTK_TOGGLE_BUTTON (w->radio_24h));

    nl->schedule_enabled = gtk_toggle_button_get_active (GTK_TOGGLE_BUTTON (w->chk_schedule));
    nl->strength = (gint) gtk_range_get_value (GTK_RANGE (w->scale_strength));
    nl->transition_ms = (gint) gtk_range_get_value (GTK_RANGE (w->scale_transition));
    nl->use_24h = use_24h;

    nl->start_hour = display_to_h24 (gtk_spin_button_get_value_as_int (GTK_SPIN_BUTTON (w->spin_start_h)),
                                      use_24h,
                                      gtk_combo_box_get_active (GTK_COMBO_BOX (w->combo_start_ampm)));
    nl->start_minute = gtk_spin_button_get_value_as_int (GTK_SPIN_BUTTON (w->spin_start_m));

    nl->end_hour = display_to_h24 (gtk_spin_button_get_value_as_int (GTK_SPIN_BUTTON (w->spin_end_h)),
                                    use_24h,
                                    gtk_combo_box_get_active (GTK_COMBO_BOX (w->combo_end_ampm)));
    nl->end_minute = gtk_spin_button_get_value_as_int (GTK_SPIN_BUTTON (w->spin_end_m));

    /* clear the manual override so the scheduler takes back control */
    nl->manual_override = FALSE;

    save_config (nl);

    /* live-update the strength if currently active */
    if (nl->is_active)
        start_transition (nl, strength_to_kelvin (nl->strength));

    scheduler_tick (nl);
}

static void
on_response (GtkDialog *dialog, gint response, gpointer data)
{
    SettingsWidgets *w = data;

    if (response == GTK_RESPONSE_OK || response == GTK_RESPONSE_APPLY)
        apply_settings_from_widgets (w);

    if (response != GTK_RESPONSE_APPLY)
    {
        w->nl->dialog = NULL;
        gtk_widget_destroy (GTK_WIDGET (dialog));
        g_free (w);
    }
}

static GtkWidget *
labeled_row (GtkWidget *grid, gint row, const gchar *label_text, GtkWidget *widget)
{
    GtkWidget *label = gtk_label_new (label_text);
    gtk_widget_set_halign (label, GTK_ALIGN_START);
    gtk_widget_set_hexpand (widget, TRUE);
    gtk_widget_set_size_request (widget, 180, -1);
    gtk_grid_attach (GTK_GRID (grid), label, 0, row, 1, 1);
    gtk_grid_attach (GTK_GRID (grid), widget, 1, row, 2, 1);
    return widget;
}

void
nightlight_show_settings (XfcePanelPlugin *plugin, NightlightPlugin *nl)
{
    GtkWidget *dialog, *content, *grid, *box;
    SettingsWidgets *w;
    gint dh, ampm;

    (void) plugin;

    if (nl->dialog != NULL)
    {
        gtk_window_present (GTK_WINDOW (nl->dialog));
        return;
    }

    w = g_new0 (SettingsWidgets, 1);
    w->nl = nl;

    dialog = gtk_dialog_new_with_buttons ("Night Light Settings",
                                           GTK_WINDOW (gtk_widget_get_toplevel (GTK_WIDGET (nl->button))),
                                           GTK_DIALOG_DESTROY_WITH_PARENT,
                                           "_Close", GTK_RESPONSE_CLOSE,
                                           "_Apply", GTK_RESPONSE_APPLY,
                                           "_OK", GTK_RESPONSE_OK,
                                           NULL);
    gtk_window_set_default_size (GTK_WINDOW (dialog), 380, -1);
    nl->dialog = dialog;

    content = gtk_dialog_get_content_area (GTK_DIALOG (dialog));
    box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 10);
    gtk_container_set_border_width (GTK_CONTAINER (box), 10);
    gtk_container_add (GTK_CONTAINER (content), box);

    /* --- Strength --- */
    grid = gtk_grid_new ();
    gtk_grid_set_row_spacing (GTK_GRID (grid), 8);
    gtk_grid_set_column_spacing (GTK_GRID (grid), 8);
    gtk_box_pack_start (GTK_BOX (box), grid, FALSE, FALSE, 0);

    w->scale_strength = gtk_scale_new_with_range (GTK_ORIENTATION_HORIZONTAL, 0, 100, 1);
    gtk_scale_set_value_pos (GTK_SCALE (w->scale_strength), GTK_POS_RIGHT);
    gtk_range_set_value (GTK_RANGE (w->scale_strength), nl->strength);
    labeled_row (grid, 0, "Warm light strength:", w->scale_strength);

    w->scale_transition = gtk_scale_new_with_range (GTK_ORIENTATION_HORIZONTAL, 0, 8000, 100);
    gtk_scale_set_value_pos (GTK_SCALE (w->scale_transition), GTK_POS_RIGHT);
    gtk_range_set_value (GTK_RANGE (w->scale_transition), nl->transition_ms);
    labeled_row (grid, 1, "Transition duration (ms):", w->scale_transition);

    gtk_box_pack_start (GTK_BOX (box), gtk_separator_new (GTK_ORIENTATION_HORIZONTAL), FALSE, FALSE, 4);

    /* --- Schedule --- */
    w->chk_schedule = gtk_check_button_new_with_label ("Use automatic schedule");
    gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (w->chk_schedule), nl->schedule_enabled);
    gtk_box_pack_start (GTK_BOX (box), w->chk_schedule, FALSE, FALSE, 0);

    GtkWidget *fmt_box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
    w->radio_24h = gtk_radio_button_new_with_label (NULL, "24-hour");
    w->radio_12h = gtk_radio_button_new_with_label_from_widget (GTK_RADIO_BUTTON (w->radio_24h), "12-hour (AM/PM)");
    gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (nl->use_24h ? w->radio_24h : w->radio_12h), TRUE);
    gtk_box_pack_start (GTK_BOX (fmt_box), w->radio_24h, FALSE, FALSE, 0);
    gtk_box_pack_start (GTK_BOX (fmt_box), w->radio_12h, FALSE, FALSE, 0);
    gtk_box_pack_start (GTK_BOX (box), fmt_box, FALSE, FALSE, 0);

    GtkWidget *sched_grid = gtk_grid_new ();
    gtk_grid_set_row_spacing (GTK_GRID (sched_grid), 6);
    gtk_grid_set_column_spacing (GTK_GRID (sched_grid), 6);
    gtk_box_pack_start (GTK_BOX (box), sched_grid, FALSE, FALSE, 0);

    /* Start time */
    gtk_grid_attach (GTK_GRID (sched_grid), gtk_label_new ("Start:"), 0, 0, 1, 1);
    w->spin_start_h = gtk_spin_button_new_with_range (0, 23, 1);
    w->spin_start_m = gtk_spin_button_new_with_range (0, 59, 1);
    gtk_spin_button_set_wrap (GTK_SPIN_BUTTON (w->spin_start_h), TRUE);
    gtk_spin_button_set_wrap (GTK_SPIN_BUTTON (w->spin_start_m), TRUE);
    w->combo_start_ampm = gtk_combo_box_text_new ();
    gtk_combo_box_text_append_text (GTK_COMBO_BOX_TEXT (w->combo_start_ampm), "AM");
    gtk_combo_box_text_append_text (GTK_COMBO_BOX_TEXT (w->combo_start_ampm), "PM");
    gtk_grid_attach (GTK_GRID (sched_grid), w->spin_start_h, 1, 0, 1, 1);
    gtk_grid_attach (GTK_GRID (sched_grid), gtk_label_new (":"), 2, 0, 1, 1);
    gtk_grid_attach (GTK_GRID (sched_grid), w->spin_start_m, 3, 0, 1, 1);
    gtk_grid_attach (GTK_GRID (sched_grid), w->combo_start_ampm, 4, 0, 1, 1);

    /* End time */
    gtk_grid_attach (GTK_GRID (sched_grid), gtk_label_new ("End:"), 0, 1, 1, 1);
    w->spin_end_h = gtk_spin_button_new_with_range (0, 23, 1);
    w->spin_end_m = gtk_spin_button_new_with_range (0, 59, 1);
    gtk_spin_button_set_wrap (GTK_SPIN_BUTTON (w->spin_end_h), TRUE);
    gtk_spin_button_set_wrap (GTK_SPIN_BUTTON (w->spin_end_m), TRUE);
    w->combo_end_ampm = gtk_combo_box_text_new ();
    gtk_combo_box_text_append_text (GTK_COMBO_BOX_TEXT (w->combo_end_ampm), "AM");
    gtk_combo_box_text_append_text (GTK_COMBO_BOX_TEXT (w->combo_end_ampm), "PM");
    gtk_grid_attach (GTK_GRID (sched_grid), w->spin_end_h, 1, 1, 1, 1);
    gtk_grid_attach (GTK_GRID (sched_grid), gtk_label_new (":"), 2, 1, 1, 1);
    gtk_grid_attach (GTK_GRID (sched_grid), w->spin_end_m, 3, 1, 1, 1);
    gtk_grid_attach (GTK_GRID (sched_grid), w->combo_end_ampm, 4, 1, 1, 1);

    /* fill in current values */
    h24_to_display (nl->start_hour, nl->use_24h, &dh, &ampm);
    gtk_spin_button_set_value (GTK_SPIN_BUTTON (w->spin_start_h), dh);
    gtk_spin_button_set_value (GTK_SPIN_BUTTON (w->spin_start_m), nl->start_minute);
    gtk_combo_box_set_active (GTK_COMBO_BOX (w->combo_start_ampm), ampm);

    h24_to_display (nl->end_hour, nl->use_24h, &dh, &ampm);
    gtk_spin_button_set_value (GTK_SPIN_BUTTON (w->spin_end_h), dh);
    gtk_spin_button_set_value (GTK_SPIN_BUTTON (w->spin_end_m), nl->end_minute);
    gtk_combo_box_set_active (GTK_COMBO_BOX (w->combo_end_ampm), ampm);

    update_hour_spin_ranges (w);

    g_signal_connect (w->radio_24h, "toggled", G_CALLBACK (on_format_toggled), w);

    g_signal_connect (dialog, "response", G_CALLBACK (on_response), w);
    gtk_widget_show_all (dialog);
}