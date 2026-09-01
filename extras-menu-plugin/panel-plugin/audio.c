#include "audio.h"

#include <pulse/glib-mainloop.h>
#include <pulse/pulseaudio.h>

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

    ExtrasMenuAudioChangedFunc callback;
    gpointer user_data;
};

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

    audio->default_sink_channels = info->volume.channels;
    emit_sink_state(audio, info);
}

static void
request_default_sink_info(ExtrasMenuAudio *audio)
{
    if (audio->default_sink_name == NULL)
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

    g_free(audio->default_sink_name);
    audio->default_sink_name = g_strdup(info->default_sink_name);

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
 * changes (default sink switched). We don't inspect the event details
 * closely -- it's cheap enough to just re-fetch server info, which in
 * turn re-fetches the (possibly new) default sink's info. This keeps
 * the logic simple and correct even if the default sink itself changes
 * (e.g. plugging in headphones). */
static void
on_subscribe_event(pa_context *context, pa_subscription_event_type_t event_type,
                    uint32_t idx, void *raw_audio)
{
    ExtrasMenuAudio *audio = raw_audio;
    (void) context;
    (void) idx;

    pa_subscription_event_type_t facility =
        event_type & PA_SUBSCRIPTION_EVENT_FACILITY_MASK;

    if (facility == PA_SUBSCRIPTION_EVENT_SINK ||
        facility == PA_SUBSCRIPTION_EVENT_SERVER)
    {
        request_server_info(audio);
    }
}

/* --- connection lifecycle ---------------------------------------------- */

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
            /* Sound server is not reachable (yet). We simply stay
             * quiet -- the popover's slider keeps whatever value it
             * was last showing (or its initial default). A more
             * complete implementation could retry after a delay, but
             * for a panel plugin popover that's opened on demand this
             * is an acceptable trade-off for now. */
            break;

        default:
            break;
    }
}

ExtrasMenuAudio *
extras_menu_audio_new(ExtrasMenuAudioChangedFunc callback, gpointer user_data)
{
    ExtrasMenuAudio *audio = g_new0(ExtrasMenuAudio, 1);
    audio->callback = callback;
    audio->user_data = user_data;
    audio->default_sink_channels = 2; /* sane fallback until we learn better */

    audio->mainloop = pa_glib_mainloop_new(NULL);
    pa_mainloop_api *api = pa_glib_mainloop_get_api(audio->mainloop);

    audio->context = pa_context_new(api, "XFCE Extras Menu");
    pa_context_set_state_callback(audio->context, on_context_state, audio);

    pa_context_connect(audio->context, NULL, PA_CONTEXT_NOFLAGS, NULL);

    return audio;
}

void
extras_menu_audio_set_volume(ExtrasMenuAudio *audio, guint percent)
{
    if (audio == NULL || audio->context == NULL || audio->default_sink_name == NULL)
        return;

    if (pa_context_get_state(audio->context) != PA_CONTEXT_READY)
        return;

    if (percent > 100)
        percent = 100;

    pa_volume_t pa_vol = (pa_volume_t) (((gdouble) percent / 100.0) * PA_VOLUME_NORM);

    pa_cvolume cvolume;
    pa_cvolume_init(&cvolume);
    pa_cvolume_set(&cvolume, audio->default_sink_channels, pa_vol);

    pa_operation *op = pa_context_set_sink_volume_by_name(
        audio->context, audio->default_sink_name, &cvolume, NULL, NULL);
    if (op != NULL)
        pa_operation_unref(op);
}

void
extras_menu_audio_free(ExtrasMenuAudio *audio)
{
    if (audio == NULL)
        return;

    if (audio->context != NULL)
    {
        pa_context_set_state_callback(audio->context, NULL, NULL);
        pa_context_disconnect(audio->context);
        pa_context_unref(audio->context);
    }

    if (audio->mainloop != NULL)
        pa_glib_mainloop_free(audio->mainloop);

    g_free(audio->default_sink_name);
    g_free(audio);
}