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

/* Fired once after the initial brightness has been read (shortly
 * after extras_menu_brightness_new() returns), and again after every
 * extras_menu_brightness_set() call completes. percent is 0-100.
 * Not fired at all if brightnessctl is missing or no backlight device
 * is found -- callers should keep the brightness row visually
 * reasonable in that case (e.g. leave it at its default position). */
typedef void (*ExtrasMenuBrightnessChangedFunc)(guint percent, gpointer user_data);

ExtrasMenuBrightness *extras_menu_brightness_new(ExtrasMenuBrightnessChangedFunc callback,
                                                  gpointer user_data);

/* Requests brightness be set to percent (0-100, clamped). Fire-and-
 * forget: the resulting value comes back through the callback once
 * brightnessctl finishes and we re-read the actual value. */
void extras_menu_brightness_set(ExtrasMenuBrightness *brightness, guint percent);

void extras_menu_brightness_free(ExtrasMenuBrightness *brightness);

G_END_DECLS

#endif /* EXTRAS_MENU_BRIGHTNESS_H */