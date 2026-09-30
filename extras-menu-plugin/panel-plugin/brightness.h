#ifndef EXTRAS_MENU_BRIGHTNESS_H
#define EXTRAS_MENU_BRIGHTNESS_H

#include <glib.h>

G_BEGIN_DECLS

/*
 * Thin wrapper around the `brightnessctl` CLI tool. We shell out to it
 * (via GSubprocess, fully async) rather than writing to
 * /sys/class/backlight/*\/brightness directly, since that file is
 * root-owned by default -- brightnessctl ships its own udev rule that
 * grants write access to the `video` group instead, which is the
 * standard, non-root way to do this on Arch (and most distros).
 *
 * There is no live-change notification support here (unlike audio.c):
 * brightnessctl has no equivalent to PulseAudio's subscribe mechanism,
 * and brightness is rarely changed by third parties while our dropdown
 * is open, so this is a reasonable simplification for now.
 */

typedef struct _ExtrasMenuBrightness ExtrasMenuBrightness;

/* Lowest brightness we ever apply. At 0% many panels turn the
 * backlight completely off, which leaves a black screen with no easy
 * way back; the slider's lower bound and extras_menu_brightness_set()
 * both enforce this. */
#define EXTRAS_MENU_BRIGHTNESS_MIN_PERCENT 5

/* Fired once after the initial brightness has been read (shortly
 * after extras_menu_brightness_new() returns), and again after every
 * extras_menu_brightness_set() call completes. percent is 0-100.
 * Not fired at all if brightnessctl is missing or no backlight device
 * is found -- callers should keep the brightness row visually
 * reasonable in that case (e.g. leave it at its default position). */
typedef void (*ExtrasMenuBrightnessChangedFunc)(guint percent, gpointer user_data);

ExtrasMenuBrightness *extras_menu_brightness_new(ExtrasMenuBrightnessChangedFunc callback,
                                                  gpointer user_data);

/* Requests brightness be set to percent (clamped to
 * EXTRAS_MENU_BRIGHTNESS_MIN_PERCENT..100). Fire-and-forget, and safe
 * to call at slider-drag rate: calls are coalesced (at most one
 * brightnessctl process at a time, ~25 launches/s max, always ending
 * on the latest requested value). Once the last request has been
 * applied the callback fires with that value; if applying failed, the
 * real value is re-read and reported instead. */
void extras_menu_brightness_set(ExtrasMenuBrightness *brightness, guint percent);

void extras_menu_brightness_free(ExtrasMenuBrightness *brightness);

G_END_DECLS

#endif /* EXTRAS_MENU_BRIGHTNESS_H */