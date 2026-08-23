/*
 * nightlight.h - Shared definitions (used by both the main plugin file
 * and the preferences dialog).
 */

#ifndef NIGHTLIGHT_H
#define NIGHTLIGHT_H

#include <gtk/gtk.h>
#include <libxfce4panel/libxfce4panel.h>

G_BEGIN_DECLS

#define NL_TEMP_NEUTRAL   6500   /* reference value used when night light is off */
#define NL_TEMP_MIN       1000   /* strongest / warmest */
#define NL_TEMP_MAX       6500   /* weakest / natural white */
#define NL_DEFAULT_STRENGTH 60   /* default strength, 0-100 */
#define NL_DEFAULT_TRANSITION_MS 2500
#define NL_SCHEDULER_INTERVAL_S  1   /* how often the scheduler re-checks the clock */
#define NL_ANIM_STEP_MS 40

typedef struct
{
    XfcePanelPlugin *plugin;

    GtkWidget *button;
    GtkWidget *icon_image;

    /* --- Settings (persisted) --- */
    gboolean master_enabled;     /* is the plugin enabled at all */
    gboolean schedule_enabled;   /* is automatic scheduling on */
    gint     strength;           /* 0-100, warm light strength */
    gint     start_hour, start_minute;
    gint     end_hour, end_minute;
    gboolean use_24h;
    gint     transition_ms;      /* transition animation duration */
    gboolean manual_override;    /* did the user manually toggle via the panel icon */
    gboolean manual_state;       /* target state while in manual override */

    /* --- Runtime state --- */
    gboolean is_active;          /* is night light currently applied */
    gint     current_temp;       /* temperature (K) currently applied to the screen */
    guint    scheduler_source;
    guint    anim_source;
    gint     anim_from;
    gint     anim_to;
    gint64   anim_started_at;    /* g_get_monotonic_time() */

    GtkWidget *dialog;           /* reference to the preferences dialog, if open */
} NightlightPlugin;

/* Defined in nightlight.c, also used by nightlight-prefs.c */
gint     strength_to_kelvin   (gint strength);
void     start_transition     (NightlightPlugin *nl, gint target_temp);
void     save_config          (NightlightPlugin *nl);
gboolean scheduler_tick       (gpointer data);
gboolean time_in_range        (gint cur_min, gint start_min, gint end_min);

/* Defined in nightlight-prefs.c, called from nightlight.c */
void nightlight_show_settings (XfcePanelPlugin *plugin, NightlightPlugin *nl);

G_END_DECLS

#endif /* NIGHTLIGHT_H */