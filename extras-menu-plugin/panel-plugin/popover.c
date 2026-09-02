#include "popover.h"

/*
 * All shape/spacing styling lives here. Colors are intentionally left
 * to the active GTK theme: we only ever set "checked" state via the
 * normal GtkToggleButton :checked selector, which every theme already
 * themes with its own selected/accent color. We do NOT hardcode any
 * background-color here, so the widget follows whatever theme (Adwaita,
 * Arc, Matcha, ...) the user has active.
 */
static const gchar *EXTRAS_MENU_CSS =
    ".extras-menu-pill {"
    "  border-radius: 14px;"
    "  padding: 10px 12px;"
    "  min-height: 28px;"
    "}"
    ".extras-menu-pill-wide {"
    "  border-radius: 14px;"
    "  padding: 10px 14px;"
    "  min-height: 28px;"
    "}"
    ".extras-menu-volume-row {"
    "  border-radius: 14px;"
    "  padding: 8px 14px;"
    "}"
    ".extras-menu-popover {"
    "  padding: 10px;"
    "}";

static void
extras_menu_apply_css(GtkWidget *widget)
{
    GtkCssProvider *provider = gtk_css_provider_new();
    gtk_css_provider_load_from_data(provider, EXTRAS_MENU_CSS, -1, NULL);

    GtkStyleContext *context = gtk_widget_get_style_context(widget);
    gtk_style_context_add_provider(context,
                                    GTK_STYLE_PROVIDER(provider),
                                    GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);

    g_object_unref(provider);
}

/* Small helper: a pill-shaped toggle button with an icon + label,
 * matching the "Wired / Bluetooth / Dark Mode / Balanced / Aeroplane
 * Mode" style entries in the mockup. */
static GtkWidget *
extras_menu_make_pill_toggle(const gchar *icon_name,
                              const gchar *label_text,
                              gboolean active,
                              gboolean wide)
{
    GtkWidget *button = gtk_toggle_button_new();
    gtk_widget_set_name(button, "extras-menu-pill-button");
    extras_menu_apply_css(button);

    GtkStyleContext *ctx = gtk_widget_get_style_context(button);
    gtk_style_context_add_class(ctx, wide ? "extras-menu-pill-wide"
                                           : "extras-menu-pill");

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *icon = gtk_image_new_from_icon_name(icon_name, GTK_ICON_SIZE_BUTTON);
    GtkWidget *label = gtk_label_new(label_text);
    gtk_label_set_xalign(GTK_LABEL(label), 0.0);

    gtk_box_pack_start(GTK_BOX(box), icon, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), label, TRUE, TRUE, 0);

    gtk_container_add(GTK_CONTAINER(button), box);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(button), active);

    return button;
}

/* A pill row with a chevron on the right, used for entries that open a
 * submenu (e.g. the "Wired" network list, or the quality-preset row
 * next to Balanced in the mockup). Purely visual for now: the chevron
 * click does nothing until a follow-up submenu is wired in. */
static GtkWidget *
extras_menu_make_pill_expander(const gchar *icon_name,
                                const gchar *label_text)
{
    GtkWidget *button = gtk_button_new();
    extras_menu_apply_css(button);

    GtkStyleContext *ctx = gtk_widget_get_style_context(button);
    gtk_style_context_add_class(ctx, "extras-menu-pill-wide");

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *icon = gtk_image_new_from_icon_name(icon_name, GTK_ICON_SIZE_BUTTON);
    GtkWidget *label = gtk_label_new(label_text);
    gtk_label_set_xalign(GTK_LABEL(label), 0.0);
    GtkWidget *chevron = gtk_image_new_from_icon_name("pan-end-symbolic",
                                                        GTK_ICON_SIZE_BUTTON);

    gtk_box_pack_start(GTK_BOX(box), icon, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), label, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(box), chevron, FALSE, FALSE, 0);

    gtk_container_add(GTK_CONTAINER(button), box);

    return button;
}

/* Generic full-width slider row with a leading icon, used for both
 * the volume row and the brightness row -- same visual shape, only
 * the icon and starting value differ. The scale itself is handed back
 * via out_scale so the caller can wire it up to a real backend.
 * out_icon (optional) hands back the icon GtkImage too, so callers
 * that want to swap it dynamically (e.g. volume -> muted icon at 0%)
 * can do so without having to dig through the row's children. */
static GtkWidget *
extras_menu_make_slider_row(const gchar *icon_name, GtkWidget **out_scale, GtkWidget **out_icon)
{
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    extras_menu_apply_css(row);
    gtk_style_context_add_class(gtk_widget_get_style_context(row),
                                 "extras-menu-volume-row");

    GtkWidget *icon = gtk_image_new_from_icon_name(icon_name, GTK_ICON_SIZE_BUTTON);
    GtkWidget *scale = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0, 100, 1);
    gtk_scale_set_draw_value(GTK_SCALE(scale), FALSE);
    gtk_range_set_value(GTK_RANGE(scale), 0);
    gtk_widget_set_hexpand(scale, TRUE);

    gtk_box_pack_start(GTK_BOX(row), icon, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(row), scale, TRUE, TRUE, 0);

    if (out_scale != NULL)
        *out_scale = scale;
    if (out_icon != NULL)
        *out_icon = icon;

    return row;
}

GtkWidget *
extras_menu_popover_content_new(GtkWidget **volume_scale, GtkWidget **volume_icon,
                                 GtkWidget **brightness_scale, GtkWidget **bluetooth_toggle)
{
    GtkWidget *root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    extras_menu_apply_css(root);
    gtk_style_context_add_class(gtk_widget_get_style_context(root),
                                 "extras-menu-popover");
    gtk_widget_set_size_request(root, 260, -1);

    /* --- volume slider row --- */
    GtkWidget *vol_scale = NULL;
    GtkWidget *vol_icon = NULL;
    gtk_box_pack_start(GTK_BOX(root),
                        extras_menu_make_slider_row("audio-volume-high-symbolic", &vol_scale, &vol_icon),
                        FALSE, FALSE, 0);
    if (volume_scale != NULL)
        *volume_scale = vol_scale;
    if (volume_icon != NULL)
        *volume_icon = vol_icon;

    /* --- brightness slider row, directly below volume --- */
    GtkWidget *bright_scale = NULL;
    gtk_box_pack_start(GTK_BOX(root),
                        extras_menu_make_slider_row("display-brightness-symbolic", &bright_scale, NULL),
                        FALSE, FALSE, 0);
    if (brightness_scale != NULL)
        *brightness_scale = bright_scale;

    /* --- 2x3 pill grid, mirroring the mockup layout --- */
    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 8);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 8);
    gtk_grid_set_column_homogeneous(GTK_GRID(grid), TRUE);

    GtkWidget *wired = extras_menu_make_pill_expander("network-wired-symbolic", "Wired");
    GtkWidget *quality = extras_menu_make_pill_expander("preferences-system-symbolic", "");

    /* Starts unchecked regardless of the mockup's default -- the real
     * state comes from the Bluetooth backend shortly after the
     * dropdown is built, same startup pattern as the volume/brightness
     * sliders (they start at 0 until their backends report in). */
    GtkWidget *bluetooth = extras_menu_make_pill_toggle("bluetooth-symbolic", "Bluetooth", FALSE, FALSE);
    if (bluetooth_toggle != NULL)
        *bluetooth_toggle = bluetooth;

    GtkWidget *balanced = extras_menu_make_pill_toggle("weather-clear-symbolic", "Balanced", FALSE, FALSE);

    GtkWidget *dark_mode = extras_menu_make_pill_toggle("weather-clear-night-symbolic", "Dark Mode", FALSE, FALSE);
    GtkWidget *airplane = extras_menu_make_pill_toggle("airplane-mode-symbolic", "Aeroplane Mode", FALSE, FALSE);

    gtk_grid_attach(GTK_GRID(grid), wired,      0, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), quality,    1, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), bluetooth,  0, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), balanced,   1, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), dark_mode,  0, 2, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), airplane,   1, 2, 1, 1);

    gtk_box_pack_start(GTK_BOX(root), grid, FALSE, FALSE, 0);

    gtk_widget_show_all(root);
    return root;
}