#ifndef __CAFFEINE_H__
#define __CAFFEINE_H__

#include <gtk/gtk.h>
#include <libxfce4panel/xfce-panel-plugin.h>
#include <libxfce4panel/xfce-panel-convenience.h>
#include <libxfce4panel/xfce-panel-macros.h>

#include "caffeine-icons.h"

G_BEGIN_DECLS

/*
 * Lock-cycle duration setting - see caffeine-prefs.h/.c for the
 * preferences dialog and xfconf storage that manage this. Declared here
 * (rather than only in caffeine-prefs.h) so caffeine.c can embed a
 * CaffeineSettings in CaffeinePlugin without a circular include, since
 * caffeine-prefs.h itself includes caffeine.h for XfcePanelPlugin/CaffeinePlugin.
 *
 * This setting is independent of caffeine's own on/off state: it does not
 * change how long caffeine stays active. While caffeine is ON, it drives
 * its own repeating lock+blank cycle (same actions xfce4-power-manager
 * would trigger on inactivity - xflock4, then DPMS off shortly after) on
 * this interval, then keeps going. If NEVER, caffeine just inhibits
 * indefinitely as before and never locks on its own.
 */
typedef enum
{
    CAFFEINE_LOCK_CYCLE_NEVER = 0,   /* pure inhibit, no self-triggered lock */
    CAFFEINE_LOCK_CYCLE_15MIN = 15,
    CAFFEINE_LOCK_CYCLE_30MIN = 30,
    CAFFEINE_LOCK_CYCLE_60MIN = 60,
    CAFFEINE_LOCK_CYCLE_CUSTOM = -1  /* use custom_minutes field instead */
} CaffeineLockCycleMode;

typedef struct
{
    CaffeineLockCycleMode mode;
    guint                 custom_minutes; /* only meaningful when mode == CUSTOM, >= 1 */
} CaffeineSettings;

typedef struct
{
    XfcePanelPlugin *plugin;

    /* widgets */
    GtkWidget *button;
    GtkWidget *icon_area;      /* GtkDrawingArea, drawn with Cairo */

    /* state */
    gboolean   active;         /* TRUE = caffeine ON (screensaver+DPMS inhibited) */

    /* org.*.ScreenSaver Inhibit — stops the lock-screen/screensaver */
    guint      ss_cookie;      /* 0 = not currently inhibited */
    gchar     *ss_backend;     /* which bus name worked, for UnInhibit + tooltip */
    gchar     *ss_path;        /* object path that worked, for UnInhibit */
    gchar     *ss_iface;       /* interface name that worked, for UnInhibit */

    /* org.freedesktop.PowerManagement.Inhibit — stops DPMS/monitor sleep */
    guint      pm_cookie;      /* 0 = not currently inhibited */
    gchar     *pm_backend;
    gchar     *pm_path;
    gchar     *pm_iface;

    guint      animation_timer_id;
    gdouble    animation_phase; /* 0..1, drives the steam wobble (Cairo fallback only) */

    /* custom user-provided PNG icons (see caffeine-icons.h) - NULL/0
     * fields mean "not provided", triggering the Cairo-drawn fallback */
    CaffeineIconSet *icons;
    guint             icon_frame_index; /* which on_frames[] entry is showing now */

    /* preferences: self-triggered lock+blank cycle while caffeine is on */
    CaffeineSettings settings;
    gchar     *xfconf_channel_name; /* unique per plugin instance, for xfconf */
    guint      lock_cycle_timer_id; /* 0 = no cycle running (mode NEVER or caffeine off) */

} CaffeinePlugin;

void caffeine_construct (XfcePanelPlugin *plugin);

G_END_DECLS

#endif /* !__CAFFEINE_H__ */