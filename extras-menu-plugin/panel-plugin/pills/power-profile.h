#ifndef EXTRAS_MENU_POWER_PROFILE_H
#define EXTRAS_MENU_POWER_PROFILE_H

#include <gtk/gtk.h>

G_BEGIN_DECLS

/*
 * The "Balanced" pill's logic: switches the system's power profile
 * (Power Saver / Balanced / Performance) through power-profiles-daemon.
 *
 * Each click moves to the next profile the machine supports (Performance
 * is missing on many laptops and is simply skipped), wrapping around.
 * The pill's icon and label show the active profile, and it follows
 * changes made elsewhere (powerprofilesctl, another app) by itself.
 * Like the Aeroplane Mode and microphone modules, it owns all of its
 * state and keeps hold of the pill's widgets itself, so the plugin needs
 * nothing but the one pointer returned here.
 *
 * Changing the profile needs no root: the daemon is reached over the
 * system bus as the logged-in user. If the daemon is not installed or
 * not running, the pill is greyed out with a tooltip saying so.
 */
typedef struct _ExtrasMenuPowerProfile ExtrasMenuPowerProfile;

/* `pill` is the Balanced pill's GtkToggleButton, as handed out by
 * extras_menu_popover_get_pill(content, EXTRAS_MENU_PILL_BALANCED).
 * Returns NULL if the pill doesn't have the expected icon + label inside. */
ExtrasMenuPowerProfile *extras_menu_power_profile_new(GtkWidget *pill);

/* Stops everything and disconnects from the pill. NULL is allowed. */
void extras_menu_power_profile_free(ExtrasMenuPowerProfile *self);

G_END_DECLS

#endif /* EXTRAS_MENU_POWER_PROFILE_H */