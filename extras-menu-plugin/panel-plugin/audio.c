#include "audio.h"

#include <pulse/glib-mainloop.h>
#include <pulse/pulseaudio.h>

/* Wait this long before reconnecting after the sound server went away
 * (restart of PipeWire/PulseAudio, logout/login of a user service...). */
#define RECONNECT_DELAY_S   3

/* Dragging the slider fires a value-changed for every pixel. Writes are
 * collected and sent at most this often. */
#define SET_DELAY_MS        50

/* After a write, what the server reports back can lag behind the
 * pointer; the slider must not be yanked around by it. Once this time
 * has passed without a new write, the real state is read once more. */
#define QUIET_AFTER_SET_MS  300

struct _ExtrasMenuAudio
{
    pa_glib_mainloop *mainloop;
    pa_context *context;

    /* Name of the current default sink, so we know which sink's
     * volume/mute to report and which one to write to. Updated
     * whenever the server tells us the default sink changed. */
    gchar *default_sink_name;

    /* Channel count of the default sink, needed to build a correctly
     * shaped pa_cvolume when we want to set the volume -- PulseAudio
     * rejects a cvolume whose channel count doesn't match the sink's. */
    guint8 default_sink_channels;

    /* Whether the default sink was muted the last time we heard from the
     * server, so a "toggle mute" request knows which way to flip. */
    gboolean muted;

    /* Index of the default sink, to tell whether a change event is
     * about it (events about other sinks, e.g. HDMI, are ignored).
     * PA_INVALID_INDEX while unknown. */
    uint32_t default_sink_index;

    /* Debounced writes of the volume. */
    guint set_timeout;
    guint pending_percent;
    gint64 quiet_until;       /* monotonic microseconds */
    guint resync_timeout;

    guint reconnect_timeout;

    ExtrasMenuAudioChangedFunc callback;
    gpointer user_data;
};

static void connect_context(ExtrasMenuAudio *audio);

/* --- helpers -------------------------------------------------------- */

/* PulseAudio volumes are per-channel and expressed on a 0..PA_VOLUME_NORM
 * (roughly 0..65536) scale. We only care about a single number for the
 * slider, so we average the channels and rescale to 0-100. */
static guint
volume_to_percent(const pa_cvolume *cvolume)
{
    guint avg = pa_cvolume_avg(cvolume);
    gdouble percent = ((gdouble) avg / (gdouble) PA_VOLUME_NORM) * 100.0;

    if (percent < 0.0)
        percent = 0.0;
    if (percent > 100.0)
        percent = 100.0;

    return (guint) (percent + 0.5);
}

static gboolean
user_is_adjusting(const ExtrasMenuAudio *audio)
{
    return audio->set_timeout != 0 || g_get_monotonic_time() < audio->quiet_until;
}

static void
emit_sink_state(ExtrasMenuAudio *audio, const pa_sink_info *info)
{
    if (audio->callback == NULL)
        return;

    audio->callback(volume_to_percent(&info->volume),
                     info->mute ? TRUE : FALSE,
                     audio->user_data);
}

/* --- fetching the default sink's current state ----------------------- */

static void
on_sink_info(pa_context *context, const pa_sink_info *info, int eol,
             void *raw_audio)
{
    ExtrasMenuAudio *audio = raw_audio;
    (void) context;

    /* eol (end-of-list) fires once more after the real results, with
     * info == NULL, to mark completion. Nothing to do on that call. */
    if (eol > 0 || info == NULL)
        return;

    audio->default_sink_index = info->index;
    audio->default_sink_channels = info->volume.channels;
    audio->muted = info->mute ? TRUE : FALSE;

    /* While the user is dragging (and shortly after), the server's
     * answer is older than the pointer; the resync timer reports the
     * real state afterwards. */
    if (user_is_adjusting(audio))
        return;

    emit_sink_state(audio, info);
}

static void
request_default_sink_info(ExtrasMenuAudio *audio)
{
    if (audio->default_sink_name == NULL || audio->context == NULL ||
        pa_context_get_state(audio->context) != PA_CONTEXT_READY)
        return;

    pa_operation *op = pa_context_get_sink_info_by_name(
        audio->context, audio->default_sink_name, on_sink_info, audio);

    if (op != NULL)
        pa_operation_unref(op);
}

/* --- tracking which sink is the default one --------------------------- */

static void
on_server_info(pa_context *context, const pa_server_info *info, void *raw_audio)
{
    ExtrasMenuAudio *audio = raw_audio;
    (void) context;

    if (info == NULL || info->default_sink_name == NULL)
        return;

    if (g_strcmp0(audio->default_sink_name, info->default_sink_name) != 0)
    {
        g_free(audio->default_sink_name);
        audio->default_sink_name = g_strdup(info->default_sink_name);
        audio->default_sink_index = PA_INVALID_INDEX; /* learned from the sink info */
    }

    request_default_sink_info(audio);
}

static void
request_server_info(ExtrasMenuAudio *audio)
{
    pa_operation *op = pa_context_get_server_info(audio->context, on_server_info, audio);
    if (op != NULL)
        pa_operation_unref(op);
}

/* --- live change notifications ---------------------------------------- */

/* Fired by the server on sink changes (volume, mute, ...) and on server
 * changes (default sink switched).
 *
 *  - A plain change of the default sink (its volume or mute): only that
 *    sink's info is fetched again -- one request.
 *  - A change of another sink: nothing to do.
 *  - Anything else (a sink appearing or disappearing, the default
 *    sink switched, or the default sink not known yet): server info is
 *    fetched, which in turn fetches the (possibly new) default sink's
 *    info. This stays correct when e.g. headphones are plugged in. */
static void
on_subscribe_event(pa_context *context, pa_subscription_event_type_t event_type,
                    uint32_t idx, void *raw_audio)
{
    ExtrasMenuAudio *audio = raw_audio;
    (void) context;

    pa_subscription_event_type_t facility =
        event_type & PA_SUBSCRIPTION_EVENT_FACILITY_MASK;
    pa_subscription_event_type_t kind =
        event_type & PA_SUBSCRIPTION_EVENT_TYPE_MASK;

    if (facility == PA_SUBSCRIPTION_EVENT_SERVER)
    {
        request_server_info(audio);
    }
    else if (facility == PA_SUBSCRIPTION_EVENT_SINK)
    {
        if (kind == PA_SUBSCRIPTION_EVENT_CHANGE &&
            audio->default_sink_index != PA_INVALID_INDEX)
        {
            if (idx == audio->default_sink_index)
                request_default_sink_info(audio);
        }
        else
        {
            request_server_info(audio);
        }
    }
}

/* --- connection lifecycle ---------------------------------------------- */

static gboolean on_reconnect_timeout(gpointer raw_audio);

static void
on_context_state(pa_context *context, void *raw_audio)
{
    ExtrasMenuAudio *audio = raw_audio;

    switch (pa_context_get_state(context))
    {
        case PA_CONTEXT_READY:
        {
            pa_context_set_subscribe_callback(context, on_subscribe_event, audio);

            pa_operation *op = pa_context_subscribe(
                context,
                PA_SUBSCRIPTION_MASK_SINK | PA_SUBSCRIPTION_MASK_SERVER,
                NULL, NULL);
            if (op != NULL)
                pa_operation_unref(op);

            request_server_info(audio);
            break;
        }

        case PA_CONTEXT_FAILED:
        case PA_CONTEXT_TERMINATED:
            /* The connection died (the sound server restarted or was
             * stopped). Try again in a moment with a fresh context;
             * the slider keeps its last value meanwhile. The context
             * isn't replaced right here: this runs inside one of its
             * own callbacks. */
            if (audio->reconnect_timeout == 0)
            {
                audio->reconnect_timeout = g_timeout_add_seconds(RECONNECT_DELAY_S,
                                                                 on_reconnect_timeout, audio);
            }
            break;

        default:
            break;
    }
}

/* Creates a context and starts connecting it. NOAUTOSPAWN: a panel
 * plugin must not start a PulseAudio daemon of its own next to a
 * PipeWire session. NOFAIL: if the server isn't up yet (the panel can
 * start before it does), wait for it instead of failing at once. */
static void
connect_context(ExtrasMenuAudio *audio)
{
    pa_mainloop_api *api = pa_glib_mainloop_get_api(audio->mainloop);

    audio->context = pa_context_new(api, "XFCE Extras Menu");
    pa_context_set_state_callback(audio->context, on_context_state, audio);

    if (pa_context_connect(audio->context, NULL,
                           PA_CONTEXT_NOAUTOSPAWN | PA_CONTEXT_NOFAIL, NULL) < 0 &&
        audio->reconnect_timeout == 0)
    {
        audio->reconnect_timeout = g_timeout_add_seconds(RECONNECT_DELAY_S,
                                                         on_reconnect_timeout, audio);
    }
}

/* Throws the current context away. Its callbacks are cleared first, so
 * nothing runs on it afterwards; whatever it still had pending is
 * cancelled by the disconnect. */
static void
drop_context(ExtrasMenuAudio *audio)
{
    if (audio->context == NULL)
        return;

    pa_context_set_state_callback(audio->context, NULL, NULL);
    pa_context_set_subscribe_callback(audio->context, NULL, NULL);
    pa_context_disconnect(audio->context);
    pa_context_unref(audio->context);
    audio->context = NULL;

    /* Belongs to the old server; the new one tells us again. */
    g_clear_pointer(&audio->default_sink_name, g_free);
    audio->default_sink_index = PA_INVALID_INDEX;
}

static gboolean
on_reconnect_timeout(gpointer raw_audio)
{
    ExtrasMenuAudio *audio = raw_audio;

    audio->reconnect_timeout = 0;
    drop_context(audio);
    connect_context(audio);

    return G_SOURCE_REMOVE;
}

/* Reads the default sink's state once the quiet time after the last
 * write is over, so the slider ends up showing what the server really
 * has (it may have clamped or refused the value). */
static gboolean
on_resync_timeout(gpointer raw_audio)
{
    ExtrasMenuAudio *audio = raw_audio;

    audio->resync_timeout = 0;
    request_default_sink_info(audio);

    return G_SOURCE_REMOVE;
}

static void
arm_resync(ExtrasMenuAudio *audio)
{
    if (audio->resync_timeout != 0)
        g_source_remove(audio->resync_timeout);

    audio->resync_timeout = g_timeout_add(QUIET_AFTER_SET_MS + 50, on_resync_timeout, audio);
}

ExtrasMenuAudio *
extras_menu_audio_new(ExtrasMenuAudioChangedFunc callback, gpointer user_data)
{
    ExtrasMenuAudio *audio = g_new0(ExtrasMenuAudio, 1);
    audio->callback = callback;
    audio->user_data = user_data;
    audio->default_sink_channels = 2; /* sane fallback until we learn better */
    audio->default_sink_index = PA_INVALID_INDEX;

    audio->mainloop = pa_glib_mainloop_new(NULL);
    connect_context(audio);

    return audio;
}

/* The actual write, run by the debounce timer. */
static gboolean
on_set_timeout(gpointer raw_audio)
{
    ExtrasMenuAudio *audio = raw_audio;
    guint percent = audio->pending_percent;

    audio->set_timeout = 0;

    if (audio->context == NULL || audio->default_sink_name == NULL ||
        pa_context_get_state(audio->context) != PA_CONTEXT_READY)
        return G_SOURCE_REMOVE;

    audio->quiet_until = g_get_monotonic_time() + QUIET_AFTER_SET_MS * 1000;
    arm_resync(audio);

    pa_volume_t pa_vol = (pa_volume_t) (((gdouble) percent / 100.0) * PA_VOLUME_NORM);

    pa_cvolume cvolume;
    pa_cvolume_init(&cvolume);
    pa_cvolume_set(&cvolume, audio->default_sink_channels, pa_vol);

    pa_operation *op = pa_context_set_sink_volume_by_name(
        audio->context, audio->default_sink_name, &cvolume, NULL, NULL);
    if (op != NULL)
        pa_operation_unref(op);

    return G_SOURCE_REMOVE;
}

void
extras_menu_audio_set_volume(ExtrasMenuAudio *audio, guint percent)
{
    if (audio == NULL || audio->context == NULL || audio->default_sink_name == NULL)
        return;

    if (pa_context_get_state(audio->context) != PA_CONTEXT_READY)
        return;

    /* Only the latest value matters: it is written when the timer
     * fires, not once per slider step. */
    audio->pending_percent = MIN(percent, 100);

    if (audio->set_timeout == 0)
        audio->set_timeout = g_timeout_add(SET_DELAY_MS, on_set_timeout, audio);
}

void
extras_menu_audio_toggle_mute(ExtrasMenuAudio *audio)
{
    if (audio == NULL || audio->context == NULL || audio->default_sink_name == NULL)
        return;

    if (pa_context_get_state(audio->context) != PA_CONTEXT_READY)
        return;

    pa_operation *op = pa_context_set_sink_mute_by_name(
        audio->context, audio->default_sink_name, audio->muted ? 0 : 1, NULL, NULL);
    if (op != NULL)
        pa_operation_unref(op);

    /* The server's answer could land inside the quiet time of a slider
     * drag a moment ago and be held back; this reads it once more. */
    arm_resync(audio);
}

void
extras_menu_audio_free(ExtrasMenuAudio *audio)
{
    if (audio == NULL)
        return;

    if (audio->set_timeout != 0)
        g_source_remove(audio->set_timeout);
    if (audio->resync_timeout != 0)
        g_source_remove(audio->resync_timeout);
    if (audio->reconnect_timeout != 0)
        g_source_remove(audio->reconnect_timeout);

    drop_context(audio);

    if (audio->mainloop != NULL)
        pa_glib_mainloop_free(audio->mainloop);

    g_free(audio->default_sink_name);
    g_free(audio);
}