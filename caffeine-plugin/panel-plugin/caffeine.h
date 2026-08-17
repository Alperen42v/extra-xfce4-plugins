#ifndef __CAFFEINE_H__
#define __CAFFEINE_H__

#include <gtk/gtk.h>
#include <libxfce4panel/xfce-panel-plugin.h>
#include <libxfce4panel/xfce-panel-convenience.h>
#include <libxfce4panel/xfce-panel-macros.h>

#include "caffeine-icons.h"

G_BEGIN_DECLS

/*
 * Lock-cycle duration setting - see caffeine-prefs.h/.c for the prefs
 * dialog and xfconf storage. Independent of caffeine's own on/off state:
 * while ON, caffeine repeats its own lock+blank cycle (xflock4, then
 * DPMS off shortly after) on this interval. If NEVER, caffeine just
 * inhibits indefinitely and never locks on its own.
 */
typedef enum
{
    CAFFEINE_LOCK_CYCLE_NEVER = 0,   /* pure inhibit, no self-triggered lock */
    CAFFEINE_LOCK_CYCLE_15MIN = 15,
    CAFFEINE_LOCK_CYCLE_30MIN = 30,
    CAFFEINE_LOCK_CYCLE_60MIN = 60,
    CAFFEINE_LOCK_CYCLE_CUSTOM = -1  /* use custom_minutes field instead */
} CaffeineLockCycleMode;

/* CaffeineIconTheme (AUTO/LIGHT/DARK) is declared in caffeine-icons.h. */

/*
 * Screen-off-only timer setting - independent of, and can run alongside,
 * the lock cycle above. While ON, this can DPMS-force the monitor off on
 * a repeating schedule WITHOUT ever locking the session. Any user input
 * wakes it again through normal DPMS/X.
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

    /* custom user-provided PNG icons - NULL/0 means "not provided",
     * triggering the Cairo-drawn fallback */
    CaffeineIconSet *icons;
    guint             icon_frame_index; /* which on_frames[] entry is showing now */
    gint              icon_pixel_size;  /* last size reported via size-changed
                                          * (or the button's initial request) */

    /* preferences: self-triggered lock+blank cycle while caffeine is on */
    CaffeineSettings settings;
    gchar     *xfconf_channel_name; /* unique per plugin instance, for xfconf */
    guint      lock_cycle_timer_id; /* 0 = no cycle running (mode NEVER or caffeine off) */

    /* preferences: screen-off-only timer, independent of the lock cycle */
    guint      screen_off_timer_id; /* 0 = no timer running (disabled or caffeine off) */

    /* handler id for GtkSettings::notify::gtk-application-prefer-dark-theme,
     * so AUTO icon theme follows live system theme changes. 0 = not connected. */
    gulong     theme_notify_handler_id;

} CaffeinePlugin;

void caffeine_construct (XfcePanelPlugin *plugin);

G_END_DECLS

#endif /* !__CAFFEINE_H__ */