#ifndef EXTRAS_MENU_NIGHT_LIGHT_CONFIG_H
#define EXTRAS_MENU_NIGHT_LIGHT_CONFIG_H

#include <glib.h>

G_BEGIN_DECLS

/*
 * The Night Light settings the preferences editor saves and the Night
 * Light pill (pills/night-light.c) reads:
 *
 *   temperature     the color temperature the screen is moved to while
 *                   Night Light is on, in Kelvin. Lower is warmer.
 *   transition_ms   how long the move to that temperature (and back
 *                   again when it is switched off) takes. 0 = at once.
 *
 * They are kept in ~/.config/xfce4/extras-menu/night-light.ini (or
 * wherever XDG_CONFIG_HOME points). Whether Night Light is currently on
 * is deliberately not saved: it always starts off.
 */

/* The color temperature that leaves the screen unchanged. */
#define EXTRAS_MENU_NIGHT_LIGHT_NEUTRAL_K 6500

#define EXTRAS_MENU_NIGHT_LIGHT_TEMPERATURE_MIN     2000
#define EXTRAS_MENU_NIGHT_LIGHT_TEMPERATURE_MAX     6000
#define EXTRAS_MENU_NIGHT_LIGHT_TEMPERATURE_DEFAULT 4000

#define EXTRAS_MENU_NIGHT_LIGHT_TRANSITION_MIN     0
#define EXTRAS_MENU_NIGHT_LIGHT_TRANSITION_MAX     3000
#define EXTRAS_MENU_NIGHT_LIGHT_TRANSITION_DEFAULT 440

/* The saved values. A missing file, a missing key or a value outside
 * the range above gives the default (or the nearest limit), so the
 * result is always usable. */
guint extras_menu_night_light_config_get_temperature(void);
guint extras_menu_night_light_config_get_transition_ms(void);

/* Saves both values (clamped to their ranges) and, on success, calls
 * every registered listener. */
gboolean extras_menu_night_light_config_save(guint temperature, guint transition_ms,
                                              GError **error);

/* Change notification, so the pill can move to a new temperature the
 * moment the preferences editor saves. Listeners run in the main
 * thread right after a successful save. Remove a listener before
 * whatever user_data points to goes away. */
typedef void (*ExtrasMenuNightLightConfigChangedFunc)(gpointer user_data);

guint extras_menu_night_light_config_add_listener(ExtrasMenuNightLightConfigChangedFunc func,
                                                   gpointer user_data);
void extras_menu_night_light_config_remove_listener(guint listener_id);

G_END_DECLS

#endif /* EXTRAS_MENU_NIGHT_LIGHT_CONFIG_H */