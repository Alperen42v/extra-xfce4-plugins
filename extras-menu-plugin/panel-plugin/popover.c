#include "popover.h"
#include "preferences/pill-layout-config.h"
#include "preferences/slider-layout-config.h"
#include "microphone.h"

/* Every pill carries its id (an EXTRAS_MENU_PILL_* string) under this
 * key, and the root content widget carries its pill grid under the
 * other one, so a layout can be applied without any extra bookkeeping. */
#define PILL_ID_KEY   "extras-menu-pill-id"
#define PILL_GRID_KEY "extras-menu-pill-grid"

/* The same two ideas for the slider rows: each row carries its id
 * (an EXTRAS_MENU_SLIDER_* string), and the root content widget carries
 * the box that holds the rows. */
#define SLIDER_ID_KEY  "extras-menu-slider-id"
#define SLIDER_BOX_KEY "extras-menu-slider-box"

/* The microphone row also carries its three working parts, so
 * extras_menu_popover_get_microphone() can hand them out without the
 * content constructor needing three more out-params. */
#define MIC_SCALE_KEY  "extras-menu-mic-scale"
#define MIC_BUTTON_KEY "extras-menu-mic-button"
#define MIC_ICON_KEY   "extras-menu-mic-icon"

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
    /* Microphone row: its icon is a button (click = mute), made to
     * look and measure exactly like the plain icon of the other rows
     * so the three sliders still line up. The thin marks under the
     * scale (at 40% and 100%) are kept to one hairline each. */
    ".extras-menu-mic-button {"
    "  padding: 0;"
    "  min-width: 0;"
    "  min-height: 0;"
    "  border: none;"
    "  box-shadow: none;"
    "  background: none;"
    "}"
    ".extras-menu-mic-row scale marks indicator {"
    "  min-width: 1px;"
    "  min-height: 6px;"
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
    /* Circle variant: only applies while the quick-actions box carries
     * the "extras-menu-quick-actions-circle" class (set by
     * extras_menu_popover_set_quick_action_shape()). Matching on the
     * parent's class means one switch restyles all the buttons, and a
     * button created later is already correct. Content min size 16px
     * plus 4px padding on each side = a 24x24 box (same footprint as
     * the square buttons, so five of them still fit), and a 50% radius
     * turns that into a true circle. Without the fixed size, an image
     * file icon that isn't square would give an oval. */
    ".extras-menu-quick-actions-circle .extras-menu-top-bar-button {"
    "  border-radius: 50%;"
    "  padding: 4px;"
    "  min-width: 16px;"
    "  min-height: 16px;"
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
    "}"
    /* Round (GNOME-style) variant: only applies while the pill grid
     * carries the "extras-menu-pills-round" class (set by
     * extras_menu_popover_set_pill_shape()). A pill is about 50px tall,
     * so a 25px radius makes each end exactly half a circle -- a
     * capsule. A radius larger than half the height is harmless (it is
     * scaled down to fit), a smaller one would leave visibly flat
     * sides, so this errs on the big side. The split pills (Wi-Fi,
     * Bluetooth) round only the outer end of each half and stay square
     * where the two meet at the divider. The side padding grows a
     * little so the icon and text don't crowd the curve. */
    ".extras-menu-pills-round .extras-menu-pill,"
    ".extras-menu-pills-round .extras-menu-pill-wide {"
    "  border-radius: 25px;"
    "  padding: 10px 18px;"
    "}"
    ".extras-menu-pills-round .extras-menu-split-pill {"
    "  border-radius: 25px;"
    "}"
    ".extras-menu-pills-round .extras-menu-split-pill-main {"
    "  border-top-left-radius: 25px;"
    "  border-bottom-left-radius: 25px;"
    "  border-top-right-radius: 0;"
    "  border-bottom-right-radius: 0;"
    "  padding: 10px 14px 10px 18px;"
    "}"
    ".extras-menu-pills-round .extras-menu-split-pill-expand {"
    "  border-top-right-radius: 25px;"
    "  border-bottom-right-radius: 25px;"
    "  border-top-left-radius: 0;"
    "  border-bottom-left-radius: 0;"
    "  padding: 10px 14px 10px 10px;"
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
 * matching the "Wired / Bluetooth / Dark Mode / Balanced / Airplane
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

/* A styled, empty icon button for the top bar. Public (see popover.h)
 * because the quick-action buttons are created by quick-actions.c --
 * they come and go with the user's settings -- but must look exactly
 * like every other top-bar button, and the styling lives in this file. */
GtkWidget *
extras_menu_top_bar_button_new(void)
{
    GtkWidget *button = gtk_button_new();
    extras_menu_apply_css(button);
    gtk_style_context_add_class(gtk_widget_get_style_context(button),
                                 "extras-menu-top-bar-button");
    return button;
}

void
extras_menu_popover_set_quick_action_shape(GtkWidget *quick_actions_box,
                                            ExtrasMenuQuickActionShape shape)
{
    if (quick_actions_box == NULL)
        return;

    GtkStyleContext *ctx = gtk_widget_get_style_context(quick_actions_box);

    if (shape == EXTRAS_MENU_QUICK_ACTION_SHAPE_CIRCLE)
    {
        gtk_style_context_add_class(ctx, "extras-menu-quick-actions-circle");

        /* Buttons are stretched to the top bar's height by default,
         * which would squash a circle into an oval. Centering the box
         * lets it shrink to the buttons' own (square) size. */
        gtk_widget_set_valign(quick_actions_box, GTK_ALIGN_CENTER);
    }
    else
    {
        gtk_style_context_remove_class(ctx, "extras-menu-quick-actions-circle");
        gtk_widget_set_valign(quick_actions_box, GTK_ALIGN_FILL);
    }
}

/* Top status bar: a battery percentage badge on the left, a row of
 * small icon-only quick-action buttons on the right -- mirrors the
 * mockup's topmost row. The badge's "100%" text and icon here are only
 * initial placeholders: extras-menu.c hides the badge until the UPower
 * backend (battery.c) reports a real battery, then keeps the text/icon
 * up to date. out_battery_label and out_battery_icon (both optional)
 * hand back the badge's pieces for that.
 *
 * The quick-action buttons are NOT created here: which buttons exist,
 * their icons and what they run are user settings. This only provides
 * the empty container for them (out_quick_actions_box), which
 * quick-actions.c fills in and keeps up to date. */
static GtkWidget *
extras_menu_make_top_bar(GtkWidget **out_battery_label,
                          GtkWidget **out_battery_icon,
                          GtkWidget **out_quick_actions_box)
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

    /* --- right: container for the quick-action buttons. Spacing is
     * a little tighter than the bar's own so that the maximum of five
     * buttons still fits next to the battery badge. --- */
    GtkWidget *quick_actions_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_box_pack_start(GTK_BOX(bar), quick_actions_box, FALSE, FALSE, 0);

    if (out_battery_label != NULL)
        *out_battery_label = battery_label;
    if (out_battery_icon != NULL)
        *out_battery_icon = battery_icon;
    if (out_quick_actions_box != NULL)
        *out_quick_actions_box = quick_actions_box;

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

/* The microphone's slider row. Like the other rows, but:
 *  - the scale runs 0..150 (the level can be pushed past 100%),
 *  - two thin marks sit under it, at 100% (above that the signal is
 *    amplified in software, which adds noise) and at 40% (a level that
 *    suits most microphones). The marks carry no text on purpose,
 *  - the icon is inside a flat button, so a click can mute.
 * The three parts are handed back through out-params and also stored on
 * the row (see MIC_*_KEY). */
static GtkWidget *
extras_menu_make_microphone_row(GtkWidget **out_scale, GtkWidget **out_button,
                                 GtkWidget **out_icon)
{
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    extras_menu_apply_css(row);

    GtkStyleContext *row_ctx = gtk_widget_get_style_context(row);
    gtk_style_context_add_class(row_ctx, "extras-menu-volume-row");
    gtk_style_context_add_class(row_ctx, "extras-menu-mic-row");

    GtkWidget *icon = gtk_image_new_from_icon_name("microphone-sensitivity-high-symbolic",
                                                    GTK_ICON_SIZE_BUTTON);

    GtkWidget *button = gtk_button_new();
    gtk_button_set_relief(GTK_BUTTON(button), GTK_RELIEF_NONE);
    gtk_widget_set_focus_on_click(button, FALSE);
    gtk_widget_set_valign(button, GTK_ALIGN_CENTER);
    gtk_style_context_add_class(gtk_widget_get_style_context(button),
                                 "extras-menu-mic-button");
    gtk_container_add(GTK_CONTAINER(button), icon);

    GtkWidget *scale = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0,
                                                 EXTRAS_MENU_MICROPHONE_MAX_PERCENT, 1);
    gtk_scale_set_draw_value(GTK_SCALE(scale), FALSE);
    gtk_range_set_value(GTK_RANGE(scale), 0);
    gtk_widget_set_hexpand(scale, TRUE);
    gtk_scale_add_mark(GTK_SCALE(scale), EXTRAS_MENU_MICROPHONE_STABLE_PERCENT,
                       GTK_POS_BOTTOM, NULL);
    gtk_scale_add_mark(GTK_SCALE(scale), EXTRAS_MENU_MICROPHONE_NORMAL_PERCENT,
                       GTK_POS_BOTTOM, NULL);

    /* The marks make the scale taller, below its trough; lift the icon a
     * little so it stays level with the trough rather than with the
     * middle of the whole scale. */
    gtk_widget_set_margin_bottom(button, 6);

    gtk_box_pack_start(GTK_BOX(row), button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(row), scale, TRUE, TRUE, 0);

    g_object_set_data(G_OBJECT(row), MIC_SCALE_KEY, scale);
    g_object_set_data(G_OBJECT(row), MIC_BUTTON_KEY, button);
    g_object_set_data(G_OBJECT(row), MIC_ICON_KEY, icon);

    if (out_scale != NULL)
        *out_scale = scale;
    if (out_button != NULL)
        *out_button = button;
    if (out_icon != NULL)
        *out_icon = icon;

    return row;
}

/* A collapsed, scrollable list that slides open below the pill grid
 * (used for the Wi-Fi network list and the Bluetooth device list). Full
 * width rather than part of the two-column grid, since a list of names
 * doesn't fit that layout. Starts collapsed; the caller decides when to
 * reveal it. out_list_box (optional) hands back the GtkListBox inside,
 * ready to be filled with rows. */
static GtkWidget *
extras_menu_make_list_revealer(GtkWidget **out_list_box)
{
    GtkWidget *revealer = gtk_revealer_new();
    gtk_revealer_set_transition_type(GTK_REVEALER(revealer), GTK_REVEALER_TRANSITION_TYPE_SLIDE_DOWN);
    gtk_revealer_set_reveal_child(GTK_REVEALER(revealer), FALSE);

    GtkWidget *scrolled = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled),
                                    GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    /* Cap the list's height so a long result doesn't make the dropdown
     * grow to fill the whole screen -- it scrolls instead. */
    gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(scrolled), 200);
    gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(scrolled), TRUE);

    GtkWidget *list_box = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(list_box), GTK_SELECTION_NONE);
    gtk_container_add(GTK_CONTAINER(scrolled), list_box);
    gtk_container_add(GTK_CONTAINER(revealer), scrolled);

    if (out_list_box != NULL)
        *out_list_box = list_box;

    return revealer;
}

/* Applies a layout to the pill grid. A pill in the layout goes to the
 * cell its position gives it (element i -> column i % columns, row
 * i / columns) and is shown; a pill that isn't in the layout is hidden,
 * and so takes no room at all. Pills are moved with child properties
 * and shown/hidden, never removed and re-added, so nothing is rebuilt
 * and the backends' handles on the pills' widgets stay valid. Two pills
 * may briefly share a cell while the loop is half-way through; GtkGrid
 * allows that.
 *
 * Every pill has no_show_all set (see extras_menu_popover_content_new())
 * so the gtk_widget_show_all() on the whole dropdown can't bring a
 * hidden one back; showing/hiding is done here, by hand, only. */
static void
extras_menu_apply_pill_order(GtkWidget *grid, const GPtrArray *layout)
{
    GList *children = gtk_container_get_children(GTK_CONTAINER(grid));

    for (GList *l = children; l != NULL; l = l->next)
    {
        GtkWidget *pill_widget = GTK_WIDGET(l->data);
        const gchar *id = g_object_get_data(G_OBJECT(pill_widget), PILL_ID_KEY);
        gint position = -1;

        for (guint i = 0; i < layout->len; i++)
        {
            const ExtrasMenuPillInfo *pill = g_ptr_array_index(layout, i);

            if (g_strcmp0(id, pill->id) == 0)
            {
                position = (gint) i;
                break;
            }
        }

        if (position >= 0)
        {
            gtk_container_child_set(GTK_CONTAINER(grid), pill_widget,
                                     "left-attach", position % EXTRAS_MENU_PILL_COLUMNS,
                                     "top-attach", position / EXTRAS_MENU_PILL_COLUMNS,
                                     NULL);
        }

        gtk_widget_set_visible(pill_widget, position >= 0);
    }

    g_list_free(children);

    /* With no pills at all the empty grid would still cost one row of
     * spacing in the dropdown, so hide it as well. */
    gtk_widget_set_visible(grid, layout->len > 0);
}

/* Applies a layout to the slider rows: the sliders in the layout are
 * shown, top to bottom in layout order; the ones not in it are hidden
 * (they keep existing and keep following their backends, they just
 * have no row on screen). Rows are reordered with
 * gtk_box_reorder_child(), never removed and re-added, so the scale
 * widgets the plugin holds on to stay valid. Like the pills, the rows
 * have no_show_all set, so the gtk_widget_show_all() on the whole
 * dropdown can't bring a hidden one back. */
static void
extras_menu_apply_slider_order(GtkWidget *box, const GPtrArray *layout)
{
    GList *children = gtk_container_get_children(GTK_CONTAINER(box));

    /* Shown rows first, in layout order. Going through the layout from
     * the top and putting each row at its final index means every
     * placement leaves the ones before it alone. */
    for (guint i = 0; i < layout->len; i++)
    {
        const ExtrasMenuSliderInfo *slider = g_ptr_array_index(layout, i);

        for (GList *l = children; l != NULL; l = l->next)
        {
            const gchar *id = g_object_get_data(G_OBJECT(l->data), SLIDER_ID_KEY);

            if (g_strcmp0(id, slider->id) == 0)
            {
                gtk_box_reorder_child(GTK_BOX(box), GTK_WIDGET(l->data), (gint) i);
                gtk_widget_show(GTK_WIDGET(l->data));
                break;
            }
        }
    }

    /* Then hide whatever the layout doesn't mention. */
    for (GList *l = children; l != NULL; l = l->next)
    {
        const gchar *id = g_object_get_data(G_OBJECT(l->data), SLIDER_ID_KEY);

        if (!extras_menu_slider_layout_contains(layout, id))
            gtk_widget_hide(GTK_WIDGET(l->data));
    }

    g_list_free(children);

    /* With no sliders at all the empty box would still cost one gap of
     * spacing in the dropdown, so hide it as well. */
    gtk_widget_set_visible(box, layout->len > 0);
}

void
extras_menu_popover_set_slider_order(GtkWidget *content, const GPtrArray *layout)
{
    if (content == NULL)
        return;

    GtkWidget *box = g_object_get_data(G_OBJECT(content), SLIDER_BOX_KEY);
    if (box != NULL)
        extras_menu_apply_slider_order(box, layout);
}

/* Turns the capsule look on or off by toggling one class on the grid;
 * the CSS above matches the pills through it, so every pill (including
 * the halves of the split ones) follows with no per-widget work. */
static void
extras_menu_apply_pill_shape(GtkWidget *grid, ExtrasMenuPillShape shape)
{
    GtkStyleContext *ctx = gtk_widget_get_style_context(grid);

    if (shape == EXTRAS_MENU_PILL_SHAPE_ROUND)
        gtk_style_context_add_class(ctx, "extras-menu-pills-round");
    else
        gtk_style_context_remove_class(ctx, "extras-menu-pills-round");
}

void
extras_menu_popover_set_pill_shape(GtkWidget *content, ExtrasMenuPillShape shape)
{
    if (content == NULL)
        return;

    GtkWidget *grid = g_object_get_data(G_OBJECT(content), PILL_GRID_KEY);
    if (grid != NULL)
        extras_menu_apply_pill_shape(grid, shape);
}

GtkWidget *
extras_menu_popover_get_pill(GtkWidget *content, const gchar *pill_id)
{
    if (content == NULL || pill_id == NULL)
        return NULL;

    GtkWidget *grid = g_object_get_data(G_OBJECT(content), PILL_GRID_KEY);
    if (grid == NULL)
        return NULL;

    GtkWidget *found = NULL;
    GList *children = gtk_container_get_children(GTK_CONTAINER(grid));

    for (GList *l = children; l != NULL; l = l->next)
    {
        const gchar *id = g_object_get_data(G_OBJECT(l->data), PILL_ID_KEY);

        if (g_strcmp0(id, pill_id) == 0)
        {
            found = GTK_WIDGET(l->data);
            break;
        }
    }

    g_list_free(children);
    return found;
}

void
extras_menu_popover_set_pill_order(GtkWidget *content, const GPtrArray *layout)
{
    if (content == NULL)
        return;

    GtkWidget *grid = g_object_get_data(G_OBJECT(content), PILL_GRID_KEY);
    if (grid != NULL)
        extras_menu_apply_pill_order(grid, layout);
}

gboolean
extras_menu_popover_get_microphone(GtkWidget *content, GtkWidget **scale,
                                    GtkWidget **mute_button, GtkWidget **icon)
{
    if (content == NULL)
        return FALSE;

    GtkWidget *box = g_object_get_data(G_OBJECT(content), SLIDER_BOX_KEY);
    if (box == NULL)
        return FALSE;

    GtkWidget *row = NULL;
    GList *children = gtk_container_get_children(GTK_CONTAINER(box));

    for (GList *l = children; l != NULL; l = l->next)
    {
        const gchar *id = g_object_get_data(G_OBJECT(l->data), SLIDER_ID_KEY);

        if (g_strcmp0(id, EXTRAS_MENU_SLIDER_MICROPHONE) == 0)
        {
            row = l->data;
            break;
        }
    }

    g_list_free(children);

    if (row == NULL)
        return FALSE;

    if (scale != NULL)
        *scale = g_object_get_data(G_OBJECT(row), MIC_SCALE_KEY);
    if (mute_button != NULL)
        *mute_button = g_object_get_data(G_OBJECT(row), MIC_BUTTON_KEY);
    if (icon != NULL)
        *icon = g_object_get_data(G_OBJECT(row), MIC_ICON_KEY);

    return TRUE;
}

GtkWidget *
extras_menu_popover_content_new(GtkWidget **battery_label, GtkWidget **battery_icon,
                                 GtkWidget **quick_actions_box,
                                 GtkWidget **volume_scale, GtkWidget **volume_icon,
                                 GtkWidget **brightness_scale, GtkWidget **bluetooth_toggle,
                                 GtkWidget **bluetooth_expand_button,
                                 GtkWidget **bluetooth_expand_chevron,
                                 GtkWidget **bluetooth_revealer, GtkWidget **bluetooth_list_box,
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

    /* --- top status bar: battery badge + quick-action button container --- */
    gtk_box_pack_start(GTK_BOX(root),
                        extras_menu_make_top_bar(battery_label, battery_icon, quick_actions_box),
                        FALSE, FALSE, 0);

    /* --- slider rows (volume, brightness) ---
     * Which of them the menu shows, and in which order, is a user
     * setting, so they live in a box of their own: the saved layout
     * reorders and hides the rows inside it without having to know what
     * else is in `root`. The box keeps the 8px gap the rows had when
     * they sat directly in `root`. */
    GtkWidget *sliders_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_box_pack_start(GTK_BOX(root), sliders_box, FALSE, FALSE, 0);

    GtkWidget *vol_scale = NULL;
    GtkWidget *vol_icon = NULL;
    GtkWidget *vol_row = extras_menu_make_slider_row("audio-volume-high-symbolic",
                                                      &vol_scale, &vol_icon);
    if (volume_scale != NULL)
        *volume_scale = vol_scale;
    if (volume_icon != NULL)
        *volume_icon = vol_icon;

    GtkWidget *bright_scale = NULL;
    GtkWidget *bright_row = extras_menu_make_slider_row("display-brightness-symbolic",
                                                         &bright_scale, NULL);
    if (brightness_scale != NULL)
        *brightness_scale = bright_scale;

    /* The microphone row hands its widgets out through
     * extras_menu_popover_get_microphone(), not through this
     * function's parameters. */
    GtkWidget *mic_row = extras_menu_make_microphone_row(NULL, NULL, NULL);

    const struct
    {
        GtkWidget *widget;
        const gchar *id;
    } slider_rows[] = {
        { vol_row,    EXTRAS_MENU_SLIDER_VOLUME     },
        { bright_row, EXTRAS_MENU_SLIDER_BRIGHTNESS },
        { mic_row,    EXTRAS_MENU_SLIDER_MICROPHONE },
    };

    for (guint i = 0; i < G_N_ELEMENTS(slider_rows); i++)
    {
        GtkWidget *slider_row = slider_rows[i].widget;

        g_object_set_data(G_OBJECT(slider_row), SLIDER_ID_KEY, (gpointer) slider_rows[i].id);
        gtk_box_pack_start(GTK_BOX(sliders_box), slider_row, FALSE, FALSE, 0);

        /* Same trick as the pills: show the row's insides now, once,
         * so that showing the row later only needs gtk_widget_show()
         * on the row, and no_show_all makes the show_all() at the end
         * of this function leave the row's visibility alone. */
        gtk_widget_show_all(slider_row);
        gtk_widget_set_no_show_all(slider_row, TRUE);
    }

    g_object_set_data(G_OBJECT(root), SLIDER_BOX_KEY, sliders_box);
    gtk_widget_set_no_show_all(sliders_box, TRUE);

    GPtrArray *slider_layout = extras_menu_slider_layout_load();
    extras_menu_apply_slider_order(sliders_box, slider_layout);
    g_ptr_array_unref(slider_layout);

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
    /* Initial look only, until the network backend reports the real
     * state: icon and label must agree with each other (a wired icon
     * next to a "Wi-Fi" label would show a contradiction for a moment
     * on every start), so both start out as Wi-Fi. network-ui.c swaps
     * them to the Ethernet look if that is what is actually active. */
    GtkWidget *network_pill = extras_menu_make_split_pill("network-wireless-symbolic", "Wi-Fi",
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
    GtkWidget *bluetooth_main_toggle = NULL;
    GtkWidget *bluetooth_expand = NULL;
    GtkWidget *bluetooth_chevron = NULL;
    GtkWidget *bluetooth = extras_menu_make_split_pill("bluetooth-symbolic", "Bluetooth",
                                                         &bluetooth_main_toggle, NULL, NULL,
                                                         &bluetooth_expand, &bluetooth_chevron,
                                                         NULL, NULL);
    if (bluetooth_toggle != NULL)
        *bluetooth_toggle = bluetooth_main_toggle;
    if (bluetooth_expand_button != NULL)
        *bluetooth_expand_button = bluetooth_expand;
    if (bluetooth_expand_chevron != NULL)
        *bluetooth_expand_chevron = bluetooth_chevron;

    GtkWidget *balanced = extras_menu_make_pill_toggle("weather-clear-symbolic", "Balanced", FALSE, FALSE);

    GtkWidget *dark_mode = extras_menu_make_pill_toggle("weather-clear-night-symbolic", "Dark Mode", FALSE, FALSE);
    GtkWidget *airplane = extras_menu_make_pill_toggle("airplane-mode-symbolic", "Airplane Mode", FALSE, FALSE);

    /* Which pills the menu shows, and in which cell each sits, is a
     * user setting. Here they are only tagged with their ids and
     * dropped into the grid (one per row for now); the saved layout
     * then moves each to its real cell, or hides it. */
    const struct
    {
        GtkWidget *widget;
        const gchar *id;
    } pill_widgets[] = {
        { network_pill, EXTRAS_MENU_PILL_NETWORK },
        { quality,   EXTRAS_MENU_PILL_QUALITY   },
        { bluetooth, EXTRAS_MENU_PILL_BLUETOOTH },
        { balanced,  EXTRAS_MENU_PILL_BALANCED  },
        { dark_mode, EXTRAS_MENU_PILL_DARK_MODE },
        { airplane,  EXTRAS_MENU_PILL_AIRPLANE  },
    };

    for (guint i = 0; i < G_N_ELEMENTS(pill_widgets); i++)
    {
        GtkWidget *pill_widget = pill_widgets[i].widget;

        g_object_set_data(G_OBJECT(pill_widget), PILL_ID_KEY, (gpointer) pill_widgets[i].id);
        gtk_grid_attach(GTK_GRID(grid), pill_widget, 0, (gint) i, 1, 1);

        /* A pill the user hasn't put in the menu stays hidden. Its
         * inner widgets are shown now, once, so that showing the pill
         * later only needs gtk_widget_show() on the pill itself; and
         * no_show_all makes the show_all() at the end of this function
         * leave the pill's visibility alone. */
        gtk_widget_show_all(pill_widget);
        gtk_widget_set_no_show_all(pill_widget, TRUE);
    }

    g_object_set_data(G_OBJECT(root), PILL_GRID_KEY, grid);
    gtk_widget_set_no_show_all(grid, TRUE);

    GPtrArray *pill_layout = extras_menu_pill_layout_load();
    extras_menu_apply_pill_order(grid, pill_layout);
    g_ptr_array_unref(pill_layout);

    extras_menu_apply_pill_shape(grid, extras_menu_pill_layout_get_shape());

    gtk_box_pack_start(GTK_BOX(root), grid, FALSE, FALSE, 0);

    /* --- Wi-Fi network list and Bluetooth device list, hidden by
     * default, each revealed by the chevron on its own pill above. The
     * caller keeps at most one of them open at a time. --- */
    GtkWidget *wifi_list = NULL;
    GtkWidget *wifi_revealer = extras_menu_make_list_revealer(&wifi_list);
    gtk_box_pack_start(GTK_BOX(root), wifi_revealer, FALSE, FALSE, 0);

    if (network_revealer != NULL)
        *network_revealer = wifi_revealer;
    if (network_list_box != NULL)
        *network_list_box = wifi_list;

    GtkWidget *bluetooth_list = NULL;
    GtkWidget *bluetooth_reveal = extras_menu_make_list_revealer(&bluetooth_list);
    gtk_box_pack_start(GTK_BOX(root), bluetooth_reveal, FALSE, FALSE, 0);

    if (bluetooth_revealer != NULL)
        *bluetooth_revealer = bluetooth_reveal;
    if (bluetooth_list_box != NULL)
        *bluetooth_list_box = bluetooth_list;

    gtk_widget_show_all(root);
    /* The revealer's child (scrolled/list_box) was just made visible
     * by show_all above, but the revealer itself stays collapsed
     * (0-height) until reveal-child is set TRUE -- show_all does not
     * override that, it only affects widget visibility, not the
     * revealer's own reveal state. */
    return root;
}