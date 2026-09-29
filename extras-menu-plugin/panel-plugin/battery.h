#ifndef EXTRAS_MENU_BATTERY_H
#define EXTRAS_MENU_BATTERY_H

#include <glib.h>

G_BEGIN_DECLS

typedef struct _ExtrasMenuBattery ExtrasMenuBattery;

typedef enum
{
    EXTRAS_MENU_BATTERY_STATE_UNKNOWN,
    EXTRAS_MENU_BATTERY_STATE_CHARGING,
    EXTRAS_MENU_BATTERY_STATE_DISCHARGING,
    EXTRAS_MENU_BATTERY_STATE_FULL,
    /* AC connected but not charging (e.g. a charge-limit threshold). */
    EXTRAS_MENU_BATTERY_STATE_PLUGGED
} ExtrasMenuBatteryState;

/* Called once the backend has a reading, and again on every change
 * (percentage, charging state, battery appearing/disappearing, UPower
 * itself starting/stopping).
 *
 * present:   FALSE on machines without a battery (desktops), or when
 *            UPower isn't running -- the UI should hide the badge.
 * percent:   0-100, only meaningful when present.
 * state:     see enum above.
 * seconds:   estimated time until full (while charging) or until
 *            empty (while discharging); 0 when unknown. */
typedef void (*ExtrasMenuBatteryChangedFunc)(gboolean present,
                                              guint percent,
                                              ExtrasMenuBatteryState state,
                                              gint64 seconds,
                                              gpointer user_data);

/* Connects to UPower's "DisplayDevice" (the aggregate battery, which
 * is what desktop environments show) over the system bus. Async:
 * results arrive through the callback from the main loop. */
ExtrasMenuBattery *extras_menu_battery_new(ExtrasMenuBatteryChangedFunc callback,
                                            gpointer user_data);

void extras_menu_battery_free(ExtrasMenuBattery *battery);

/* Themed symbolic icon name for a reading, e.g.
 * "battery-good-charging-symbolic". Caller frees with g_free(). */
gchar *extras_menu_battery_icon_name(guint percent, ExtrasMenuBatteryState state);

/* Human-readable tooltip, e.g. "Charging - 1 h 20 min until full".
 * Caller frees with g_free(). */
gchar *extras_menu_battery_describe(guint percent, ExtrasMenuBatteryState state,
                                     gint64 seconds);

G_END_DECLS

#endif /* EXTRAS_MENU_BATTERY_H */