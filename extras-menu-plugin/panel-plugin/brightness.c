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

    /* Coalescing state for set(): while the user drags the slider we
     * get dozens of requests per second, and spawning a process for
     * each one made the UI stutter (and the slider fight the drag).
     * Instead only the latest requested value is kept (pending_*),
     * sent at most once per SET_INTERVAL_MS, with one process in
     * flight at a time. */
    gboolean has_pending;
    guint pending_percent;
    guint last_sent_percent;
    gboolean has_last_sent;
    gboolean set_in_flight;
    guint flush_source;

    /* Cancelled on free() so late async completions never touch a
     * freed struct. */
    GCancellable *cancellable;
};

/* Minimum gap between two brightnessctl launches during a drag. */
#define SET_INTERVAL_MS 40

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
        /* Cancelled means free() already ran: brightness is gone. */
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

    g_subprocess_communicate_async(proc, NULL, brightness->cancellable, on_get_finished, brightness);
    g_object_unref(proc);
}

/* --- setting the brightness --------------------------------------------- */

static void schedule_flush(ExtrasMenuBrightness *brightness);

static void
on_set_finished(GObject *source, GAsyncResult *result, gpointer user_data)
{
    GSubprocess *proc = G_SUBPROCESS(source);

    GError *error = NULL;
    gboolean ok = g_subprocess_wait_check_finish(proc, result, &error);
    gboolean cancelled = g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
    g_clear_error(&error);

    /* Cancelled means free() already ran: user_data is gone. */
    if (cancelled)
        return;

    ExtrasMenuBrightness *brightness = user_data;
    brightness->set_in_flight = FALSE;

    if (brightness->has_pending)
    {
        /* The user kept dragging: apply the newest value next and
         * don't report anything yet -- reporting a stale value now
         * would yank the slider back under their finger. */
        schedule_flush(brightness);
        return;
    }

    if (ok)
    {
        /* We know exactly what we set, so report it directly instead
         * of spawning another process just to read it back. */
        if (brightness->callback != NULL)
            brightness->callback(brightness->last_sent_percent, brightness->user_data);
    }
    else
    {
        /* Failed (no permission, no device...): re-read the real value
         * so the UI reflects reality rather than what we asked for. */
        request_current_brightness(brightness);
    }
}

/* Sends the newest pending value, if any. */
static gboolean
flush_pending(gpointer user_data)
{
    ExtrasMenuBrightness *brightness = user_data;
    brightness->flush_source = 0;

    if (!brightness->has_pending || brightness->set_in_flight)
        return G_SOURCE_REMOVE;

    guint percent = brightness->pending_percent;
    brightness->has_pending = FALSE;

    /* Skip no-op writes (the slider can emit several events for the
     * same integer percentage). */
    if (brightness->has_last_sent && brightness->last_sent_percent == percent)
        return G_SOURCE_REMOVE;

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
        return G_SOURCE_REMOVE;
    }

    brightness->last_sent_percent = percent;
    brightness->has_last_sent = TRUE;
    brightness->set_in_flight = TRUE;

    g_subprocess_wait_check_async(proc, brightness->cancellable, on_set_finished, brightness);
    g_object_unref(proc);

    return G_SOURCE_REMOVE;
}

static void
schedule_flush(ExtrasMenuBrightness *brightness)
{
    if (brightness->flush_source == 0)
        brightness->flush_source = g_timeout_add(SET_INTERVAL_MS, flush_pending, brightness);
}

/* --- public API ----------------------------------------------------------- */

ExtrasMenuBrightness *
extras_menu_brightness_new(ExtrasMenuBrightnessChangedFunc callback, gpointer user_data)
{
    ExtrasMenuBrightness *brightness = g_new0(ExtrasMenuBrightness, 1);
    brightness->callback = callback;
    brightness->user_data = user_data;
    brightness->available = FALSE;
    brightness->cancellable = g_cancellable_new();

    request_current_brightness(brightness);

    return brightness;
}

void
extras_menu_brightness_set(ExtrasMenuBrightness *brightness, guint percent)
{
    if (brightness == NULL)
        return;

    percent = CLAMP(percent, EXTRAS_MENU_BRIGHTNESS_MIN_PERCENT, 100);

    /* Only remember the newest value; flush_pending() sends it. */
    brightness->pending_percent = percent;
    brightness->has_pending = TRUE;

    if (!brightness->set_in_flight)
        schedule_flush(brightness);
}

void
extras_menu_brightness_free(ExtrasMenuBrightness *brightness)
{
    if (brightness == NULL)
        return;

    if (brightness->flush_source != 0)
        g_source_remove(brightness->flush_source);

    g_cancellable_cancel(brightness->cancellable);
    g_object_unref(brightness->cancellable);

    g_free(brightness);
}