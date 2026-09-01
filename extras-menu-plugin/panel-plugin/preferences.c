#include "preferences.h"

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
    GtkWidget *dialog = gtk_dialog_new_with_buttons(
        "Extras Menu Preferences",
        GTK_WINDOW(gtk_widget_get_toplevel(GTK_WIDGET(panel_plugin))),
        GTK_DIALOG_DESTROY_WITH_PARENT,
        "_Close", GTK_RESPONSE_CLOSE,
        NULL);

    gtk_window_set_resizable(GTK_WINDOW(dialog), FALSE);
    gtk_container_set_border_width(GTK_CONTAINER(dialog), 12);

    GtkWidget *content_area = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(box), 8);
    gtk_container_add(GTK_CONTAINER(content_area), box);

    GtkWidget *title_label = gtk_label_new(NULL);
    gchar *title_markup = g_strdup_printf("<b>Extras Menu</b>");
    gtk_label_set_markup(GTK_LABEL(title_label), title_markup);
    g_free(title_markup);
    gtk_label_set_xalign(GTK_LABEL(title_label), 0.0);
    gtk_box_pack_start(GTK_BOX(box), title_label, FALSE, FALSE, 0);

    GtkWidget *version_label = gtk_label_new(NULL);
    gchar *version_markup = g_strdup_printf("Version %s", EXTRAS_MENU_VERSION);
    gtk_label_set_text(GTK_LABEL(version_label), version_markup);
    g_free(version_markup);
    gtk_label_set_xalign(GTK_LABEL(version_label), 0.0);
    gtk_style_context_add_class(gtk_widget_get_style_context(version_label), "dim-label");
    gtk_box_pack_start(GTK_BOX(box), version_label, FALSE, FALSE, 0);

    /* --- release note -------------------------------------------------
     * This is a placeholder for now: as the plugin grows (real toggles
     * for Bluetooth/network/dark mode, color customization, etc.),
     * actual per-user settings controls will replace/join this note.
     * Kept as a plain, non-scrolling label since the current note is
     * short; switch to a GtkScrolledWindow if it grows substantially. */
    GtkWidget *separator = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_box_pack_start(GTK_BOX(box), separator, FALSE, FALSE, 4);

    GtkWidget *note_label = gtk_label_new(
        "This is an early alpha build. Only the volume and brightness "
        "sliders are wired up to real system controls so far -- the "
        "rest of the toggles (Bluetooth, network, Dark Mode, Aeroplane "
        "Mode) are still visual placeholders. More settings will appear "
        "on this screen as those are implemented.");
    gtk_label_set_line_wrap(GTK_LABEL(note_label), TRUE);
    gtk_label_set_xalign(GTK_LABEL(note_label), 0.0);
    gtk_widget_set_size_request(note_label, 320, -1);
    gtk_box_pack_start(GTK_BOX(box), note_label, FALSE, FALSE, 0);

    g_signal_connect(dialog, "response", G_CALLBACK(on_dialog_response), NULL);

    gtk_widget_show_all(dialog);
}