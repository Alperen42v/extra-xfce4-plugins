#include "night-light-page.h"
#include "night-light-config.h"

/*
 * Two sliders (color temperature, transition time) and a reset button.
 *
 * Dragging a slider changes its value many times a second, and every
 * save is a file write plus a screen transition in the dropdown, so
 * saving waits until the slider has been still for SAVE_DELAY_MS. If the
 * page goes away before that, the pending change is saved first.
 */

#define SAVE_DELAY_MS 250

typedef struct
{
    GtkWidget *temperature_scale;
    GtkWidget *transition_scale;

    /* The values the sliders hold, kept here so a last save doesn't
     * have to read widgets that are being torn down. */
    guint temperature;
    guint transition_ms;

    guint save_timeout; /* GSource id, 0 = nothing waiting to be saved */
} PageState;

/* --- saving ------------------------------------------------------------------------- */

static void
save_now(PageState *st)
{
    GError *error = NULL;

    if (!extras_menu_night_light_config_save(st->temperature, st->transition_ms, &error))
    {
        g_warning("extras-menu: couldn't save the Night Light settings: %s",
                  error != NULL ? error->message : "unknown error");
        g_clear_error(&error);
    }
}

static gboolean
on_save_timeout(gpointer user_data)
{
    PageState *st = user_data;

    st->save_timeout = 0;
    save_now(st);
    return G_SOURCE_REMOVE;
}

static void
schedule_save(PageState *st)
{
    if (st->save_timeout != 0)
        g_source_remove(st->save_timeout);

    st->save_timeout = g_timeout_add(SAVE_DELAY_MS, on_save_timeout, st);
}

/* --- the sliders ---------------------------------------------------------------------- */

static gchar *
on_format_temperature(GtkScale *scale, gdouble value, gpointer user_data)
{
    (void) scale;
    (void) user_data;
    return g_strdup_printf("%.0f K", value);
}

static gchar *
on_format_transition(GtkScale *scale, gdouble value, gpointer user_data)
{
    (void) scale;
    (void) user_data;
    return g_strdup_printf("%.0f ms", value);
}

static void
on_temperature_changed(GtkRange *range, gpointer user_data)
{
    PageState *st = user_data;

    st->temperature = (guint) gtk_range_get_value(range);
    schedule_save(st);
}

static void
on_transition_changed(GtkRange *range, gpointer user_data)
{
    PageState *st = user_data;

    st->transition_ms = (guint) gtk_range_get_value(range);
    schedule_save(st);
}

static void
on_reset_clicked(GtkButton *button, gpointer user_data)
{
    PageState *st = user_data;
    (void) button;

    /* The sliders' own "value-changed" handlers update the state and
     * schedule the save. */
    gtk_range_set_value(GTK_RANGE(st->temperature_scale),
                        EXTRAS_MENU_NIGHT_LIGHT_TEMPERATURE_DEFAULT);
    gtk_range_set_value(GTK_RANGE(st->transition_scale),
                        EXTRAS_MENU_NIGHT_LIGHT_TRANSITION_DEFAULT);
}

/* A horizontal slider with its value shown at the right, and a word at
 * each end saying which way is which. */
static GtkWidget *
make_scale(gdouble minimum, gdouble maximum, gdouble step, gdouble value,
           const gchar *minimum_text, const gchar *maximum_text,
           gchar *(*format)(GtkScale *, gdouble, gpointer))
{
    GtkWidget *scale = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL,
                                                 minimum, maximum, step);

    gtk_scale_set_digits(GTK_SCALE(scale), 0);
    gtk_scale_set_draw_value(GTK_SCALE(scale), TRUE);
    gtk_scale_set_value_pos(GTK_SCALE(scale), GTK_POS_RIGHT);
    gtk_scale_add_mark(GTK_SCALE(scale), minimum, GTK_POS_BOTTOM, minimum_text);
    gtk_scale_add_mark(GTK_SCALE(scale), maximum, GTK_POS_BOTTOM, maximum_text);
    gtk_range_set_value(GTK_RANGE(scale), value);
    gtk_widget_set_hexpand(scale, TRUE);
    gtk_widget_set_size_request(scale, 280, -1);
    g_signal_connect(scale, "format-value", G_CALLBACK(format), NULL);

    return scale;
}

/* --- building the page ------------------------------------------------------------------ */

static void
on_page_destroy(GtkWidget *page, gpointer user_data)
{
    PageState *st = user_data;
    (void) page;

    /* A change made just before closing the dialog must not be lost. */
    if (st->save_timeout != 0)
    {
        g_source_remove(st->save_timeout);
        st->save_timeout = 0;
        save_now(st);
    }

    g_free(st);
}

GtkWidget *
extras_menu_night_light_page_new(void)
{
    PageState *st = g_new0(PageState, 1);
    st->temperature = extras_menu_night_light_config_get_temperature();
    st->transition_ms = extras_menu_night_light_config_get_transition_ms();

    GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);

    GtkWidget *heading = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(heading), "<b>Night light</b>");
    gtk_label_set_xalign(GTK_LABEL(heading), 0.0);
    gtk_box_pack_start(GTK_BOX(page), heading, FALSE, FALSE, 0);

    GtkWidget *description = gtk_label_new(
        "Settings for the Night Light button in the menu, which makes the "
        "screen's colors warmer (less blue) and back again. Changes apply "
        "immediately: if Night Light is on, the screen moves to the new "
        "temperature.");
    gtk_label_set_line_wrap(GTK_LABEL(description), TRUE);
    gtk_label_set_xalign(GTK_LABEL(description), 0.0);
    gtk_label_set_max_width_chars(GTK_LABEL(description), 60);
    gtk_style_context_add_class(gtk_widget_get_style_context(description), "dim-label");
    gtk_box_pack_start(GTK_BOX(page), description, FALSE, FALSE, 0);

    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(grid), 16);
    gtk_grid_set_row_spacing(GTK_GRID(grid), 18);
    gtk_widget_set_margin_top(grid, 8);

    st->temperature_scale = make_scale(EXTRAS_MENU_NIGHT_LIGHT_TEMPERATURE_MIN,
                                       EXTRAS_MENU_NIGHT_LIGHT_TEMPERATURE_MAX, 100,
                                       st->temperature, "Warmer", "Cooler",
                                       on_format_temperature);
    st->transition_scale = make_scale(EXTRAS_MENU_NIGHT_LIGHT_TRANSITION_MIN,
                                      EXTRAS_MENU_NIGHT_LIGHT_TRANSITION_MAX, 10,
                                      st->transition_ms, "Instant", "Slow",
                                      on_format_transition);

    GtkWidget *temperature_label = gtk_label_new("Color temperature");
    gtk_label_set_xalign(GTK_LABEL(temperature_label), 0.0);
    gtk_widget_set_valign(temperature_label, GTK_ALIGN_START);
    gtk_widget_set_margin_top(temperature_label, 4);

    GtkWidget *transition_label = gtk_label_new("Transition time");
    gtk_label_set_xalign(GTK_LABEL(transition_label), 0.0);
    gtk_widget_set_valign(transition_label, GTK_ALIGN_START);
    gtk_widget_set_margin_top(transition_label, 4);

    gtk_grid_attach(GTK_GRID(grid), temperature_label, 0, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), st->temperature_scale, 1, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), transition_label, 0, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), st->transition_scale, 1, 1, 1, 1);
    gtk_box_pack_start(GTK_BOX(page), grid, FALSE, FALSE, 0);

    /* Connected only now, so setting the initial values above didn't
     * schedule a save. */
    g_signal_connect(st->temperature_scale, "value-changed",
                     G_CALLBACK(on_temperature_changed), st);
    g_signal_connect(st->transition_scale, "value-changed",
                     G_CALLBACK(on_transition_changed), st);

    GtkWidget *footer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_margin_top(footer, 6);

    GtkWidget *reset_button = gtk_button_new_with_label("Reset to defaults");
    g_signal_connect(reset_button, "clicked", G_CALLBACK(on_reset_clicked), st);
    gtk_box_pack_start(GTK_BOX(footer), reset_button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(page), footer, FALSE, FALSE, 0);

    g_signal_connect(page, "destroy", G_CALLBACK(on_page_destroy), st);

    return page;
}