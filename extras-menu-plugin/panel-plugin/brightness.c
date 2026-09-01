#include "brightness.h"

#include <gio/gio.h>
#include <stdlib.h>
#include <string.h>

struct _ExtrasMenuBrightness
{
    ExtrasMenuBrightnessChangedFunc callback;
    gpointer user_data;

    /* TRUE once we've confirmed brightnessctl exists and works, so
     * callers could in principle check this if they want to hide the
     * brightness row entirely when no backlight is controllable. Not
     * currently read anywhere, but kept for that future use. */
    gboolean available;
};

/* --- reading the current brightness ------------------------------------ */

/* brightnessctl -m prints machine-readable, colon-separated fields:
 *   device,class,current,percent%,max
 * e.g. "intel_backlight,backlight,1499,60%,2500". We only need the
 * percent field (4th, index 3), with its trailing '%' stripped. */
static gboolean
parse_percent_from_machine_output(const gchar *output, guint *out_percent)
{
    if (output == NULL)
        return FALSE;

    gchar **fields = g_strsplit(output, ",", -1);
    gboolean ok = FALSE;

    if (fields != NULL && fields[0] && fields[1] && fields[2] && fields[3])
    {
        gchar *percent_field = g_strdup(fields[3]);
        gchar *percent_sign = strchr(percent_field, '%');
        if (percent_sign != NULL)
            *percent_sign = '\0';

        gchar *endptr = NULL;
        glong value = strtol(percent_field, &endptr, 10);

        if (endptr != percent_field && value >= 0 && value <= 100)
        {
            *out_percent = (guint) value;
            ok = TRUE;
        }

        g_free(percent_field);
    }

    g_strfreev(fields);
    return ok;
}

static void
on_get_finished(GObject *source, GAsyncResult *result, gpointer user_data)
{
    ExtrasMenuBrightness *brightness = user_data;
    GSubprocess *proc = G_SUBPROCESS(source);

    GBytes *stdout_bytes = NULL;
    GError *error = NULL;

    if (!g_subprocess_communicate_finish(proc, result, &stdout_bytes, NULL, &error))
    {
        g_clear_error(&error);
        return;
    }

    gsize size = 0;
    const gchar *data = g_bytes_get_data(stdout_bytes, &size);
    gchar *text = g_strndup(data, size);
    g_strstrip(text);

    guint percent = 0;
    if (parse_percent_from_machine_output(text, &percent))
    {
        brightness->available = TRUE;
        if (brightness->callback != NULL)
            brightness->callback(percent, brightness->user_data);
    }

    g_free(text);
    g_bytes_unref(stdout_bytes);
}

static void
request_current_brightness(ExtrasMenuBrightness *brightness)
{
    GError *error = NULL;

    /* Important: "-m get" only prints the raw current value (e.g.
     * "4882"), NOT a percentage -- the multi-field
     * "device,class,current,percent%,max" output that
     * parse_percent_from_machine_output() expects only comes from
     * "-m info". Using "get" here was the original bug that made the
     * slider ignore the real brightness. */
    GSubprocess *proc = g_subprocess_new(
        G_SUBPROCESS_FLAGS_STDOUT_PIPE,
        &error,
        "brightnessctl", "-m", "info", NULL);

    if (proc == NULL)
    {
        /* brightnessctl not installed, or not on PATH -- silently give
         * up; the UI simply won't reflect real brightness. */
        g_clear_error(&error);
        return;
    }

    g_subprocess_communicate_async(proc, NULL, NULL, on_get_finished, brightness);
    g_object_unref(proc);
}

/* --- setting the brightness --------------------------------------------- */

static void
on_set_finished(GObject *source, GAsyncResult *result, gpointer user_data)
{
    ExtrasMenuBrightness *brightness = user_data;
    GSubprocess *proc = G_SUBPROCESS(source);

    GError *error = NULL;
    g_subprocess_wait_check_finish(proc, result, &error);
    g_clear_error(&error);

    /* Whether it succeeded or not, re-read the actual value so the UI
     * always reflects reality rather than what we merely asked for. */
    request_current_brightness(brightness);
}

/* --- public API ----------------------------------------------------------- */

ExtrasMenuBrightness *
extras_menu_brightness_new(ExtrasMenuBrightnessChangedFunc callback, gpointer user_data)
{
    ExtrasMenuBrightness *brightness = g_new0(ExtrasMenuBrightness, 1);
    brightness->callback = callback;
    brightness->user_data = user_data;
    brightness->available = FALSE;

    request_current_brightness(brightness);

    return brightness;
}

void
extras_menu_brightness_set(ExtrasMenuBrightness *brightness, guint percent)
{
    if (brightness == NULL)
        return;

    if (percent > 100)
        percent = 100;

    gchar *value_arg = g_strdup_printf("%u%%", percent);

    GError *error = NULL;
    GSubprocess *proc = g_subprocess_new(
        G_SUBPROCESS_FLAGS_NONE,
        &error,
        "brightnessctl", "set", value_arg, NULL);

    g_free(value_arg);

    if (proc == NULL)
    {
        g_clear_error(&error);
        return;
    }

    g_subprocess_wait_check_async(proc, NULL, on_set_finished, brightness);
    g_object_unref(proc);
}

void
extras_menu_brightness_free(ExtrasMenuBrightness *brightness)
{
    g_free(brightness);
}