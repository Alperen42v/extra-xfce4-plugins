#include "home-page.h"
#include "preferences.h"

#define EXTRAS_MENU_LICENSE_TEXT "License: GNU General Public License v3.0 (GPL-3.0)"
#define EXTRAS_MENU_REPO_URL     "https://github.com/Alperen42v/extra-xfce4-plugins"

GtkWidget *
extras_menu_home_page_new(void)
{
    GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);

    GtkWidget *name_label = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(name_label),
                         "<span size=\"xx-large\" weight=\"bold\">Extras Menu</span>");
    gtk_label_set_xalign(GTK_LABEL(name_label), 0.0);
    gtk_box_pack_start(GTK_BOX(page), name_label, FALSE, FALSE, 0);

    gchar *version_text = g_strdup_printf("Version %s", EXTRAS_MENU_VERSION);
    GtkWidget *version_label = gtk_label_new(version_text);
    g_free(version_text);
    gtk_label_set_xalign(GTK_LABEL(version_label), 0.0);
    gtk_style_context_add_class(gtk_widget_get_style_context(version_label), "dim-label");
    gtk_box_pack_start(GTK_BOX(page), version_label, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(page), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), FALSE, FALSE, 6);

    GtkWidget *about_label = gtk_label_new(
        "A quick-settings menu for the XFCE panel. One click on the panel "
        "button opens a dropdown with volume and brightness sliders, Wi-Fi "
        "and Bluetooth, battery status and a row of shortcut buttons.");
    gtk_label_set_line_wrap(GTK_LABEL(about_label), TRUE);
    gtk_label_set_xalign(GTK_LABEL(about_label), 0.0);
    gtk_label_set_max_width_chars(GTK_LABEL(about_label), 56);
    gtk_box_pack_start(GTK_BOX(page), about_label, FALSE, FALSE, 0);

    GtkWidget *hint_label = gtk_label_new(
        "Pick a section on the left to change how the menu looks and behaves.");
    gtk_label_set_line_wrap(GTK_LABEL(hint_label), TRUE);
    gtk_label_set_xalign(GTK_LABEL(hint_label), 0.0);
    gtk_label_set_max_width_chars(GTK_LABEL(hint_label), 56);
    gtk_style_context_add_class(gtk_widget_get_style_context(hint_label), "dim-label");
    gtk_box_pack_start(GTK_BOX(page), hint_label, FALSE, FALSE, 0);

    /* Footer, pinned to the bottom of the page whatever its height:
     * licence line, then the project link as the very last thing.
     * Packed from the end, so the first call below ends up lowest. */
    GtkWidget *repo_link = gtk_link_button_new_with_label(EXTRAS_MENU_REPO_URL, EXTRAS_MENU_REPO_URL);
    gtk_widget_set_halign(repo_link, GTK_ALIGN_START);

    /* A link button carries button padding that would push its text in
     * from the left edge of the labels above it; drop the horizontal
     * part so everything lines up. */
    GtkCssProvider *link_css = gtk_css_provider_new();
    gtk_css_provider_load_from_data(link_css, "button { padding: 2px 0; min-height: 0; }", -1, NULL);
    gtk_style_context_add_provider(gtk_widget_get_style_context(repo_link),
                                    GTK_STYLE_PROVIDER(link_css),
                                    GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(link_css);
    gtk_widget_set_tooltip_text(repo_link, "Open the project page on GitHub");
    gtk_box_pack_end(GTK_BOX(page), repo_link, FALSE, FALSE, 0);

    GtkWidget *license_label = gtk_label_new(EXTRAS_MENU_LICENSE_TEXT);
    gtk_label_set_xalign(GTK_LABEL(license_label), 0.0);
    gtk_style_context_add_class(gtk_widget_get_style_context(license_label), "dim-label");
    gtk_box_pack_end(GTK_BOX(page), license_label, FALSE, FALSE, 0);

    gtk_box_pack_end(GTK_BOX(page), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), FALSE, FALSE, 6);

    return page;
}