#include "caffeine-config.h"

#include <glib/gstdio.h>

#define CONFIG_GROUP "caffeine"
#define DURATION_KEY "duration-minutes"
#define SCREEN_OFF_ENABLED_KEY "screen-off-enabled"
#define SCREEN_OFF_MINUTES_KEY "screen-off-minutes"

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

gboolean
extras_menu_caffeine_config_get_screen_off_enabled(void)
{
    gboolean enabled = FALSE;
    gchar *path = config_file_path();
    GKeyFile *file = g_key_file_new();

    if (g_key_file_load_from_file(file, path, G_KEY_FILE_NONE, NULL))
    {
        /* A missing or malformed key gives FALSE. */
        enabled = g_key_file_get_boolean(file, CONFIG_GROUP, SCREEN_OFF_ENABLED_KEY, NULL);
    }

    g_key_file_free(file);
    g_free(path);
    return enabled;
}

guint
extras_menu_caffeine_config_get_screen_off_minutes(void)
{
    guint minutes = EXTRAS_MENU_CAFFEINE_SCREEN_OFF_DEFAULT_MINUTES;
    gchar *path = config_file_path();
    GKeyFile *file = g_key_file_new();

    if (g_key_file_load_from_file(file, path, G_KEY_FILE_NONE, NULL))
    {
        gint value = g_key_file_get_integer(file, CONFIG_GROUP, SCREEN_OFF_MINUTES_KEY, NULL);

        if (value > 0)
            minutes = MIN((guint) value, (guint) EXTRAS_MENU_CAFFEINE_SCREEN_OFF_MAX_MINUTES);
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

/* Loads the file as it is on disk (a missing file just gives an empty
 * one), lets `change` set what is to be saved, writes it back and, on
 * success, tells the listeners. Starting from what's on disk keeps every
 * key the change doesn't touch. */
typedef void (*ChangeFunc)(GKeyFile *file, gpointer data);

static gboolean
update_file(ChangeFunc change, gpointer data, GError **error)
{
    gchar *path = config_file_path();
    GKeyFile *file = g_key_file_new();

    g_key_file_load_from_file(file, path, G_KEY_FILE_NONE, NULL);
    change(file, data);

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

static void
change_duration(GKeyFile *file, gpointer data)
{
    g_key_file_set_integer(file, CONFIG_GROUP, DURATION_KEY, GPOINTER_TO_INT(data));
}

gboolean
extras_menu_caffeine_config_set_duration(guint minutes, GError **error)
{
    minutes = MIN(minutes, (guint) EXTRAS_MENU_CAFFEINE_MAX_MINUTES);

    if (extras_menu_caffeine_config_get_duration() == minutes)
        return TRUE;

    return update_file(change_duration, GINT_TO_POINTER((gint) minutes), error);
}

typedef struct
{
    gboolean enabled;
    guint minutes;
} ScreenOffSetting;

static void
change_screen_off(GKeyFile *file, gpointer data)
{
    const ScreenOffSetting *setting = data;

    g_key_file_set_boolean(file, CONFIG_GROUP, SCREEN_OFF_ENABLED_KEY, setting->enabled);
    g_key_file_set_integer(file, CONFIG_GROUP, SCREEN_OFF_MINUTES_KEY, (gint) setting->minutes);
}

gboolean
extras_menu_caffeine_config_set_screen_off(gboolean enabled, guint minutes, GError **error)
{
    ScreenOffSetting setting;

    setting.enabled = enabled;
    setting.minutes = CLAMP(minutes, 1u, (guint) EXTRAS_MENU_CAFFEINE_SCREEN_OFF_MAX_MINUTES);

    if (extras_menu_caffeine_config_get_screen_off_enabled() == setting.enabled &&
        extras_menu_caffeine_config_get_screen_off_minutes() == setting.minutes)
        return TRUE;

    return update_file(change_screen_off, &setting, error);
}
