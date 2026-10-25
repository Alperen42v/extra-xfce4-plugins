#include "pill-layout-config.h"

#include <glib/gstdio.h>

#define CONFIG_GROUP "pill-layout"
#define ORDER_KEY    "order"
#define SHAPE_KEY    "shape"

/* The catalog of every pill, in default order. Adding a pill = one line
 * here (plus its id in the header, EXTRAS_MENU_PILL_COUNT, and building
 * the widget in popover.c). */
static const ExtrasMenuPillInfo pills[] = {
    { EXTRAS_MENU_PILL_NETWORK,   "Wi-Fi / Ethernet", "network-wireless-symbolic",
      "Turns Wi-Fi on or off; the arrow lists nearby networks." },
    { EXTRAS_MENU_PILL_QUALITY,   "Quality",          "preferences-system-symbolic",
      "Placeholder button -- doesn't do anything yet." },
    { EXTRAS_MENU_PILL_BLUETOOTH, "Bluetooth",        "bluetooth-symbolic",
      "Turns Bluetooth on or off; the arrow lists devices." },
    { EXTRAS_MENU_PILL_BALANCED,  "Balanced",         "weather-clear-symbolic",
      "Shows the power profile; click to switch between Power Saver, Balanced and Performance." },
    { EXTRAS_MENU_PILL_DARK_MODE, "Dark Mode",        "weather-clear-night-symbolic",
      "Not functional yet -- only shown for layout." },
    { EXTRAS_MENU_PILL_AIRPLANE,  "Airplane Mode",   "airplane-mode-symbolic",
      "Turns Wi-Fi and Bluetooth off together, and back on again." },
};

G_STATIC_ASSERT(G_N_ELEMENTS(pills) == EXTRAS_MENU_PILL_COUNT);

/* Index of the pill with this id in the table, or -1. */
static gint
pill_index_for_id(const gchar *id)
{
    for (guint i = 0; i < G_N_ELEMENTS(pills); i++)
    {
        if (g_strcmp0(pills[i].id, id) == 0)
            return (gint) i;
    }

    return -1;
}

const ExtrasMenuPillInfo *
extras_menu_pill_catalog_get(guint index)
{
    g_return_val_if_fail(index < G_N_ELEMENTS(pills), NULL);
    return &pills[index];
}

gboolean
extras_menu_pill_layout_contains(const GPtrArray *layout, const gchar *id)
{
    for (guint i = 0; i < layout->len; i++)
    {
        const ExtrasMenuPillInfo *pill = g_ptr_array_index(layout, i);

        if (g_strcmp0(pill->id, id) == 0)
            return TRUE;
    }

    return FALSE;
}

/* --- defaults / load / save --------------------------------------------------- */

GPtrArray *
extras_menu_pill_layout_defaults(void)
{
    GPtrArray *layout = g_ptr_array_new();

    for (guint i = 0; i < G_N_ELEMENTS(pills); i++)
        g_ptr_array_add(layout, (gpointer) &pills[i]);

    return layout;
}

static gchar *
config_file_path(void)
{
    return g_build_filename(g_get_user_config_dir(), "xfce4", "extras-menu",
                            "pill-layout.ini", NULL);
}

GPtrArray *
extras_menu_pill_layout_load(void)
{
    GPtrArray *layout = g_ptr_array_new();
    gboolean seen[EXTRAS_MENU_PILL_COUNT] = { FALSE };
    gboolean configured = FALSE; /* the file has a pill list (possibly empty) */

    gchar *path = config_file_path();
    GKeyFile *file = g_key_file_new();
    GError *error = NULL;

    if (g_key_file_load_from_file(file, path, G_KEY_FILE_NONE, &error))
    {
        gsize length = 0;
        gchar **ids = g_key_file_get_string_list(file, CONFIG_GROUP, ORDER_KEY, &length, NULL);

        /* NULL means there is no such key; an empty list is a real
         * answer ("no pills") and must not fall back to the defaults. */
        configured = (ids != NULL);

        for (gsize i = 0; ids != NULL && i < length; i++)
        {
            gint index = pill_index_for_id(ids[i]);

            if (index >= 0 && !seen[index])
            {
                seen[index] = TRUE;
                g_ptr_array_add(layout, (gpointer) &pills[index]);
            }
        }

        g_strfreev(ids);
    }
    else
    {
        /* No file yet is the normal first-run case; anything else is
         * worth a log line, but either way the plugin still works. */
        if (!g_error_matches(error, G_FILE_ERROR, G_FILE_ERROR_NOENT))
        {
            g_warning("extras-menu: couldn't read %s: %s -- using the default pill order",
                      path, error->message);
        }
        g_clear_error(&error);
    }

    /* Nothing saved yet: show every pill. */
    if (!configured)
    {
        for (guint i = 0; i < G_N_ELEMENTS(pills); i++)
            g_ptr_array_add(layout, (gpointer) &pills[i]);
    }

    g_key_file_free(file);
    g_free(path);
    return layout;
}

/* --- pill shape ---------------------------------------------------------------- */

static const gchar *
shape_to_string(ExtrasMenuPillShape shape)
{
    return shape == EXTRAS_MENU_PILL_SHAPE_ROUND ? "round" : "default";
}

ExtrasMenuPillShape
extras_menu_pill_layout_get_shape(void)
{
    ExtrasMenuPillShape shape = EXTRAS_MENU_PILL_SHAPE_DEFAULT;
    gchar *path = config_file_path();
    GKeyFile *file = g_key_file_new();

    if (g_key_file_load_from_file(file, path, G_KEY_FILE_NONE, NULL))
    {
        gchar *value = g_key_file_get_string(file, CONFIG_GROUP, SHAPE_KEY, NULL);

        /* Anything other than "round" (missing, misspelt) is the default. */
        if (g_strcmp0(value, "round") == 0)
            shape = EXTRAS_MENU_PILL_SHAPE_ROUND;

        g_free(value);
    }

    g_key_file_free(file);
    g_free(path);
    return shape;
}

/* --- change listeners --------------------------------------------------------- */

typedef struct
{
    guint id;
    ExtrasMenuPillLayoutChangedFunc func;
    gpointer user_data;
} Listener;

static GSList *listeners = NULL;
static guint next_listener_id = 1;

guint
extras_menu_pill_layout_add_listener(ExtrasMenuPillLayoutChangedFunc func, gpointer user_data)
{
    Listener *listener = g_new0(Listener, 1);
    listener->id = next_listener_id++;
    listener->func = func;
    listener->user_data = user_data;

    listeners = g_slist_append(listeners, listener);
    return listener->id;
}

void
extras_menu_pill_layout_remove_listener(guint listener_id)
{
    for (GSList *l = listeners; l != NULL; l = l->next)
    {
        Listener *listener = l->data;
        if (listener->id == listener_id)
        {
            listeners = g_slist_remove(listeners, listener);
            g_free(listener);
            return;
        }
    }
}

static void
notify_listeners(void)
{
    /* Walk a snapshot of the ids, so a listener that adds or removes
     * listeners from inside its callback can't invalidate the walk; an
     * id removed in the meantime is simply skipped. */
    GArray *ids = g_array_new(FALSE, FALSE, sizeof(guint));
    for (GSList *l = listeners; l != NULL; l = l->next)
        g_array_append_val(ids, ((Listener *) l->data)->id);

    for (guint i = 0; i < ids->len; i++)
    {
        guint id = g_array_index(ids, guint, i);

        for (GSList *l = listeners; l != NULL; l = l->next)
        {
            Listener *listener = l->data;
            if (listener->id == id)
            {
                listener->func(listener->user_data);
                break;
            }
        }
    }

    g_array_free(ids, TRUE);
}

gboolean
extras_menu_pill_layout_save(const GPtrArray *layout, GError **error)
{
    const gchar *ids[EXTRAS_MENU_PILL_COUNT];
    guint count = MIN(layout->len, (guint) EXTRAS_MENU_PILL_COUNT);

    for (guint i = 0; i < count; i++)
    {
        const ExtrasMenuPillInfo *pill = g_ptr_array_index(layout, i);
        ids[i] = pill->id;
    }

    GKeyFile *file = g_key_file_new();
    g_key_file_set_string_list(file, CONFIG_GROUP, ORDER_KEY, ids, count);

    /* This file is rebuilt from scratch on every save, so the shape
     * (which the pill list knows nothing about) is carried over. */
    g_key_file_set_string(file, CONFIG_GROUP, SHAPE_KEY,
                          shape_to_string(extras_menu_pill_layout_get_shape()));

    gchar *path = config_file_path();
    gchar *dir = g_path_get_dirname(path);
    g_mkdir_with_parents(dir, 0700);

    gboolean ok = g_key_file_save_to_file(file, path, error);

    g_free(dir);
    g_free(path);
    g_key_file_free(file);

    if (ok)
        notify_listeners();

    return ok;
}

gboolean
extras_menu_pill_layout_set_shape(ExtrasMenuPillShape shape, GError **error)
{
    if (extras_menu_pill_layout_get_shape() == shape)
        return TRUE;

    gchar *path = config_file_path();
    GKeyFile *file = g_key_file_new();

    /* Start from what's on disk so the pill list is kept as it is (an
     * empty one included); a missing file just means we start empty,
     * and load() then still shows every pill, because there is no
     * "order" key. */
    g_key_file_load_from_file(file, path, G_KEY_FILE_NONE, NULL);
    g_key_file_set_string(file, CONFIG_GROUP, SHAPE_KEY, shape_to_string(shape));

    gchar *dir = g_path_get_dirname(path);
    g_mkdir_with_parents(dir, 0700);

    gboolean ok = g_key_file_save_to_file(file, path, error);

    g_free(dir);
    g_free(path);
    g_key_file_free(file);

    if (ok)
        notify_listeners();

    return ok;
}