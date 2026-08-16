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

/* CaffeineIconTheme (AUTO/LIGHT/DARK) is declared in caffeine-icons.h,
 * which this file includes above - kept there since it's fundamentally
 * about which icon files get loaded, and caffeine-icons.h has no
 * dependency on the rest of this file. */
/*
 * Screen-off-only timer setting - independent of, and can run alongside,
 * the lock cycle above. While Caffeine is ON, this can turn the monitor
 * off (DPMS force off) on a repeating schedule of its own, WITHOUT ever
 * locking the session (no xflock4 call at all) - the computer keeps
 * running, only the display goes dark. Any user input wakes the monitor
 * again through the normal DPMS/X mechanism, and the plugin's own timer
 * simply restarts for the next cycle.
 */
typedef enum
{
    CAFFEINE_SCREEN_OFF_NEVER = 0,   /* disabled - screen only blanks via the lock cycle, if that's on */
    CAFFEINE_SCREEN_OFF_5MIN = 5,
    CAFFEINE_SCREEN_OFF_10MIN = 10,
    CAFFEINE_SCREEN_OFF_15MIN = 15,
    CAFFEINE_SCREEN_OFF_30MIN = 30,
    CAFFEINE_SCREEN_OFF_CUSTOM = -1  /* use screen_off_custom_minutes field instead */
} CaffeineScreenOffMode;

typedef struct
{
    CaffeineLockCycleMode mode;
    guint                 custom_minutes; /* only meaningful when mode == CUSTOM, >= 1 */
    CaffeineIconTheme     icon_theme;     /* which custom icon variant to load */

    gboolean              screen_off_enabled;
    CaffeineScreenOffMode screen_off_mode;
    guint                 screen_off_custom_minutes; /* only meaningful when screen_off_mode == CUSTOM, >= 1 */
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
    gint              icon_pixel_size;  /* last size the panel reported via
                                          * size-changed (or the button's initial
                                          * request); used to (re)load icons at the
                                          * right size without depending on
                                          * gtk_widget_get_allocation(), which can
                                          * still read 0 very early on before the
                                          * widget has been through GTK's full
                                          * size-allocate pass */

    /* preferences: self-triggered lock+blank cycle while caffeine is on */
    CaffeineSettings settings;
    gchar     *xfconf_channel_name; /* unique per plugin instance, for xfconf */
    guint      lock_cycle_timer_id; /* 0 = no cycle running (mode NEVER or caffeine off) */

    /* preferences: screen-off-only timer, independent of the lock cycle -
     * see CaffeineScreenOffMode in this file for what it does */
    guint      screen_off_timer_id; /* 0 = no timer running (disabled or caffeine off) */

    /* signal handler id for GtkSettings::notify::gtk-application-prefer-
     * dark-theme, so AUTO icon theme mode follows live system theme
     * changes (and corrects itself if the theme wasn't settled yet at
     * construct time). 0 = not connected. */
    gulong     theme_notify_handler_id;

} CaffeinePlugin;

void caffeine_construct (XfcePanelPlugin *plugin);

G_END_DECLS

#endif /* !__CAFFEINE_H__ */