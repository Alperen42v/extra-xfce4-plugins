#ifndef EXTRAS_MENU_PILL_LAYOUT_CONFIG_H
#define EXTRAS_MENU_PILL_LAYOUT_CONFIG_H

#include <glib.h>

G_BEGIN_DECLS

/* The dropdown's big buttons ("pills") sit in a grid this many columns
 * wide, filled left to right, top to bottom. */
#define EXTRAS_MENU_PILL_COLUMNS 2

/* How many pills exist in total (the catalog the user can pick from).
 * The menu shows any subset of them, so a layout is never longer than
 * this. Must match the table in pill-layout-config.c (checked at
 * compile time). */
#define EXTRAS_MENU_PILL_COUNT 6

/* The stable ids of the pills. These are what is written to the config
 * file, so never rename one -- a user's saved layout would stop
 * matching it. */
#define EXTRAS_MENU_PILL_NETWORK   "network"
#define EXTRAS_MENU_PILL_QUALITY   "quality"
#define EXTRAS_MENU_PILL_BLUETOOTH "bluetooth"
#define EXTRAS_MENU_PILL_BALANCED  "balanced"
#define EXTRAS_MENU_PILL_DARK_MODE "dark-mode"
#define EXTRAS_MENU_PILL_AIRPLANE  "airplane"

/* What the preferences editor needs to show a pill. The structs live in
 * a static table: pointers to them never need freeing. */
typedef struct
{
    const gchar *id;     /* one of the EXTRAS_MENU_PILL_* ids above */
    const gchar *title;  /* name shown in the editor */
    const gchar *icon;   /* theme icon name shown in the editor */
    const gchar *description; /* one line for the "add a pill" window */
} ExtrasMenuPillInfo;

/* The catalog: every pill that exists, in default order, whether or not
 * the menu currently shows it. `index` must be below
 * EXTRAS_MENU_PILL_COUNT. This is what the "add a pill" window lists. */
const ExtrasMenuPillInfo *extras_menu_pill_catalog_get(guint index);

/*
 * A layout is the list of pills the menu shows: a GPtrArray of const
 * ExtrasMenuPillInfo*, one per grid cell in reading order (element 0 is
 * the top-left cell, element 1 the one to its right, element 2 the left
 * cell of the second row, and so on). It holds each pill at most once,
 * and may be empty -- a pill that isn't in the layout isn't shown.
 * The elements point into the static table, so the array has no free
 * function: release it with g_ptr_array_unref() and nothing else.
 */

/* Every pill, in the order the plugin shipped with (Wi-Fi, quality,
 * Bluetooth, balanced, dark mode, aeroplane mode). */
GPtrArray *extras_menu_pill_layout_defaults(void);

/* Whether the layout shows the pill with this id. */
gboolean extras_menu_pill_layout_contains(const GPtrArray *layout, const gchar *id);

/* Loads the saved layout from
 * ~/.config/xfce4/extras-menu/pill-layout.ini (or wherever
 * XDG_CONFIG_HOME points). Falls back to the defaults (every pill) if
 * the file doesn't exist, can't be read or has no pill list at all. A
 * file whose list is empty is respected as "the user removed every
 * pill". Unknown ids and repeats in the file are ignored. A pill the
 * list leaves out is simply not shown -- including one added in a later
 * version of the plugin, which the user can then add from the "add a
 * pill" window. */
GPtrArray *extras_menu_pill_layout_load(void);

/* Writes the layout (at most EXTRAS_MENU_PILL_COUNT entries; an empty
 * one is fine) and, on success, calls every registered listener. */
gboolean extras_menu_pill_layout_save(const GPtrArray *layout, GError **error);

/* Change notification, so the dropdown can move its pills the moment
 * the preferences editor saves. Listeners run in the main thread right
 * after a successful save. Remove a listener before whatever user_data
 * points to goes away. */
typedef void (*ExtrasMenuPillLayoutChangedFunc)(gpointer user_data);

guint extras_menu_pill_layout_add_listener(ExtrasMenuPillLayoutChangedFunc func,
                                            gpointer user_data);
void extras_menu_pill_layout_remove_listener(guint listener_id);

G_END_DECLS

#endif /* EXTRAS_MENU_PILL_LAYOUT_CONFIG_H */