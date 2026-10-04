#include "preferences.h"
#include "home-page.h"
#include "quick-actions-page.h"

/*
 * The dialog is a sidebar of sections on the left and the selected
 * section's page on the right, so settings don't pile up on one long
 * screen as the plugin grows. The first entry below is what opens.
 *
 * To add a section: write a function returning its page widget (see
 * home-page.h / quick-actions-page.h for the shape), then add one line
 * to this table. The sidebar entry and the page switching come for free.
 */
typedef struct
{
    const gchar *name;   /* internal id, unique */
    const gchar *title;  /* sidebar label */
    GtkWidget *(*build)(void);
} PreferencesSection;

static const PreferencesSection sections[] = {
    { "home",          "Home",                 extras_menu_home_page_new },
    { "quick-actions", "Quick action buttons", extras_menu_quick_actions_page_new },
};

/* The one open preferences dialog, or NULL. Cleared automatically when
 * the dialog is destroyed (see gtk_widget_destroyed below). */
static GtkWidget *preferences_dialog = NULL;

static void
on_dialog_response(GtkDialog *dialog, gint response_id, gpointer user_data)
{
    (void) response_id;
    (void) user_data;
    gtk_widget_destroy(GTK_WIDGET(dialog));
}

void
extras_menu_preferences_show(XfcePanelPlugin *panel_plugin)
{
    if (preferences_dialog != NULL)
    {
        gtk_window_present(GTK_WINDOW(preferences_dialog));
        return;
    }

    GtkWidget *dialog = gtk_dialog_new_with_buttons(
        "Extras Menu Preferences",
        GTK_WINDOW(gtk_widget_get_toplevel(GTK_WIDGET(panel_plugin))),
        GTK_DIALOG_DESTROY_WITH_PARENT,
        "_Close", GTK_RESPONSE_CLOSE,
        NULL);

    preferences_dialog = dialog;
    g_signal_connect(dialog, "destroy", G_CALLBACK(gtk_widget_destroyed), &preferences_dialog);

    gtk_window_set_default_size(GTK_WINDOW(dialog), 720, -1);

    GtkWidget *content_area = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
    GtkWidget *layout = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_box_pack_start(GTK_BOX(content_area), layout, TRUE, TRUE, 0);

    /* The stack is homogeneous by default, so the dialog is as large as
     * its biggest page and doesn't resize when switching sections. */
    GtkWidget *stack = gtk_stack_new();
    gtk_stack_set_transition_type(GTK_STACK(stack), GTK_STACK_TRANSITION_TYPE_CROSSFADE);
    gtk_widget_set_hexpand(stack, TRUE);

    for (guint i = 0; i < G_N_ELEMENTS(sections); i++)
    {
        GtkWidget *page = sections[i].build();
        gtk_widget_set_margin_start(page, 18);
        gtk_widget_set_margin_end(page, 18);
        gtk_widget_set_margin_top(page, 16);
        gtk_widget_set_margin_bottom(page, 16);
        gtk_stack_add_titled(GTK_STACK(stack), page, sections[i].name, sections[i].title);
    }

    /* The first section added is the one shown on open; stated
     * explicitly so reordering the table can't change that by accident. */
    gtk_stack_set_visible_child_name(GTK_STACK(stack), sections[0].name);

    GtkWidget *sidebar = gtk_stack_sidebar_new();
    gtk_stack_sidebar_set_stack(GTK_STACK_SIDEBAR(sidebar), GTK_STACK(stack));
    gtk_widget_set_size_request(sidebar, 190, -1);

    gtk_box_pack_start(GTK_BOX(layout), sidebar, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(layout), gtk_separator_new(GTK_ORIENTATION_VERTICAL), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(layout), stack, TRUE, TRUE, 0);

    g_signal_connect(dialog, "response", G_CALLBACK(on_dialog_response), NULL);

    gtk_widget_show_all(dialog);
}