#include "caffeine-page.h"
#include "caffeine-config.h"

/* The ready-made choices, in the order they are listed. The "custom"
 * choice comes after them. */
static const struct
{
    const gchar *label;
    guint minutes;
} presets[] = {
    { "Until I turn it off", EXTRAS_MENU_CAFFEINE_UNLIMITED },
    { "15 minutes",          15 },
    { "30 minutes",          30 },
    { "1 hour",              60 },
    { "2 hours",             120 },
    { "4 hours",             240 },
    { "8 hours",             480 },
};

/* What the custom spin button shows when the saved duration is one of
 * the presets (so choosing "Custom" has something sensible to start
 * from). */
#define CUSTOM_DEFAULT_MINUTES 90

typedef struct
{
    GtkWidget *custom_radio;
    GtkWidget *custom_spin;
} PageState;

static void
save_duration(guint minutes)
{
    GError *error = NULL;

    if (!extras_menu_caffeine_config_set_duration(minutes, &error))
    {
        g_warning("extras-menu: couldn't save the Caffeine setting: %s",
                  error != NULL ? error->message : "unknown error");
        g_clear_error(&error);
    }
}

static void
on_preset_toggled(GtkToggleButton *button, gpointer user_data)
{
    (void) user_data;

    /* A radio group reports both the choice being left and the one
     * being picked; only the second matters. */
    if (!gtk_toggle_button_get_active(button))
        return;

    save_duration(GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(button), "minutes")));
}

static void
on_custom_toggled(GtkToggleButton *button, gpointer user_data)
{
    PageState *st = user_data;
    gboolean active = gtk_toggle_button_get_active(button);

    gtk_widget_set_sensitive(st->custom_spin, active);

    if (active)
        save_duration((guint) gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(st->custom_spin)));
}

static void
on_custom_value_changed(GtkSpinButton *spin, gpointer user_data)
{
    PageState *st = user_data;

    if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(st->custom_radio)))
        save_duration((guint) gtk_spin_button_get_value_as_int(spin));
}

GtkWidget *
extras_menu_caffeine_page_new(void)
{
    PageState *st = g_new0(PageState, 1);

    GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    g_object_set_data_full(G_OBJECT(page), "page-state", st, g_free);

    GtkWidget *heading = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(heading), "<b>Caffeine</b>");
    gtk_label_set_xalign(GTK_LABEL(heading), 0.0);
    gtk_box_pack_start(GTK_BOX(page), heading, FALSE, FALSE, 0);

    GtkWidget *description = gtk_label_new(
        "While the Caffeine button in the menu is on, the screen stays on "
        "and the computer doesn't go to sleep by itself. Choose how long it "
        "stays on before it switches itself off again. Changes apply "
        "immediately, also to Caffeine if it is on right now.");
    gtk_label_set_line_wrap(GTK_LABEL(description), TRUE);
    gtk_label_set_xalign(GTK_LABEL(description), 0.0);
    gtk_label_set_max_width_chars(GTK_LABEL(description), 60);
    gtk_style_context_add_class(gtk_widget_get_style_context(description), "dim-label");
    gtk_box_pack_start(GTK_BOX(page), description, FALSE, FALSE, 0);

    guint saved = extras_menu_caffeine_config_get_duration();
    gboolean saved_is_preset = FALSE;

    GtkWidget *first_radio = NULL;
    GtkWidget *radios[G_N_ELEMENTS(presets)];

    for (guint i = 0; i < G_N_ELEMENTS(presets); i++)
    {
        GtkWidget *radio = (first_radio == NULL)
            ? gtk_radio_button_new_with_label(NULL, presets[i].label)
            : gtk_radio_button_new_with_label_from_widget(GTK_RADIO_BUTTON(first_radio),
                                                          presets[i].label);
        if (first_radio == NULL)
            first_radio = radio;

        g_object_set_data(G_OBJECT(radio), "minutes", GUINT_TO_POINTER(presets[i].minutes));
        radios[i] = radio;

        if (presets[i].minutes == saved)
            saved_is_preset = TRUE;

        gtk_box_pack_start(GTK_BOX(page), radio, FALSE, FALSE, 0);
    }

    /* The custom choice: a radio button with a number of minutes next to it. */
    GtkWidget *custom_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);

    st->custom_radio = gtk_radio_button_new_with_label_from_widget(GTK_RADIO_BUTTON(first_radio),
                                                                   "Custom:");
    st->custom_spin = gtk_spin_button_new_with_range(1, EXTRAS_MENU_CAFFEINE_MAX_MINUTES, 1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(st->custom_spin),
                              saved_is_preset ? CUSTOM_DEFAULT_MINUTES : saved);

    gtk_box_pack_start(GTK_BOX(custom_row), st->custom_radio, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(custom_row), st->custom_spin, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(custom_row), gtk_label_new("minutes"), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(page), custom_row, FALSE, FALSE, 0);

    /* Show the saved choice before any handler is connected, so that
     * building the page doesn't write anything back. */
    if (saved_is_preset)
    {
        for (guint i = 0; i < G_N_ELEMENTS(presets); i++)
        {
            if (presets[i].minutes == saved)
                gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(radios[i]), TRUE);
        }
    }
    else
    {
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(st->custom_radio), TRUE);
    }
    gtk_widget_set_sensitive(st->custom_spin, !saved_is_preset);

    for (guint i = 0; i < G_N_ELEMENTS(presets); i++)
        g_signal_connect(radios[i], "toggled", G_CALLBACK(on_preset_toggled), NULL);

    g_signal_connect(st->custom_radio, "toggled", G_CALLBACK(on_custom_toggled), st);
    g_signal_connect(st->custom_spin, "value-changed", G_CALLBACK(on_custom_value_changed), st);

    return page;
}
