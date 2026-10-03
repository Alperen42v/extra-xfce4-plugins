#include "quick-actions-config.h"

#include <glib/gstdio.h>

#define CONFIG_GROUP     "quick-actions"
#define FALLBACK_ICON    "applications-other-symbolic"

/* --- single action -------------------------------------------------------- */

ExtrasMenuQuickActionConfig *
extras_menu_quick_action_config_new(const gchar *name, const gchar *icon, const gchar *command)
{
    ExtrasMenuQuickActionConfig *action = g_new0(ExtrasMenuQuickActionConfig, 1);
    action->name = g_strdup(name != NULL ? name : "");
    action->icon = g_strdup(icon != NULL ? icon : "");
    action->command = g_strdup(command != NULL ? command : "");
    return action;
}

ExtrasMenuQuickActionConfig *
extras_menu_quick_action_config_copy(const ExtrasMenuQuickActionConfig *src)
{
    return extras_menu_quick_action_config_new(src->name, src->icon, src->command);
}

void
extras_menu_quick_action_config_free(gpointer data)
{
    ExtrasMenuQuickActionConfig *action = data;

    if (action == NULL)
        return;

    g_free(action->name);
    g_free(action->icon);
    g_free(action->command);
    g_free(action);
}

static GPtrArray *
new_action_array(void)
{
    return g_ptr_array_new_with_free_func(extras_menu_quick_action_config_free);
}

/* --- defaults ------------------------------------------------------------- */

GPtrArray *
extras_menu_quick_actions_config_defaults(void)
{
    GPtrArray *actions = new_action_array();

    g_ptr_array_add(actions, extras_menu_quick_action_config_new(
        "Screenshot", "camera-photo-symbolic", "xfce4-screenshooter"));
    g_ptr_array_add(actions, extras_menu_quick_action_config_new(
        "Settings", "preferences-system-symbolic", "xfce4-settings-manager"));
    g_ptr_array_add(actions, extras_menu_quick_action_config_new(
        "Lock screen", "system-lock-screen-symbolic", "xflock4"));
    g_ptr_array_add(actions, extras_menu_quick_action_config_new(
        "Log out", "system-shutdown-symbolic", "xfce4-session-logout"));

    return actions;
}

/* --- file on disk --------------------------------------------------------- */

static gchar *
config_file_path(void)
{
    return g_build_filename(g_get_user_config_dir(), "xfce4", "extras-menu",
                            "quick-actions.ini", NULL);
}

GPtrArray *
extras_menu_quick_actions_config_load(void)
{
    gchar *path = config_file_path();
    GKeyFile *file = g_key_file_new();
    GError *error = NULL;

    if (!g_key_file_load_from_file(file, path, G_KEY_FILE_NONE, &error))
    {
        /* No file yet is the normal first-run case; anything else is
         * worth a log line, but either way the plugin still works. */
        if (!g_error_matches(error, G_FILE_ERROR, G_FILE_ERROR_NOENT))
        {
            g_warning("extras-menu: couldn't read %s: %s -- using the default buttons",
                      path, error->message);
        }
        g_clear_error(&error);
        g_key_file_free(file);
        g_free(path);
        return extras_menu_quick_actions_config_defaults();
    }

    gint count = g_key_file_get_integer(file, CONFIG_GROUP, "count", &error);
    if (error != NULL)
    {
        /* File exists but has no usable "count": treat it as not yet
         * configured rather than as "zero buttons". */
        g_clear_error(&error);
        g_key_file_free(file);
        g_free(path);
        return extras_menu_quick_actions_config_defaults();
    }
    count = CLAMP(count, 0, EXTRAS_MENU_QUICK_ACTIONS_MAX);

    GPtrArray *actions = new_action_array();

    for (gint i = 0; i < count; i++)
    {
        gchar *group = g_strdup_printf("action-%d", i);

        if (g_key_file_has_group(file, group))
        {
            gchar *name = g_key_file_get_string(file, group, "name", NULL);
            gchar *icon = g_key_file_get_string(file, group, "icon", NULL);
            gchar *command = g_key_file_get_string(file, group, "command", NULL);

            g_ptr_array_add(actions, extras_menu_quick_action_config_new(name, icon, command));

            g_free(name);
            g_free(icon);
            g_free(command);
        }

        g_free(group);
    }

    g_key_file_free(file);
    g_free(path);
    return actions;
}

/* --- change listeners ----------------------------------------------------- */

typedef struct
{
    guint id;
    ExtrasMenuQuickActionsChangedFunc func;
    gpointer user_data;
} Listener;

static GSList *listeners = NULL;
static guint next_listener_id = 1;

guint
extras_menu_quick_actions_config_add_listener(ExtrasMenuQuickActionsChangedFunc func,
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
extras_menu_quick_actions_config_remove_listener(guint listener_id)
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
    /* Iterate over a snapshot of the ids, so a listener that adds or
     * removes listeners from inside its callback can't invalidate the
     * walk; an id that has been removed meanwhile is simply skipped. */
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
extras_menu_quick_actions_config_save(const GPtrArray *actions, GError **error)
{
    GKeyFile *file = g_key_file_new();
    guint count = MIN(actions->len, (guint) EXTRAS_MENU_QUICK_ACTIONS_MAX);

    g_key_file_set_integer(file, CONFIG_GROUP, "count", (gint) count);

    for (guint i = 0; i < count; i++)
    {
        const ExtrasMenuQuickActionConfig *action = g_ptr_array_index((GPtrArray *) actions, i);
        gchar *group = g_strdup_printf("action-%u", i);

        g_key_file_set_string(file, group, "name", action->name);
        g_key_file_set_string(file, group, "icon", action->icon);
        g_key_file_set_string(file, group, "command", action->command);

        g_free(group);
    }

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

/* --- icon / command helpers ------------------------------------------------ */

void
extras_menu_quick_action_set_image(GtkImage *image, const gchar *icon, GtkIconSize size)
{
    gchar *trimmed = g_strstrip(g_strdup(icon != NULL ? icon : ""));

    if (trimmed[0] == '/' || g_str_has_prefix(trimmed, "~/"))
    {
        gchar *path = trimmed[0] == '~'
            ? g_build_filename(g_get_home_dir(), trimmed + 2, NULL)
            : g_strdup(trimmed);

        gint pixels = 16;
        gtk_icon_size_lookup(size, &pixels, NULL);

        /* Keep the aspect ratio: a non-square picture shouldn't be
         * squashed to fit the square button. */
        GdkPixbuf *pixbuf = gdk_pixbuf_new_from_file_at_scale(path, pixels, pixels, TRUE, NULL);
        if (pixbuf != NULL)
        {
            gtk_image_set_from_pixbuf(image, pixbuf);
            g_object_unref(pixbuf);
        }
        else
        {
            gtk_image_set_from_icon_name(image, "image-missing", size);
        }

        g_free(path);
    }
    else if (trimmed[0] != '\0')
    {
        gtk_image_set_from_icon_name(image, trimmed, size);
    }
    else
    {
        gtk_image_set_from_icon_name(image, FALLBACK_ICON, size);
    }

    g_free(trimmed);
}

const gchar *
extras_menu_quick_action_command_problem(const gchar *command)
{
    gchar *trimmed = g_strstrip(g_strdup(command != NULL ? command : ""));

    if (trimmed[0] == '\0')
    {
        g_free(trimmed);
        return "No command set -- this button won't be shown.";
    }

    GError *error = NULL;
    gchar **argv = NULL;
    gboolean parsed = g_shell_parse_argv(trimmed, NULL, &argv, &error);

    g_strfreev(argv);
    g_clear_error(&error);
    g_free(trimmed);

    return parsed ? NULL : "Can't parse this command -- check the quotes.";
}