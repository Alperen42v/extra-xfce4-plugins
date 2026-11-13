#include "caffeine-config.h"

#include <glib/gstdio.h>

#define CONFIG_GROUP "caffeine"
#define DURATION_KEY "duration-minutes"

static gchar *
config_file_path(void)
{
    return g_build_filename(g_get_user_config_dir(), "xfce4", "extras-menu",
                            "caffeine.ini", NULL);
}

guint
extras_menu_caffeine_config_get_duration(void)
{
    guint minutes = EXTRAS_MENU_CAFFEINE_UNLIMITED;
    gchar *path = config_file_path();
    GKeyFile *file = g_key_file_new();

    if (g_key_file_load_from_file(file, path, G_KEY_FILE_NONE, NULL))
    {
        /* A missing or unreadable key gives 0, which is "unlimited" too;
         * so does a negative number. */
        gint value = g_key_file_get_integer(file, CONFIG_GROUP, DURATION_KEY, NULL);

        if (value > 0)
            minutes = MIN((guint) value, (guint) EXTRAS_MENU_CAFFEINE_MAX_MINUTES);
    }

    g_key_file_free(file);
    g_free(path);
    return minutes;
}

/* --- change listeners --------------------------------------------------------- */

typedef struct
{
    guint id;
    ExtrasMenuCaffeineConfigChangedFunc func;
    gpointer user_data;
} Listener;

static GSList *listeners = NULL;
static guint next_listener_id = 1;

guint
extras_menu_caffeine_config_add_listener(ExtrasMenuCaffeineConfigChangedFunc func,
                                          gpointer user_data)
{
    Listener *listener = g_new0(Listener, 1);
    listener->id = next_listener_id++;
    listener->func = func;
    listener->user_data = user_data;

    listeners = g_slist_append(listeners, listener);
    return listener->id;
}

void
extras_menu_caffeine_config_remove_listener(guint listener_id)
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
extras_menu_caffeine_config_set_duration(guint minutes, GError **error)
{
    minutes = MIN(minutes, (guint) EXTRAS_MENU_CAFFEINE_MAX_MINUTES);

    if (extras_menu_caffeine_config_get_duration() == minutes)
        return TRUE;

    gchar *path = config_file_path();
    GKeyFile *file = g_key_file_new();

    /* Start from what's on disk so any other key in the file is kept;
     * a missing file just means we start empty. */
    g_key_file_load_from_file(file, path, G_KEY_FILE_NONE, NULL);
    g_key_file_set_integer(file, CONFIG_GROUP, DURATION_KEY, (gint) minutes);

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
