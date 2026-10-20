#include "microphone.h"

#include <gio/gio.h>
#include <string.h>

/*
 * Microphone level and mute for the dropdown's microphone row.
 *
 * Everything goes through `pactl` (PulseAudio's own tool, also provided
 * by PipeWire's pipewire-pulse), always against @DEFAULT_SOURCE@, so
 * the row follows whichever input device is the default:
 *
 *   pactl get-source-volume / get-source-mute   read the current state
 *   pactl set-source-volume / set-source-mute   apply the user's change
 *   pactl subscribe                              tells us when anything
 *                                                changed, from anywhere
 *
 * Nothing here blocks the main loop: every pactl run is asynchronous,
 * and everything pending is cancelled in extras_menu_microphone_free().
 * Callbacks check for that cancellation first and touch nothing else
 * in that case, because the module may already be gone.
 */

#define DEFAULT_SOURCE      "@DEFAULT_SOURCE@"

/* Dragging the slider fires a value-changed for every pixel; a pactl
 * run per event would be wasteful, so changes are collected and sent at
 * most this often. */
#define SET_DELAY_MS        60

/* Several events usually arrive together (one change often produces a
 * burst); they are folded into one refresh. */
#define REFRESH_DELAY_MS    80

/* After the user touched the slider, the level read back from the
 * server (which can lag behind the pointer) must not yank it around. */
#define QUIET_AFTER_USER_US (400 * 1000)

#define RESUBSCRIBE_DELAY_S 3

struct _ExtrasMenuMicrophone
{
    GtkWidget *scale;
    GtkWidget *mute_button;
    GtkWidget *icon;
    gulong scale_handler;
    gulong button_handler;

    /* Last known state of the default input device. */
    gboolean available;      /* pactl worked and there is an input device */
    gint volume;             /* percent */
    gint read_volume;        /* what the running refresh has read so far */
    gboolean muted;
    const gchar *icon_name;  /* the one currently shown (a string literal) */

    /* TRUE while this module itself moves the slider, so its own
     * "value-changed" handler doesn't send the value straight back. */
    gboolean updating_from_backend;

    /* Debounced writes of the slider's value. */
    guint set_timeout;
    gint pending_volume;
    gint64 last_user_change;

    /* Debounced, non-overlapping refreshes. */
    guint refresh_timeout;
    gboolean refresh_running;
    gboolean refresh_again;

    /* `pactl subscribe` and its reconnect timer. */
    GSubprocess *subscribe;
    GDataInputStream *subscribe_stream;
    guint resubscribe_timeout;

    GCancellable *cancellable;
};

static void start_refresh(ExtrasMenuMicrophone *mic);
static void schedule_refresh(ExtrasMenuMicrophone *mic);
static void start_subscribe(ExtrasMenuMicrophone *mic);

/* --- helpers ----------------------------------------------------------------------- */

/* Starts pactl with the C locale, because its output is translated
 * ("Mute: yes" is not "yes" in every language) and is parsed below. */
static GSubprocess *
spawn_pactl(const gchar * const *argv, GSubprocessFlags flags, GError **error)
{
    GSubprocessLauncher *launcher = g_subprocess_launcher_new(flags);
    g_subprocess_launcher_setenv(launcher, "LC_ALL", "C", TRUE);

    GSubprocess *process = g_subprocess_launcher_spawnv(launcher, argv, error);

    g_object_unref(launcher);
    return process;
}

/* The number just before the first '%' in pactl's volume line:
 *   "Volume: front-left: 26214 /  40% / -23.70 dB, front-right: ..."
 * Returns -1 if there is none. */
static gint
parse_percent(const gchar *text)
{
    const gchar *percent = strchr(text, '%');

    if (percent == NULL)
        return -1;

    const gchar *start = percent;
    while (start > text && g_ascii_isdigit(start[-1]))
        start--;

    if (start == percent)
        return -1;

    return (gint) CLAMP(g_ascii_strtoll(start, NULL, 10), 0,
                        EXTRAS_MENU_MICROPHONE_MAX_PERCENT);
}

static gboolean
user_is_adjusting(const ExtrasMenuMicrophone *mic)
{
    return mic->set_timeout != 0 ||
           g_get_monotonic_time() - mic->last_user_change < QUIET_AFTER_USER_US;
}

/* --- showing the state --------------------------------------------------------------- */

static const gchar *
icon_for_state(const ExtrasMenuMicrophone *mic)
{
    if (!mic->available || mic->muted || mic->volume == 0)
        return "microphone-sensitivity-muted-symbolic";
    if (mic->volume <= 33)
        return "microphone-sensitivity-low-symbolic";
    if (mic->volume <= 66)
        return "microphone-sensitivity-medium-symbolic";

    return "microphone-sensitivity-high-symbolic";
}

static void
update_icon(ExtrasMenuMicrophone *mic)
{
    const gchar *name = icon_for_state(mic);

    /* Same literal means same icon: skip the needless re-render. */
    if (name != mic->icon_name)
    {
        mic->icon_name = name;
        gtk_image_set_from_icon_name(GTK_IMAGE(mic->icon), name, GTK_ICON_SIZE_BUTTON);
    }
}

/* Puts mic's state on the widgets. The slider is only moved when
 * `move_slider` is set (not while the user is dragging it). A muted
 * microphone has a greyed-out slider: changing the level of something
 * that is off would do nothing audible. */
static void
apply_state(ExtrasMenuMicrophone *mic, gboolean move_slider)
{
    if (move_slider)
    {
        mic->updating_from_backend = TRUE;
        gtk_range_set_value(GTK_RANGE(mic->scale), mic->volume);
        mic->updating_from_backend = FALSE;
    }

    update_icon(mic);

    gtk_widget_set_sensitive(mic->scale, mic->available && !mic->muted);
    gtk_widget_set_sensitive(mic->mute_button, mic->available);
    gtk_widget_set_tooltip_text(mic->mute_button,
                                !mic->available ? "No microphone found"
                                : mic->muted    ? "Unmute microphone"
                                                : "Mute microphone");
}

/* --- reading the state ----------------------------------------------------------------- */

static void
refresh_done(ExtrasMenuMicrophone *mic)
{
    mic->refresh_running = FALSE;

    if (mic->refresh_again)
    {
        mic->refresh_again = FALSE;
        start_refresh(mic);
    }
}

static void
refresh_failed(ExtrasMenuMicrophone *mic)
{
    mic->available = FALSE;
    apply_state(mic, FALSE);
    refresh_done(mic);
}

static void
on_mute_read(GObject *source, GAsyncResult *result, gpointer user_data)
{
    gchar *out = NULL;
    GError *error = NULL;

    gboolean ok = g_subprocess_communicate_utf8_finish(G_SUBPROCESS(source), result,
                                                        &out, NULL, &error);

    /* Cancelled means the module is being freed: touch nothing of it. */
    if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    {
        g_clear_error(&error);
        g_free(out);
        return;
    }
    g_clear_error(&error);

    ExtrasMenuMicrophone *mic = user_data;

    if (!ok || out == NULL || !g_subprocess_get_successful(G_SUBPROCESS(source)))
    {
        g_free(out);
        refresh_failed(mic);
        return;
    }

    gboolean adjusting = user_is_adjusting(mic);

    mic->available = TRUE;
    mic->muted = (strstr(out, "yes") != NULL);
    if (!adjusting)
        mic->volume = mic->read_volume;

    apply_state(mic, !adjusting);

    g_free(out);
    refresh_done(mic);
}

static void
on_volume_read(GObject *source, GAsyncResult *result, gpointer user_data)
{
    gchar *out = NULL;
    GError *error = NULL;

    gboolean ok = g_subprocess_communicate_utf8_finish(G_SUBPROCESS(source), result,
                                                        &out, NULL, &error);

    if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    {
        g_clear_error(&error);
        g_free(out);
        return;
    }
    g_clear_error(&error);

    ExtrasMenuMicrophone *mic = user_data;
    gint percent = (ok && out != NULL &&
                    g_subprocess_get_successful(G_SUBPROCESS(source))) ? parse_percent(out) : -1;

    g_free(out);

    if (percent < 0)
    {
        refresh_failed(mic);
        return;
    }

    mic->read_volume = percent;

    /* Second half of the refresh: the mute state. */
    const gchar *argv[] = { "pactl", "get-source-mute", DEFAULT_SOURCE, NULL };
    GError *spawn_error = NULL;
    GSubprocess *process = spawn_pactl(argv,
                                       G_SUBPROCESS_FLAGS_STDOUT_PIPE |
                                       G_SUBPROCESS_FLAGS_STDERR_SILENCE,
                                       &spawn_error);
    if (process == NULL)
    {
        g_clear_error(&spawn_error);
        refresh_failed(mic);
        return;
    }

    g_subprocess_communicate_utf8_async(process, NULL, mic->cancellable, on_mute_read, mic);
    g_object_unref(process);
}

static void
start_refresh(ExtrasMenuMicrophone *mic)
{
    /* One refresh at a time; a request that arrives meanwhile is
     * remembered and runs right after. */
    if (mic->refresh_running)
    {
        mic->refresh_again = TRUE;
        return;
    }
    mic->refresh_running = TRUE;

    const gchar *argv[] = { "pactl", "get-source-volume", DEFAULT_SOURCE, NULL };
    GError *error = NULL;
    GSubprocess *process = spawn_pactl(argv,
                                       G_SUBPROCESS_FLAGS_STDOUT_PIPE |
                                       G_SUBPROCESS_FLAGS_STDERR_SILENCE,
                                       &error);
    if (process == NULL)
    {
        /* Most likely pactl isn't installed. The row stays disabled. */
        g_warning("extras-menu: couldn't run pactl for the microphone: %s", error->message);
        g_clear_error(&error);
        refresh_failed(mic);
        return;
    }

    g_subprocess_communicate_utf8_async(process, NULL, mic->cancellable, on_volume_read, mic);
    g_object_unref(process);
}

static gboolean
on_refresh_timeout(gpointer user_data)
{
    ExtrasMenuMicrophone *mic = user_data;

    mic->refresh_timeout = 0;
    start_refresh(mic);

    return G_SOURCE_REMOVE;
}

static void
schedule_refresh(ExtrasMenuMicrophone *mic)
{
    if (mic->refresh_timeout == 0)
        mic->refresh_timeout = g_timeout_add(REFRESH_DELAY_MS, on_refresh_timeout, mic);
}

/* --- writing the state ----------------------------------------------------------------- */

static void
on_set_done_refresh(GObject *source, GAsyncResult *result, gpointer user_data)
{
    GError *error = NULL;

    g_subprocess_wait_finish(G_SUBPROCESS(source), result, &error);

    if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    {
        g_clear_error(&error);
        return;
    }
    g_clear_error(&error);

    /* Read back what really happened, in case the change didn't take
     * (and as a fallback if the event stream is down). */
    schedule_refresh(user_data);
}

/* Runs a pactl command whose output isn't needed. With refresh_after,
 * the state is read back once it has finished. */
static void
run_pactl_set(ExtrasMenuMicrophone *mic, const gchar * const *argv, gboolean refresh_after)
{
    GError *error = NULL;
    GSubprocess *process = spawn_pactl(argv,
                                       G_SUBPROCESS_FLAGS_STDOUT_SILENCE |
                                       G_SUBPROCESS_FLAGS_STDERR_SILENCE,
                                       &error);
    if (process == NULL)
    {
        g_warning("extras-menu: couldn't run pactl for the microphone: %s", error->message);
        g_clear_error(&error);
        return;
    }

    g_subprocess_wait_async(process, mic->cancellable,
                            refresh_after ? on_set_done_refresh : NULL, mic);
    g_object_unref(process);
}

static gboolean
on_set_timeout(gpointer user_data)
{
    ExtrasMenuMicrophone *mic = user_data;

    mic->set_timeout = 0;
    mic->last_user_change = g_get_monotonic_time();

    gchar *level = g_strdup_printf("%d%%", mic->pending_volume);
    const gchar *argv[] = { "pactl", "set-source-volume", DEFAULT_SOURCE, level, NULL };

    run_pactl_set(mic, argv, FALSE);
    g_free(level);

    return G_SOURCE_REMOVE;
}

static void
on_scale_value_changed(GtkRange *range, gpointer user_data)
{
    ExtrasMenuMicrophone *mic = user_data;

    if (mic->updating_from_backend)
        return;

    gint volume = (gint) (gtk_range_get_value(range) + 0.5);

    mic->volume = volume;
    mic->pending_volume = volume;
    mic->last_user_change = g_get_monotonic_time();

    /* The icon follows the slider at once, not after the server's echo. */
    update_icon(mic);

    if (mic->set_timeout == 0)
        mic->set_timeout = g_timeout_add(SET_DELAY_MS, on_set_timeout, mic);
}

static void
on_mute_button_clicked(GtkButton *button, gpointer user_data)
{
    ExtrasMenuMicrophone *mic = user_data;
    (void) button;

    /* Show the new state right away; the read-back after the command
     * corrects it if it didn't work. */
    mic->muted = !mic->muted;
    apply_state(mic, FALSE);

    const gchar *argv[] = { "pactl", "set-source-mute", DEFAULT_SOURCE, "toggle", NULL };
    run_pactl_set(mic, argv, TRUE);
}

/* --- following changes made elsewhere ---------------------------------------------------- */

static void
stop_subscribe(ExtrasMenuMicrophone *mic)
{
    g_clear_object(&mic->subscribe_stream);

    if (mic->subscribe != NULL)
    {
        g_subprocess_force_exit(mic->subscribe);
        g_clear_object(&mic->subscribe);
    }
}

static gboolean
on_resubscribe_timeout(gpointer user_data)
{
    ExtrasMenuMicrophone *mic = user_data;

    mic->resubscribe_timeout = 0;
    start_subscribe(mic);
    schedule_refresh(mic); /* what happened while there was no stream */

    return G_SOURCE_REMOVE;
}

static void
schedule_resubscribe(ExtrasMenuMicrophone *mic)
{
    if (mic->resubscribe_timeout == 0)
    {
        mic->resubscribe_timeout = g_timeout_add_seconds(RESUBSCRIBE_DELAY_S,
                                                         on_resubscribe_timeout, mic);
    }
}

static void read_next_event(ExtrasMenuMicrophone *mic);

static void
on_event_line(GObject *source, GAsyncResult *result, gpointer user_data)
{
    GError *error = NULL;
    gchar *line = g_data_input_stream_read_line_finish(G_DATA_INPUT_STREAM(source), result,
                                                       NULL, &error);

    if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    {
        g_clear_error(&error);
        g_free(line);
        return;
    }
    g_clear_error(&error);

    ExtrasMenuMicrophone *mic = user_data;

    if (line == NULL)
    {
        /* The stream ended (the sound server restarted, or pactl died):
         * try again in a moment. */
        stop_subscribe(mic);
        schedule_resubscribe(mic);
        return;
    }

    /* "Event 'change' on source #5", "Event 'change' on server" (the
     * default input was switched). Events about streams, sinks and
     * cards don't matter here. */
    if (strstr(line, "on source #") != NULL || strstr(line, "on server") != NULL)
        schedule_refresh(mic);

    g_free(line);
    read_next_event(mic);
}

static void
read_next_event(ExtrasMenuMicrophone *mic)
{
    g_data_input_stream_read_line_async(mic->subscribe_stream, G_PRIORITY_DEFAULT,
                                        mic->cancellable, on_event_line, mic);
}

static void
start_subscribe(ExtrasMenuMicrophone *mic)
{
    const gchar *argv[] = { "pactl", "subscribe", NULL };
    GError *error = NULL;

    mic->subscribe = spawn_pactl(argv,
                                 G_SUBPROCESS_FLAGS_STDOUT_PIPE |
                                 G_SUBPROCESS_FLAGS_STDERR_SILENCE,
                                 &error);
    if (mic->subscribe == NULL)
    {
        g_clear_error(&error);
        schedule_resubscribe(mic);
        return;
    }

    mic->subscribe_stream = g_data_input_stream_new(g_subprocess_get_stdout_pipe(mic->subscribe));
    read_next_event(mic);
}

/* --- setup and teardown --------------------------------------------------------------------- */

ExtrasMenuMicrophone *
extras_menu_microphone_new(GtkWidget *scale, GtkWidget *mute_button, GtkWidget *icon)
{
    g_return_val_if_fail(GTK_IS_RANGE(scale), NULL);
    g_return_val_if_fail(GTK_IS_BUTTON(mute_button), NULL);
    g_return_val_if_fail(GTK_IS_IMAGE(icon), NULL);

    ExtrasMenuMicrophone *mic = g_new0(ExtrasMenuMicrophone, 1);

    mic->scale = g_object_ref(scale);
    mic->mute_button = g_object_ref(mute_button);
    mic->icon = g_object_ref(icon);
    mic->cancellable = g_cancellable_new();

    mic->scale_handler = g_signal_connect(scale, "value-changed",
                                          G_CALLBACK(on_scale_value_changed), mic);
    mic->button_handler = g_signal_connect(mute_button, "clicked",
                                           G_CALLBACK(on_mute_button_clicked), mic);

    /* Disabled until the first read says there is a microphone. */
    apply_state(mic, FALSE);

    start_refresh(mic);
    start_subscribe(mic);

    return mic;
}

void
extras_menu_microphone_free(ExtrasMenuMicrophone *mic)
{
    if (mic == NULL)
        return;

    /* Everything still running reports "cancelled" and then leaves the
     * (by then freed) module alone. */
    g_cancellable_cancel(mic->cancellable);

    if (mic->set_timeout != 0)
        g_source_remove(mic->set_timeout);
    if (mic->refresh_timeout != 0)
        g_source_remove(mic->refresh_timeout);
    if (mic->resubscribe_timeout != 0)
        g_source_remove(mic->resubscribe_timeout);

    stop_subscribe(mic);

    g_signal_handler_disconnect(mic->scale, mic->scale_handler);
    g_signal_handler_disconnect(mic->mute_button, mic->button_handler);

    g_object_unref(mic->scale);
    g_object_unref(mic->mute_button);
    g_object_unref(mic->icon);
    g_object_unref(mic->cancellable);

    g_free(mic);
}