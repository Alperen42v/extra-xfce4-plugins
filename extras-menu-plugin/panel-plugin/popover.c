#include "popover.h"

/*
 * All shape/spacing styling lives here. Colors are intentionally left
 * to the active GTK theme: we only ever set "checked" state via the
 * normal GtkToggleButton :checked selector, which every theme already
 * themes with its own selected/accent color. We do NOT hardcode any
 * background-color for individual pills/rows here, so those follow
 * whatever theme (Adwaita, Arc, Matcha, ...) the user has active.
 *
 * The one exception is .extras-menu-popover's own background below:
 * plain GtkBox nodes (which is what the root content container is)
 * don't paint a background on their own even when a theme defines
 * one, so without an explicit background here the box would stay
 * fully transparent -- previously masked only by the window's own
 * (opaque, square-cornered) background, which is why removing that to
 * fix the black-corners issue made the whole dropdown transparent.
 * @theme_bg_color is the standard GTK3 symbolic color themes use for
 * this kind of surface (menus, popovers), so this still respects
 * whatever theme is active rather than hardcoding a specific color.
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
    /* Top bar: battery badge is a small pill, quick-action buttons are
     * plain icon buttons (no pill background) so they read as
     * lightweight/secondary next to the battery badge and the pill
     * grid below -- matches the mockup, where these are unadorned
     * icons rather than filled buttons. */
    ".extras-menu-battery-badge {"
    "  border-radius: 10px;"
    "  padding: 4px 10px;"
    "  background-color: alpha(@theme_fg_color, 0.08);"
    "}"
    ".extras-menu-top-bar-button {"
    "  border-radius: 8px;"
    "  padding: 4px;"
    "  min-width: 0;"
    "  min-height: 0;"
    "}"
    ".extras-menu-popover {"
    "  padding: 10px;"
    "  border-radius: 12px;"
    "  background-color: @theme_bg_color;"
    "}"
    /* Split pill (network toggle + expand chevron): the outer box just
     * clips its two children into one pill-shaped silhouette. The
     * children's own halves of the border-radius (below) create the
     * combined rounded-rect look, so this only needs overflow
     * clipping, not a border-radius of its own. */
    ".extras-menu-split-pill {"
    "  border-radius: 14px;"
    "}"
    /* Left half: rounded on the left, square on the right where it
     * meets the divider. */
    ".extras-menu-split-pill-main {"
    "  border-top-left-radius: 14px;"
    "  border-bottom-left-radius: 14px;"
    "  border-top-right-radius: 0;"
    "  border-bottom-right-radius: 0;"
    "  padding: 10px 14px;"
    "  min-height: 28px;"
    "}"
    /* The divider itself: intentionally faint (not a full-contrast
     * border) so it reads as a soft seam between the two halves rather
     * than a hard line competing with the pill's own outline. Sized
     * down from the pill's full height so it doesn't touch the top/
     * bottom edges. */
    ".extras-menu-split-pill-divider {"
    "  min-height: 16px;"
    "  margin-top: 6px;"
    "  margin-bottom: 6px;"
    "  opacity: 0.25;"
    "}"
    /* Right half: rounded on the right, square on the left, narrower
     * than the main button since it only holds a chevron glyph. */
    ".extras-menu-split-pill-expand {"
    "  border-top-right-radius: 14px;"
    "  border-bottom-right-radius: 14px;"
    "  border-top-left-radius: 0;"
    "  border-bottom-left-radius: 0;"
    "  padding: 10px 10px;"
    "  min-height: 28px;"
    "  min-width: 0;"
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

/* A pill split into two independently clickable regions, separated by
 * a subtle vertical divider: a main toggle button on the left (icon +
 * label, e.g. turning Wi-Fi on/off) and a small chevron button on the
 * right (e.g. revealing the network list) -- used for the network
 * pill, where "turn Wi-Fi on" and "show me the network list" are
 * different actions the mockup's single ">" glyph doesn't distinguish
 * between. Returns the outer container (ready to place in the grid);
 * out_main_toggle, out_label, out_icon, out_expand_button,
 * out_chevron_icon, out_icon_stack and out_spinner (all optional) hand
 * back the pieces callers need to wire up or update dynamically --
 * out_chevron_icon so the caller can flip it (e.g. to
 * pan-down-symbolic while the list below it is expanded, and back to
 * pan-end-symbolic when collapsed); out_icon_stack/out_spinner so the
 * caller can show a spinner in place of the leading icon while a
 * connection attempt is in progress (gtk_stack_set_visible_child_name
 * the stack to "spinner" and gtk_spinner_start(), then back to "icon"
 * and gtk_spinner_stop() when done). */
static GtkWidget *
extras_menu_make_split_pill(const gchar *icon_name,
                             const gchar *label_text,
                             GtkWidget **out_main_toggle,
                             GtkWidget **out_label,
                             GtkWidget **out_icon,
                             GtkWidget **out_expand_button,
                             GtkWidget **out_chevron_icon,
                             GtkWidget **out_icon_stack,
                             GtkWidget **out_spinner)
{
    GtkWidget *outer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    extras_menu_apply_css(outer);
    gtk_style_context_add_class(gtk_widget_get_style_context(outer), "extras-menu-split-pill");

    /* --- left: main toggle (icon + label) --- */
    GtkWidget *main_toggle = gtk_toggle_button_new();
    extras_menu_apply_css(main_toggle);
    gtk_style_context_add_class(gtk_widget_get_style_context(main_toggle),
                                 "extras-menu-split-pill-main");
    gtk_widget_set_hexpand(main_toggle, TRUE);

    GtkWidget *main_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *icon = gtk_image_new_from_icon_name(icon_name, GTK_ICON_SIZE_BUTTON);

    /* Stack lets the icon and spinner occupy the same slot, swapped
     * out via gtk_stack_set_visible_child() while a connection attempt
     * is in progress -- avoids the pill's width/layout jumping around
     * when switching between them, unlike show/hide on two separate
     * packed widgets. */
    GtkWidget *icon_stack = gtk_stack_new();
    GtkWidget *spinner = gtk_spinner_new();
    gtk_stack_add_named(GTK_STACK(icon_stack), icon, "icon");
    gtk_stack_add_named(GTK_STACK(icon_stack), spinner, "spinner");
    gtk_stack_set_visible_child_name(GTK_STACK(icon_stack), "icon");

    GtkWidget *label = gtk_label_new(label_text);
    gtk_label_set_xalign(GTK_LABEL(label), 0.0);
    gtk_widget_set_hexpand(label, TRUE);

    gtk_box_pack_start(GTK_BOX(main_box), icon_stack, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(main_box), label, TRUE, TRUE, 0);
    gtk_container_add(GTK_CONTAINER(main_toggle), main_box);

    /* --- subtle divider between the two regions --- */
    GtkWidget *divider = gtk_separator_new(GTK_ORIENTATION_VERTICAL);
    extras_menu_apply_css(divider);
    gtk_style_context_add_class(gtk_widget_get_style_context(divider),
                                 "extras-menu-split-pill-divider");

    /* --- right: small chevron button, expands the network list --- */
    GtkWidget *expand_button = gtk_button_new();
    extras_menu_apply_css(expand_button);
    gtk_style_context_add_class(gtk_widget_get_style_context(expand_button),
                                 "extras-menu-split-pill-expand");

    GtkWidget *chevron = gtk_image_new_from_icon_name("pan-end-symbolic", GTK_ICON_SIZE_BUTTON);
    gtk_container_add(GTK_CONTAINER(expand_button), chevron);

    gtk_box_pack_start(GTK_BOX(outer), main_toggle, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(outer), divider, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(outer), expand_button, FALSE, FALSE, 0);

    if (out_main_toggle != NULL)
        *out_main_toggle = main_toggle;
    if (out_label != NULL)
        *out_label = label;
    if (out_icon != NULL)
        *out_icon = icon;
    if (out_expand_button != NULL)
        *out_expand_button = expand_button;
    if (out_chevron_icon != NULL)
        *out_chevron_icon = chevron;
    if (out_icon_stack != NULL)
        *out_icon_stack = icon_stack;
    if (out_spinner != NULL)
        *out_spinner = spinner;

    return outer;
}

/* Simple single-button pill with a trailing chevron, no split
 * click-regions -- used for placeholder entries like "quality" that
 * don't yet have real behavior wired up (unlike the network pill,
 * which needs the on/off vs. expand-list distinction from
 * extras_menu_make_split_pill above). */
static GtkWidget *
extras_menu_make_pill_expander(const gchar *icon_name, const gchar *label_text)
{
    GtkWidget *button = gtk_button_new();
    extras_menu_apply_css(button);
    gtk_style_context_add_class(gtk_widget_get_style_context(button), "extras-menu-pill-wide");

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *icon = gtk_image_new_from_icon_name(icon_name, GTK_ICON_SIZE_BUTTON);
    GtkWidget *label = gtk_label_new(label_text);
    gtk_label_set_xalign(GTK_LABEL(label), 0.0);
    GtkWidget *chevron = gtk_image_new_from_icon_name("pan-end-symbolic", GTK_ICON_SIZE_BUTTON);

    gtk_box_pack_start(GTK_BOX(box), icon, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), label, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(box), chevron, FALSE, FALSE, 0);

    gtk_container_add(GTK_CONTAINER(button), box);

    return button;
}

/* Top status bar: a battery percentage badge on the left, a row of
 * small icon-only quick-action buttons on the right (screenshot,
 * settings, lock, power) -- mirrors the mockup's topmost row. Purely
 * visual for now: the battery percentage is a fixed placeholder and
 * the buttons don't do anything yet, both wired up as a follow-up once
 * this shape is approved. out_battery_label and out_battery_icon (both
 * optional) hand back the badge's pieces so a later pass can drive
 * them from a real UPower backend; the four button out-params
 * similarly hand back the action buttons themselves for wiring up
 * click handlers later. */
static GtkWidget *
extras_menu_make_top_bar(GtkWidget **out_battery_label,
                          GtkWidget **out_battery_icon,
                          GtkWidget **out_screenshot_button,
                          GtkWidget **out_settings_button,
                          GtkWidget **out_lock_button,
                          GtkWidget **out_power_button)
{
    GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);

    /* --- left: battery badge (icon + percentage), pill-shaped --- */
    GtkWidget *battery_badge = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    extras_menu_apply_css(battery_badge);
    gtk_style_context_add_class(gtk_widget_get_style_context(battery_badge),
                                 "extras-menu-battery-badge");

    GtkWidget *battery_icon = gtk_image_new_from_icon_name("battery-good-charging-symbolic",
                                                             GTK_ICON_SIZE_BUTTON);
    GtkWidget *battery_label = gtk_label_new("100%");

    gtk_box_pack_start(GTK_BOX(battery_badge), battery_icon, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(battery_badge), battery_label, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(bar), battery_badge, FALSE, FALSE, 0);

    /* Spacer so the quick-action buttons sit at the right edge
     * regardless of the battery badge's width. */
    GtkWidget *spacer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_hexpand(spacer, TRUE);
    gtk_box_pack_start(GTK_BOX(bar), spacer, TRUE, TRUE, 0);

    /* --- right: quick-action icon buttons --- */
    GtkWidget *screenshot_button = gtk_button_new();
    extras_menu_apply_css(screenshot_button);
    gtk_style_context_add_class(gtk_widget_get_style_context(screenshot_button),
                                 "extras-menu-top-bar-button");
    gtk_container_add(GTK_CONTAINER(screenshot_button),
                       gtk_image_new_from_icon_name("camera-photo-symbolic", GTK_ICON_SIZE_BUTTON));

    GtkWidget *settings_button = gtk_button_new();
    extras_menu_apply_css(settings_button);
    gtk_style_context_add_class(gtk_widget_get_style_context(settings_button),
                                 "extras-menu-top-bar-button");
    gtk_container_add(GTK_CONTAINER(settings_button),
                       gtk_image_new_from_icon_name("preferences-system-symbolic", GTK_ICON_SIZE_BUTTON));

    GtkWidget *lock_button = gtk_button_new();
    extras_menu_apply_css(lock_button);
    gtk_style_context_add_class(gtk_widget_get_style_context(lock_button),
                                 "extras-menu-top-bar-button");
    gtk_container_add(GTK_CONTAINER(lock_button),
                       gtk_image_new_from_icon_name("system-lock-screen-symbolic", GTK_ICON_SIZE_BUTTON));

    GtkWidget *power_button = gtk_button_new();
    extras_menu_apply_css(power_button);
    gtk_style_context_add_class(gtk_widget_get_style_context(power_button),
                                 "extras-menu-top-bar-button");
    gtk_container_add(GTK_CONTAINER(power_button),
                       gtk_image_new_from_icon_name("system-shutdown-symbolic", GTK_ICON_SIZE_BUTTON));

    gtk_box_pack_start(GTK_BOX(bar), screenshot_button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(bar), settings_button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(bar), lock_button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(bar), power_button, FALSE, FALSE, 0);

    if (out_battery_label != NULL)
        *out_battery_label = battery_label;
    if (out_battery_icon != NULL)
        *out_battery_icon = battery_icon;
    if (out_screenshot_button != NULL)
        *out_screenshot_button = screenshot_button;
    if (out_settings_button != NULL)
        *out_settings_button = settings_button;
    if (out_lock_button != NULL)
        *out_lock_button = lock_button;
    if (out_power_button != NULL)
        *out_power_button = power_button;

    return bar;
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
extras_menu_popover_content_new(GtkWidget **battery_label, GtkWidget **battery_icon,
                                 GtkWidget **screenshot_button, GtkWidget **settings_button,
                                 GtkWidget **lock_button, GtkWidget **power_button,
                                 GtkWidget **volume_scale, GtkWidget **volume_icon,
                                 GtkWidget **brightness_scale, GtkWidget **bluetooth_toggle,
                                 GtkWidget **network_toggle, GtkWidget **network_pill_label,
                                 GtkWidget **network_pill_icon, GtkWidget **network_expand_button,
                                 GtkWidget **network_expand_chevron,
                                 GtkWidget **network_icon_stack, GtkWidget **network_spinner,
                                 GtkWidget **network_revealer, GtkWidget **network_list_box)
{
    GtkWidget *root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    extras_menu_apply_css(root);
    gtk_style_context_add_class(gtk_widget_get_style_context(root),
                                 "extras-menu-popover");
    gtk_widget_set_size_request(root, 260, -1);

    /* --- top status bar: battery badge + quick-action buttons --- */
    gtk_box_pack_start(GTK_BOX(root),
                        extras_menu_make_top_bar(battery_label, battery_icon,
                                                  screenshot_button, settings_button,
                                                  lock_button, power_button),
                        FALSE, FALSE, 0);

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

    GtkWidget *network_label = NULL;
    GtkWidget *network_icon = NULL;
    GtkWidget *network_main_toggle = NULL;
    GtkWidget *network_expand = NULL;
    GtkWidget *network_chevron = NULL;
    GtkWidget *network_stack = NULL;
    GtkWidget *network_spin = NULL;
    GtkWidget *wired = extras_menu_make_split_pill("network-wired-symbolic", "Wi-Fi",
                                                     &network_main_toggle, &network_label,
                                                     &network_icon, &network_expand,
                                                     &network_chevron, &network_stack,
                                                     &network_spin);
    if (network_toggle != NULL)
        *network_toggle = network_main_toggle;
    if (network_pill_label != NULL)
        *network_pill_label = network_label;
    if (network_pill_icon != NULL)
        *network_pill_icon = network_icon;
    if (network_expand_button != NULL)
        *network_expand_button = network_expand;
    if (network_expand_chevron != NULL)
        *network_expand_chevron = network_chevron;
    if (network_icon_stack != NULL)
        *network_icon_stack = network_stack;
    if (network_spinner != NULL)
        *network_spinner = network_spin;

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

    /* --- Wi-Fi network list, hidden by default, revealed when the
     * "Wi-Fi" pill above is clicked. Full width (not part of the 2x3
     * grid) since a network list doesn't fit the two-column layout. */
    GtkWidget *revealer = gtk_revealer_new();
    gtk_revealer_set_transition_type(GTK_REVEALER(revealer), GTK_REVEALER_TRANSITION_TYPE_SLIDE_DOWN);
    gtk_revealer_set_reveal_child(GTK_REVEALER(revealer), FALSE);

    GtkWidget *scrolled = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled),
                                    GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    /* Cap the list's height so a long scan result doesn't make the
     * dropdown grow to fill the whole screen -- it scrolls instead. */
    gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(scrolled), 200);
    gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(scrolled), TRUE);

    GtkWidget *list_box = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(list_box), GTK_SELECTION_NONE);
    gtk_container_add(GTK_CONTAINER(scrolled), list_box);
    gtk_container_add(GTK_CONTAINER(revealer), scrolled);

    gtk_box_pack_start(GTK_BOX(root), revealer, FALSE, FALSE, 0);

    if (network_revealer != NULL)
        *network_revealer = revealer;
    if (network_list_box != NULL)
        *network_list_box = list_box;

    gtk_widget_show_all(root);
    /* The revealer's child (scrolled/list_box) was just made visible
     * by show_all above, but the revealer itself stays collapsed
     * (0-height) until reveal-child is set TRUE -- show_all does not
     * override that, it only affects widget visibility, not the
     * revealer's own reveal state. */
    return root;
}