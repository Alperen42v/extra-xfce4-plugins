#include "night-light-config.h"

#include <glib/gstdio.h>

#define CONFIG_GROUP     "night-light"
#define TEMPERATURE_KEY  "temperature"
#define TRANSITION_KEY   "transition-ms"

static gchar *
config_file_path(void)
{
    return g_build_filename(g_get_user_config_dir(), "xfce4", "extras-menu",
                            "night-light.ini", NULL);
}

/* Loads the file, or returns NULL (no file yet is the normal first-run
 * case and not worth a log line; anything else is). */
static GKeyFile *
load_file(void)
{
    gchar *path = config_file_path();
    GKeyFile *file = g_key_file_new();
    GError *error = NULL;

    if (!g_key_file_load_from_file(file, path, G_KEY_FILE_NONE, &error))
    {
        if (!g_error_matches(error, G_FILE_ERROR, G_FILE_ERROR_NOENT))
        {
            g_warning("extras-menu: couldn't read %s: %s -- using the default Night Light settings",
                      path, error->message);
        }
        g_clear_error(&error);
        g_key_file_free(file);
        file = NULL;
    }

    g_free(path);
    return file;
}

static guint
get_value(const gchar *key, gint minimum, gint maximum, gint fallback)
{
    GKeyFile *file = load_file();
    gint value = fallback;

    if (file != NULL)
    {
        GError *error = NULL;
        gint read = g_key_file_get_integer(file, CONFIG_GROUP, key, &error);

        if (error == NULL)
            value = read;
        g_clear_error(&error);
        g_key_file_free(file);
    }

    return (guint) CLAMP(value, minimum, maximum);
}

guint
extras_menu_night_light_config_get_temperature(void)
{
    return get_value(TEMPERATURE_KEY,
                     EXTRAS_MENU_NIGHT_LIGHT_TEMPERATURE_MIN,
                     EXTRAS_MENU_NIGHT_LIGHT_TEMPERATURE_MAX,
                     EXTRAS_MENU_NIGHT_LIGHT_TEMPERATURE_DEFAULT);
}

guint
extras_menu_night_light_config_get_transition_ms(void)
{
    return get_value(TRANSITION_KEY,
                     EXTRAS_MENU_NIGHT_LIGHT_TRANSITION_MIN,
                     EXTRAS_MENU_NIGHT_LIGHT_TRANSITION_MAX,
                     EXTRAS_MENU_NIGHT_LIGHT_TRANSITION_DEFAULT);
}

/* --- change listeners --------------------------------------------------------- */

typedef struct
{
    guint id;
    ExtrasMenuNightLightConfigChangedFunc func;
    gpointer user_data;
} Listener;

static GSList *listeners = NULL;
static guint next_listener_id = 1;

guint
extras_menu_night_light_config_add_listener(ExtrasMenuNightLightConfigChangedFunc func,
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
extras_menu_night_light_config_remove_listener(guint listener_id)
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
extras_menu_night_light_config_save(guint temperature, guint transition_ms, GError **error)
{
    temperature = CLAMP(temperature,
                        (guint) EXTRAS_MENU_NIGHT_LIGHT_TEMPERATURE_MIN,
                        (guint) EXTRAS_MENU_NIGHT_LIGHT_TEMPERATURE_MAX);
    transition_ms = MIN(transition_ms, (guint) EXTRAS_MENU_NIGHT_LIGHT_TRANSITION_MAX);

    GKeyFile *file = g_key_file_new();
    g_key_file_set_integer(file, CONFIG_GROUP, TEMPERATURE_KEY, (gint) temperature);
    g_key_file_set_integer(file, CONFIG_GROUP, TRANSITION_KEY, (gint) transition_ms);

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