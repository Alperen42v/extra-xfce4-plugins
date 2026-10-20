#ifndef EXTRAS_MENU_MICROPHONE_H
#define EXTRAS_MENU_MICROPHONE_H

#include <gtk/gtk.h>

G_BEGIN_DECLS

/* The microphone slider's scale, in percent of the source's nominal
 * volume. Above NORMAL the signal is amplified in software (it can get
 * noisy or clip), which is why the slider draws a thin line there.
 * STABLE is the level that works well for most microphones: the second
 * thin line. MAX is where the slider ends. */
#define EXTRAS_MENU_MICROPHONE_MAX_PERCENT    150
#define EXTRAS_MENU_MICROPHONE_NORMAL_PERCENT 100
#define EXTRAS_MENU_MICROPHONE_STABLE_PERCENT  40

typedef struct _ExtrasMenuMicrophone ExtrasMenuMicrophone;

/*
 * Wires the dropdown's microphone row to the default input device.
 *
 * `scale` is the row's GtkScale (0..MAX_PERCENT), `mute_button` the
 * button around its icon (a click mutes/unmutes the microphone) and
 * `icon` the GtkImage inside that button, whose icon this module
 * swaps with the level. All three come from
 * extras_menu_popover_get_microphone(). The module keeps references to
 * them until extras_menu_microphone_free().
 *
 * It talks to PulseAudio or PipeWire (pipewire-pulse) through the
 * `pactl` tool: it reads the level and mute state of @DEFAULT_SOURCE@,
 * writes the user's changes back, and follows `pactl subscribe` so
 * changes made elsewhere (pavucontrol, a keyboard mute key, switching
 * the default input) show up in the row. If pactl or an input device
 * is missing, the row is simply shown disabled.
 *
 * The module owns all its state: the plugin only keeps the returned
 * pointer.
 */
ExtrasMenuMicrophone *extras_menu_microphone_new(GtkWidget *scale,
                                                  GtkWidget *mute_button,
                                                  GtkWidget *icon);

/* Stops everything the module started and releases it. NULL is fine. */
void extras_menu_microphone_free(ExtrasMenuMicrophone *mic);

G_END_DECLS

#endif /* EXTRAS_MENU_MICROPHONE_H */