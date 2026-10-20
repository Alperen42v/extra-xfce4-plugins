#ifndef EXTRAS_MENU_SLIDER_LAYOUT_CONFIG_H
#define EXTRAS_MENU_SLIDER_LAYOUT_CONFIG_H

#include <glib.h>

G_BEGIN_DECLS

/* How many sliders exist in total (the catalog the user can pick from).
 * The menu shows any subset of them, in any order, so a layout is never
 * longer than this. Must match the table in slider-layout-config.c
 * (checked at compile time). */
#define EXTRAS_MENU_SLIDER_COUNT 3

/* The stable ids of the sliders. These are what is written to the
 * config file, so never rename one -- a user's saved layout would stop
 * matching it. */
#define EXTRAS_MENU_SLIDER_VOLUME     "volume"
#define EXTRAS_MENU_SLIDER_BRIGHTNESS "brightness"
#define EXTRAS_MENU_SLIDER_MICROPHONE "microphone"

/* What the preferences editor needs to show a slider. The structs live
 * in a static table: pointers to them never need freeing. */
typedef struct
{
    const gchar *id;          /* one of the EXTRAS_MENU_SLIDER_* ids above */
    const gchar *title;       /* name shown in the editor */
    const gchar *icon;        /* theme icon name shown in the editor */
    const gchar *description; /* one line for the "add a slider" window */
} ExtrasMenuSliderInfo;

/* The catalog: every slider that exists, in default order, whether or
 * not the menu currently shows it. `index` must be below
 * EXTRAS_MENU_SLIDER_COUNT. This is what the "add a slider" window
 * lists. */
const ExtrasMenuSliderInfo *extras_menu_slider_catalog_get(guint index);

/*
 * A layout is the list of sliders the menu shows, top to bottom: a
 * GPtrArray of const ExtrasMenuSliderInfo*. It holds each slider at
 * most once, and may be empty -- a slider that isn't in the layout isn't
 * shown (it keeps working in the background; it just has no row). The
 * elements point into the static table, so the array has no free
 * function: release it with g_ptr_array_unref() and nothing else.
 */

/* Every slider, in the order the plugin shipped with (volume,
 * brightness, then microphone). */
GPtrArray *extras_menu_slider_layout_defaults(void);

/* Whether the layout shows the slider with this id. */
gboolean extras_menu_slider_layout_contains(const GPtrArray *layout, const gchar *id);

/* Loads the saved layout from
 * ~/.config/xfce4/extras-menu/slider-layout.ini (or wherever
 * XDG_CONFIG_HOME points). Falls back to the defaults (every slider) if
 * the file doesn't exist, can't be read or has no slider list at all. A
 * file whose list is empty is respected as "the user removed every
 * slider". Unknown ids and repeats in the file are ignored. A slider
 * the list leaves out is simply not shown -- including one added in a
 * later version of the plugin, which the user can then add from the
 * "add a slider" window. */
GPtrArray *extras_menu_slider_layout_load(void);

/* Writes the layout (at most EXTRAS_MENU_SLIDER_COUNT entries; an empty
 * one is fine) and, on success, calls every registered listener. */
gboolean extras_menu_slider_layout_save(const GPtrArray *layout, GError **error);

/* Change notification, so the dropdown can move its sliders the moment
 * the preferences editor saves. Listeners run in the main thread right
 * after a successful save. Remove a listener before whatever user_data
 * points to goes away. */
typedef void (*ExtrasMenuSliderLayoutChangedFunc)(gpointer user_data);

guint extras_menu_slider_layout_add_listener(ExtrasMenuSliderLayoutChangedFunc func,
                                              gpointer user_data);
void extras_menu_slider_layout_remove_listener(guint listener_id);

G_END_DECLS

#endif /* EXTRAS_MENU_SLIDER_LAYOUT_CONFIG_H */