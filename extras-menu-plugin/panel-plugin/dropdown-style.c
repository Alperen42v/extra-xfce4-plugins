/*
 * The dropdown's background: the panel's own color and transparency.
 * See dropdown-style.h.
 *
 * A panel plugin lives inside a small window of its own (the wrapper's
 * "plug"), which the panel keeps painted like the panel itself: with the
 * solid color from the panel's settings when it has one, otherwise with
 * what the theme says for a panel. So reading that window's background
 * color tells what the panel looks like behind the plugin, without having
 * to find and parse the panel's settings.
 */

#include "dropdown-style.h"

/* The lists are this much darker than the dropdown itself
 * (see shade() in the CSS: 0.7 = 30% darker). */
#define LIST_SHADE "0.7"

/* Below this the panel background is taken to be "none": a menu painted
 * with it would be unreadable over other windows, so the theme's color is
 * used instead. Also what a background image shows up as, since an image
 * leaves the color transparent. */
#define MIN_USABLE_ALPHA 0.1

struct _ExtrasMenuDropdownStyle
{
    GtkCssProvider *provider; /* holds the colors; added to the widgets below */
    gchar *css;               /* what the provider holds now */

    GtkWidget *anchor;        /* the plugin widget: finds the window it lives in */
    GtkWidget *popover;       /* the dropdown window */

    GtkWidget *toplevel;      /* the plugin's window whose style is watched (weak) */
    gulong style_updated_id;  /* ... on `toplevel` */
    gulong hierarchy_id;      /* on `anchor`: the plugin was put into its window */
    gulong show_id;           /* on `popover` */
};

/* --- reading the panel's color ------------------------------------------------------ */

/* The background color of the window the plugin lives in. FALSE if it
 * isn't in one yet, or the color is too transparent to use. */
static gboolean
panel_background_color(GtkWidget *anchor, GdkRGBA *out)
{
    GtkWidget *toplevel = gtk_widget_get_toplevel(anchor);

    if (toplevel == NULL || !gtk_widget_is_toplevel(toplevel))
        return FALSE;

    GdkRGBA *rgba = NULL;
    gtk_style_context_get(gtk_widget_get_style_context(toplevel), GTK_STATE_FLAG_NORMAL,
                          GTK_STYLE_PROPERTY_BACKGROUND_COLOR, &rgba, NULL);
    if (rgba == NULL)
        return FALSE;

    gboolean usable = rgba->alpha >= MIN_USABLE_ALPHA;
    if (usable)
        *out = *rgba;

    gdk_rgba_free(rgba);
    return usable;
}

/* The stylesheet for a background color, or for the theme's own when
 * `color` is NULL. The dropdown's box and its lists are the only things
 * this touches. */
static gchar *
build_css(const GdkRGBA *color)
{
    gchar *value = color != NULL ? gdk_rgba_to_string(color) : g_strdup("@theme_bg_color");
    gchar *css = g_strdup_printf(".extras-menu-popover { background-color: %s; }"
                                 ".extras-menu-list { background-color: shade(%s, " LIST_SHADE "); }",
                                 value, value);

    g_free(value);
    return css;
}

/* Re-reads the panel's color and, if it changed, restyles the dropdown. */
static void
update(ExtrasMenuDropdownStyle *style)
{
    GdkRGBA rgba;
    gboolean have_color = panel_background_color(style->anchor, &rgba);
    gchar *css = build_css(have_color ? &rgba : NULL);

    /* Nothing new: leave the stylesheet alone, so a style change that
     * has nothing to do with the background costs next to nothing. */
    if (g_strcmp0(css, style->css) == 0)
    {
        g_free(css);
        return;
    }

    g_free(style->css);
    style->css = css;
    gtk_css_provider_load_from_data(style->provider, css, -1, NULL);
}

/* --- following the panel ------------------------------------------------------------- */

/* The window's style changed: the panel's background setting or the
 * theme did. */
static void
on_toplevel_style_updated(GtkWidget *widget, gpointer user_data)
{
    (void) widget;
    update(user_data);
}

static void
unwatch_toplevel(ExtrasMenuDropdownStyle *style)
{
    if (style->toplevel == NULL)
        return;

    g_signal_handler_disconnect(style->toplevel, style->style_updated_id);
    g_object_remove_weak_pointer(G_OBJECT(style->toplevel), (gpointer *) &style->toplevel);
    style->toplevel = NULL;
    style->style_updated_id = 0;
}

/* Starts watching the window the plugin lives in, if it is in one now
 * and isn't watched already. */
static void
watch_toplevel(ExtrasMenuDropdownStyle *style)
{
    GtkWidget *toplevel = gtk_widget_get_toplevel(style->anchor);

    if (toplevel == NULL || !gtk_widget_is_toplevel(toplevel) || toplevel == style->toplevel)
        return;

    unwatch_toplevel(style);

    style->toplevel = toplevel;
    g_object_add_weak_pointer(G_OBJECT(toplevel), (gpointer *) &style->toplevel);
    style->style_updated_id = g_signal_connect(toplevel, "style-updated",
                                               G_CALLBACK(on_toplevel_style_updated), style);
}

/* The plugin was put into its window (this happens after construct()). */
static void
on_anchor_hierarchy_changed(GtkWidget *widget, GtkWidget *previous_toplevel, gpointer user_data)
{
    (void) widget;
    (void) previous_toplevel;

    watch_toplevel(user_data);
    update(user_data);
}

/* A last look every time the dropdown opens, in case a change slipped
 * past (the window not being watched yet, say). Cheap when nothing changed. */
static void
on_popover_show(GtkWidget *widget, gpointer user_data)
{
    (void) widget;

    watch_toplevel(user_data);
    update(user_data);
}

/* --- setup and teardown ----------------------------------------------------------------- */

static void
add_provider_to(ExtrasMenuDropdownStyle *style, GtkWidget *widget)
{
    if (widget == NULL)
        return;

    gtk_style_context_add_provider(gtk_widget_get_style_context(widget),
                                   GTK_STYLE_PROVIDER(style->provider),
                                   GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
}

/* The part that doesn't depend on the plugin struct: `content` is the
 * dropdown's root box and the lists the scrolled windows of the Wi-Fi and
 * Bluetooth lists. A stylesheet added to a widget only reaches that
 * widget, not its children, so each of these gets it directly. Any of
 * the lists may be NULL. */
static ExtrasMenuDropdownStyle *
style_new(GtkWidget *anchor, GtkWidget *popover, GtkWidget *content,
          GtkWidget *wifi_list, GtkWidget *bluetooth_list)
{
    ExtrasMenuDropdownStyle *style = g_new0(ExtrasMenuDropdownStyle, 1);

    style->anchor = anchor;
    style->popover = popover;
    style->provider = gtk_css_provider_new();

    add_provider_to(style, content);
    add_provider_to(style, wifi_list);
    add_provider_to(style, bluetooth_list);

    /* Right away, with whatever is known now: the theme's color if the
     * plugin isn't in its window yet. */
    update(style);
    watch_toplevel(style);

    style->hierarchy_id = g_signal_connect(anchor, "hierarchy-changed",
                                           G_CALLBACK(on_anchor_hierarchy_changed), style);
    style->show_id = g_signal_connect(popover, "show", G_CALLBACK(on_popover_show), style);

    return style;
}

static void
style_free(ExtrasMenuDropdownStyle *style)
{
    unwatch_toplevel(style);
    g_signal_handler_disconnect(style->anchor, style->hierarchy_id);
    g_signal_handler_disconnect(style->popover, style->show_id);

    g_object_unref(style->provider);
    g_free(style->css);
    g_free(style);
}

/* The scrolled window inside a list's revealer, or NULL. */
static GtkWidget *
list_of(GtkWidget *revealer)
{
    return revealer != NULL ? gtk_bin_get_child(GTK_BIN(revealer)) : NULL;
}

void
extras_menu_dropdown_style_attach(ExtrasMenuPlugin *plugin)
{
    if (plugin->popover == NULL || plugin->dropdown_style != NULL)
        return;

    GtkWidget *content = gtk_bin_get_child(GTK_BIN(plugin->popover));
    if (content == NULL)
        return;

    plugin->dropdown_style = style_new(GTK_WIDGET(plugin), plugin->popover, content,
                                       list_of(plugin->network_revealer),
                                       list_of(plugin->bluetooth_revealer));
}

void
extras_menu_dropdown_style_free(ExtrasMenuPlugin *plugin)
{
    if (plugin->dropdown_style == NULL)
        return;

    style_free(plugin->dropdown_style);
    plugin->dropdown_style = NULL;
}