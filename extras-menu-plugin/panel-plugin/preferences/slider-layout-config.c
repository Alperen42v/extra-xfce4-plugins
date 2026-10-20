#include "slider-layout-config.h"

#include <glib/gstdio.h>

#define CONFIG_GROUP "slider-layout"
#define ORDER_KEY    "order"

/* The catalog of every slider, in default order. Adding a slider = one
 * line here (plus its id in the header, EXTRAS_MENU_SLIDER_COUNT, and
 * building the row in popover.c). */
static const ExtrasMenuSliderInfo sliders[] = {
    { EXTRAS_MENU_SLIDER_VOLUME,     "Volume",     "audio-volume-high-symbolic",
      "Sets the sound volume; the icon changes when it is muted." },
    { EXTRAS_MENU_SLIDER_BRIGHTNESS, "Brightness", "display-brightness-symbolic",
      "Sets the screen brightness (needs brightnessctl)." },
    { EXTRAS_MENU_SLIDER_MICROPHONE, "Microphone", "microphone-sensitivity-high-symbolic",
      "Sets the microphone level up to 150%; click its icon to mute (needs pactl)." },
};

G_STATIC_ASSERT(G_N_ELEMENTS(sliders) == EXTRAS_MENU_SLIDER_COUNT);

/* Index of the slider with this id in the table, or -1. */
static gint
slider_index_for_id(const gchar *id)
{
    for (guint i = 0; i < G_N_ELEMENTS(sliders); i++)
    {
        if (g_strcmp0(sliders[i].id, id) == 0)
            return (gint) i;
    }

    return -1;
}

const ExtrasMenuSliderInfo *
extras_menu_slider_catalog_get(guint index)
{
    g_return_val_if_fail(index < G_N_ELEMENTS(sliders), NULL);
    return &sliders[index];
}

gboolean
extras_menu_slider_layout_contains(const GPtrArray *layout, const gchar *id)
{
    for (guint i = 0; i < layout->len; i++)
    {
        const ExtrasMenuSliderInfo *slider = g_ptr_array_index(layout, i);

        if (g_strcmp0(slider->id, id) == 0)
            return TRUE;
    }

    return FALSE;
}

/* --- defaults / load / save --------------------------------------------------- */

GPtrArray *
extras_menu_slider_layout_defaults(void)
{
    GPtrArray *layout = g_ptr_array_new();

    for (guint i = 0; i < G_N_ELEMENTS(sliders); i++)
        g_ptr_array_add(layout, (gpointer) &sliders[i]);

    return layout;
}

static gchar *
config_file_path(void)
{
    return g_build_filename(g_get_user_config_dir(), "xfce4", "extras-menu",
                            "slider-layout.ini", NULL);
}

GPtrArray *
extras_menu_slider_layout_load(void)
{
    GPtrArray *layout = g_ptr_array_new();
    gboolean seen[EXTRAS_MENU_SLIDER_COUNT] = { FALSE };
    gboolean configured = FALSE; /* the file has a slider list (possibly empty) */

    gchar *path = config_file_path();
    GKeyFile *file = g_key_file_new();
    GError *error = NULL;

    if (g_key_file_load_from_file(file, path, G_KEY_FILE_NONE, &error))
    {
        gsize length = 0;
        gchar **ids = g_key_file_get_string_list(file, CONFIG_GROUP, ORDER_KEY, &length, NULL);

        /* NULL means there is no such key; an empty list is a real
         * answer ("no sliders") and must not fall back to the defaults. */
        configured = (ids != NULL);

        for (gsize i = 0; ids != NULL && i < length; i++)
        {
            gint index = slider_index_for_id(ids[i]);

            if (index >= 0 && !seen[index])
            {
                seen[index] = TRUE;
                g_ptr_array_add(layout, (gpointer) &sliders[index]);
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
            g_warning("extras-menu: couldn't read %s: %s -- using the default sliders",
                      path, error->message);
        }
        g_clear_error(&error);
    }

    /* Nothing saved yet: show every slider. */
    if (!configured)
    {
        for (guint i = 0; i < G_N_ELEMENTS(sliders); i++)
            g_ptr_array_add(layout, (gpointer) &sliders[i]);
    }

    g_key_file_free(file);
    g_free(path);
    return layout;
}

/* --- change listeners --------------------------------------------------------- */

typedef struct
{
    guint id;
    ExtrasMenuSliderLayoutChangedFunc func;
    gpointer user_data;
} Listener;

static GSList *listeners = NULL;
static guint next_listener_id = 1;

guint
extras_menu_slider_layout_add_listener(ExtrasMenuSliderLayoutChangedFunc func, gpointer user_data)
{
    Listener *listener = g_new0(Listener, 1);
    listener->id = next_listener_id++;
    listener->func = func;
    listener->user_data = user_data;

    listeners = g_slist_append(listeners, listener);
    return listener->id;
}

void
extras_menu_slider_layout_remove_listener(guint listener_id)
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
extras_menu_slider_layout_save(const GPtrArray *layout, GError **error)
{
    const gchar *ids[EXTRAS_MENU_SLIDER_COUNT];
    guint count = MIN(layout->len, (guint) EXTRAS_MENU_SLIDER_COUNT);

    for (guint i = 0; i < count; i++)
    {
        const ExtrasMenuSliderInfo *slider = g_ptr_array_index(layout, i);
        ids[i] = slider->id;
    }

    GKeyFile *file = g_key_file_new();
    g_key_file_set_string_list(file, CONFIG_GROUP, ORDER_KEY, ids, count);

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