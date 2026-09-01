#ifndef EXTRAS_MENU_AUDIO_H
#define EXTRAS_MENU_AUDIO_H

#include <glib.h>

G_BEGIN_DECLS

/*
 * Thin wrapper around libpulse. Works identically against PipeWire's
 * pipewire-pulse compatibility socket -- same API, no special-casing
 * needed on our side. Talks to the default sink only, which covers
 * the "single master volume slider" use case of the popover.
 *
 * Everything runs on the caller's GLib main context via
 * pa_glib_mainloop, so no extra thread is spawned and all callbacks
 * arrive safely on the GTK main thread.
 */

typedef struct _ExtrasMenuAudio ExtrasMenuAudio;

/* Fired whenever the default sink's volume or mute state is known or
 * changes -- on initial connect, when we change it ourselves, and
 * when it changes elsewhere (pavucontrol, media keys, another app).
 * volume_percent is 0-100. */
typedef void (*ExtrasMenuAudioChangedFunc)(guint volume_percent,
                                            gboolean muted,
                                            gpointer user_data);

/* Starts connecting to the sound server asynchronously. Safe to call
 * even if no server is reachable yet at this instant -- the callback
 * simply never fires until one appears (or fires once it does). */
ExtrasMenuAudio *extras_menu_audio_new(ExtrasMenuAudioChangedFunc callback,
                                        gpointer user_data);

/* Requests the default sink's volume be set to percent (0-100,
 * clamped). Fire-and-forget: the actual resulting state comes back
 * through the ExtrasMenuAudioChangedFunc callback, since PulseAudio
 * operations are asynchronous. Safe to call before the connection is
 * fully established -- the request is just dropped in that case. */
void extras_menu_audio_set_volume(ExtrasMenuAudio *audio, guint percent);

/* Disconnects and frees everything. */
void extras_menu_audio_free(ExtrasMenuAudio *audio);

G_END_DECLS

#endif /* EXTRAS_MENU_AUDIO_H */