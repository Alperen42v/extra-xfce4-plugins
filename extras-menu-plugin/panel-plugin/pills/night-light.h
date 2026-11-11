#ifndef EXTRAS_MENU_NIGHT_LIGHT_H
#define EXTRAS_MENU_NIGHT_LIGHT_H

#include <gtk/gtk.h>

G_BEGIN_DECLS

/*
 * The Night Light pill's logic: turns the screen's colors warmer (less
 * blue) and back again, moving between the two smoothly.
 *
 * Switching the pill on moves the screen from its normal colors to the
 * saved color temperature (default 4000 K); switching it off moves it
 * back. Both moves take the saved transition time (default 440 ms; 0
 * means at once). Both values are set in the preferences editor's
 * "Night light" section (see preferences/night-light-config.h), and a
 * change made there is applied right away: while Night Light is on, the
 * screen moves to the new temperature.
 *
 * It works through the X server's gamma ramps (the RandR extension), the
 * same way tools like redshift do, so it needs no helper program, but
 * it needs an X11 session. Without one the pill is greyed out and says
 * why in its tooltip. The ramps that were in place (a calibration
 * profile, say) are remembered while Night Light is on and put back
 * exactly when it goes off, or when the plugin is removed.
 *
 * Night Light always starts off; whether it was on is not remembered.
 *
 * Like the other pill modules, it owns all of its state and keeps hold
 * of the pill itself, so the plugin needs nothing but the one pointer.
 */
typedef struct _ExtrasMenuNightLight ExtrasMenuNightLight;

/* `pill` is the Night Light pill's GtkToggleButton, as handed out by
 * extras_menu_popover_get_pill(content, EXTRAS_MENU_PILL_NIGHT_LIGHT).
 * Returns NULL if `pill` isn't a toggle button. */
ExtrasMenuNightLight *extras_menu_night_light_new(GtkWidget *pill);

/* Puts the screen's colors back, stops listening to the pill and the
 * settings. NULL is allowed. */
void extras_menu_night_light_free(ExtrasMenuNightLight *self);

G_END_DECLS

#endif /* EXTRAS_MENU_NIGHT_LIGHT_H */