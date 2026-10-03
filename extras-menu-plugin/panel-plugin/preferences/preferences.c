#include "preferences.h"
#include "quick-actions-page.h"

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

    gtk_window_set_resizable(GTK_WINDOW(dialog), FALSE);
    gtk_container_set_border_width(GTK_CONTAINER(dialog), 12);

    GtkWidget *content_area = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(box), 8);
    gtk_container_add(GTK_CONTAINER(content_area), box);

    GtkWidget *title_label = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(title_label), "<b>Extras Menu</b>");
    gtk_label_set_xalign(GTK_LABEL(title_label), 0.0);
    gtk_box_pack_start(GTK_BOX(box), title_label, FALSE, FALSE, 0);

    gchar *version_text = g_strdup_printf("Version %s", EXTRAS_MENU_VERSION);
    GtkWidget *version_label = gtk_label_new(version_text);
    g_free(version_text);
    gtk_label_set_xalign(GTK_LABEL(version_label), 0.0);
    gtk_style_context_add_class(gtk_widget_get_style_context(version_label), "dim-label");
    gtk_box_pack_start(GTK_BOX(box), version_label, FALSE, FALSE, 0);

    GtkWidget *separator = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_box_pack_start(GTK_BOX(box), separator, FALSE, FALSE, 4);

    gtk_box_pack_start(GTK_BOX(box), extras_menu_quick_actions_page_new(), TRUE, TRUE, 0);

    g_signal_connect(dialog, "response", G_CALLBACK(on_dialog_response), NULL);

    gtk_widget_show_all(dialog);
}