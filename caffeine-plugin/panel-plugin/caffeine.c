/*
 * Caffeine Plugin for the Xfce Panel
 *
 * Left-click toggles "keep awake" mode using the standard
 * org.freedesktop.ScreenSaver DBus Inhibit/UnInhibit interface.
 */

#include <math.h>
#include <gio/gio.h>
#include <glib/gstdio.h>

#include "caffeine.h"
#include "caffeine-prefs.h"
#include "caffeine-icons.h"

#define BORDER 4
#define ANIM_INTERVAL_MS 60   /* ~16 fps */
#define LOCK_TO_BLANK_DELAY_SEC 7   /* gap between xflock4 and forcing DPMS off */

/* ---------------------------------------------------------------------- */
/* DBus screensaver inhibit helpers                                       */
/* ---------------------------------------------------------------------- */

/*
 * Different desktops expose Inhibit/UnInhibit under different bus
 * names/paths/interfaces (xfce4-power-manager in particular does not
 * mirror its bus name into its object path). We inhibit both the
 * screensaver and the power manager's DPMS independently, and remember
 * which target worked for each so UnInhibit hits the same one.
 */
typedef struct
{
    const gchar *bus_name;
    const gchar *object_path;
    const gchar *interface_name;
} InhibitTarget;

static const InhibitTarget screensaver_targets[] = {
    { "org.freedesktop.ScreenSaver", "/org/freedesktop/ScreenSaver", "org.freedesktop.ScreenSaver" },
    { "org.xfce.ScreenSaver", "/org/xfce/ScreenSaver", "org.xfce.ScreenSaver" },
    { "org.gnome.ScreenSaver", "/org/gnome/ScreenSaver", "org.gnome.ScreenSaver" },
    { NULL, NULL, NULL }
};

static const InhibitTarget power_targets[] = {
    { "org.freedesktop.PowerManagement", "/org/freedesktop/PowerManagement/Inhibit", "org.freedesktop.PowerManagement.Inhibit" },
    { NULL, NULL, NULL }
};

/* Tries each target in turn until Inhibit() succeeds; out_* are set to
 * newly-allocated copies of the target that worked (caller frees). */
static gboolean
try_inhibit (GDBusConnection *conn, const InhibitTarget *targets,
             guint *out_cookie, gchar **out_backend,
             gchar **out_path, gchar **out_iface)
{
    GError *error = NULL;
    gint    i;

    for (i = 0; targets[i].bus_name != NULL; i++)
    {
        const InhibitTarget *t = &targets[i];
        GVariant *result;
        guint32   cookie = 0;

        g_clear_error (&error);
        result = g_dbus_connection_call_sync (conn,
                                               t->bus_name,
                                               t->object_path,
                                               t->interface_name,
                                               "Inhibit",
                                               g_variant_new ("(ss)",
                                                              "xfce4-caffeine-plugin",
                                                              "User requested to keep the screen awake"),
                                               G_VARIANT_TYPE ("(u)"),
                                               G_DBUS_CALL_FLAGS_NONE,
                                               3000, NULL, &error);

        if (result != NULL)
        {
            g_variant_get (result, "(u)", &cookie);
            g_variant_unref (result);

            *out_cookie = cookie;
            *out_backend = g_strdup (t->bus_name);
            *out_path = g_strdup (t->object_path);
            *out_iface = g_strdup (t->interface_name);
            return TRUE;
        }

        g_debug ("Caffeine: Inhibit via %s (%s, %s) failed: %s",
                 t->bus_name, t->object_path, t->interface_name,
                 error ? error->message : "unknown error");
    }

    g_clear_error (&error);
    return FALSE;
}

static void
try_uninhibit (GDBusConnection *conn, guint cookie,
               const gchar *backend, const gchar *object_path, const gchar *interface_name)
{
    GError *error = NULL;

    if (cookie == 0 || backend == NULL || object_path == NULL || interface_name == NULL)
        return;

    g_dbus_connection_call_sync (conn,
                                  backend,
                                  object_path,
                                  interface_name,
                                  "UnInhibit",
                                  g_variant_new ("(u)", cookie),
                                  NULL,
                                  G_DBUS_CALL_FLAGS_NONE,
                                  3000, NULL, &error);

    if (error != NULL)
    {
        g_warning ("Caffeine: UnInhibit via %s (%s, %s) failed: %s",
                   backend, object_path, interface_name, error->message);
        g_clear_error (&error);
    }
}

/* Returns TRUE if at least one of screensaver/power-management inhibit
 * succeeded; logs a warning for whichever one failed. */
static gboolean
caffeine_dbus_inhibit (CaffeinePlugin *caffeine)
{
    GDBusConnection *conn;
    GError          *error = NULL;
    gboolean         ss_ok, pm_ok;

    conn = g_bus_get_sync (G_BUS_TYPE_SESSION, NULL, &error);
    if (conn == NULL)
    {
        g_warning ("Caffeine: could not connect to session bus: %s", error->message);
        g_clear_error (&error);
        return FALSE;
    }

    ss_ok = try_inhibit (conn, screensaver_targets, &caffeine->ss_cookie, &caffeine->ss_backend,
                         &caffeine->ss_path, &caffeine->ss_iface);
    pm_ok = try_inhibit (conn, power_targets, &caffeine->pm_cookie, &caffeine->pm_backend,
                         &caffeine->pm_path, &caffeine->pm_iface);

    g_object_unref (conn);

    if (!ss_ok)
        g_warning ("Caffeine: could not inhibit screensaver on any known bus name");
    if (!pm_ok)
        g_warning ("Caffeine: could not inhibit power management on any known bus name");

    return ss_ok || pm_ok;
}

static void
caffeine_dbus_uninhibit (CaffeinePlugin *caffeine)
{
    GDBusConnection *conn;
    GError          *error = NULL;

    if (caffeine->ss_cookie == 0 && caffeine->pm_cookie == 0)
        return;

    conn = g_bus_get_sync (G_BUS_TYPE_SESSION, NULL, &error);
    if (conn == NULL)
    {
        g_warning ("Caffeine: could not connect to session bus: %s", error->message);
        g_clear_error (&error);
        return;
    }

    try_uninhibit (conn, caffeine->ss_cookie, caffeine->ss_backend, caffeine->ss_path, caffeine->ss_iface);
    try_uninhibit (conn, caffeine->pm_cookie, caffeine->pm_backend, caffeine->pm_path, caffeine->pm_iface);

    g_object_unref (conn);

    caffeine->ss_cookie = 0;
    caffeine->pm_cookie = 0;

    g_free (caffeine->ss_backend); caffeine->ss_backend = NULL;
    g_free (caffeine->ss_path);    caffeine->ss_path = NULL;
    g_free (caffeine->ss_iface);   caffeine->ss_iface = NULL;
    g_free (caffeine->pm_backend); caffeine->pm_backend = NULL;
    g_free (caffeine->pm_path);    caffeine->pm_path = NULL;
    g_free (caffeine->pm_iface);   caffeine->pm_iface = NULL;
}

/* ---------------------------------------------------------------------- */
/* Drawing: cup + optional animated steam                                 */
/* ---------------------------------------------------------------------- */

static void
draw_cup (cairo_t *cr, gint w, gint h, gboolean active)
{
    gdouble cup_left, cup_right, cup_top, cup_bottom;
    gdouble handle_cx, handle_cy, handle_r;

    cup_left   = w * 0.22;
    cup_right  = w * 0.72;
    cup_top    = h * 0.45;
    cup_bottom = h * 0.85;

    if (active)
        cairo_set_source_rgb (cr, 0.98, 0.75, 0.15); /* amber/yellow */
    else
        cairo_set_source_rgb (cr, 0.95, 0.95, 0.95); /* near-white */

    /* cup body */
    cairo_new_path (cr);
    cairo_move_to (cr, cup_left, cup_top);
    cairo_line_to (cr, cup_right, cup_top);
    cairo_line_to (cr, cup_right - 3, cup_bottom - 6);
    cairo_curve_to (cr,
                     cup_right - 3, cup_bottom,
                     cup_left + 3, cup_bottom,
                     cup_left + 3, cup_bottom - 6);
    cairo_close_path (cr);
    cairo_fill_preserve (cr);

    /* outline */
    cairo_set_source_rgb (cr, 0.15, 0.15, 0.15);
    cairo_set_line_width (cr, 1.6);
    cairo_stroke (cr);

    /* handle */
    handle_cx = cup_right + (w * 0.09);
    handle_cy = (cup_top + cup_bottom) / 2.0;
    handle_r  = h * 0.14;

    cairo_new_path (cr);
    cairo_arc (cr, handle_cx, handle_cy, handle_r, -M_PI * 0.65, M_PI * 0.65);
    cairo_set_line_width (cr, 3.0);
    if (active)
        cairo_set_source_rgb (cr, 0.98, 0.75, 0.15);
    else
        cairo_set_source_rgb (cr, 0.95, 0.95, 0.95);
    cairo_stroke_preserve (cr);
    cairo_set_source_rgb (cr, 0.15, 0.15, 0.15);
    cairo_set_line_width (cr, 1.0);
    cairo_stroke (cr);

    /* saucer */
    cairo_new_path (cr);
    cairo_move_to (cr, cup_left - 4, cup_bottom + 2);
    cairo_line_to (cr, cup_right + 4, cup_bottom + 2);
    cairo_set_source_rgb (cr, 0.15, 0.15, 0.15);
    cairo_set_line_width (cr, 2.2);
    cairo_stroke (cr);
}

/* Wavy steam wisp: sine-perturbed strokes drifting upward and fading
 * based on the animation phase. */
static void
draw_steam (cairo_t *cr, gint w, gint h, gdouble phase)
{
    gint i;
    gdouble base_x[2];
    base_x[0] = w * 0.38;
    base_x[1] = w * 0.55;

    cairo_set_line_cap (cr, CAIRO_LINE_CAP_ROUND);

    for (i = 0; i < 2; i++)
    {
        gdouble x0 = base_x[i];
        gdouble y0 = h * 0.40;
        gdouble local_phase = phase + (i * 0.5);
        gdouble alpha = 0.55 + 0.35 * sin (local_phase * 2.0 * M_PI);
        gint j;

        if (alpha < 0.15) alpha = 0.15;

        cairo_set_source_rgba (cr, 0.85, 0.85, 0.85, alpha);
        cairo_set_line_width (cr, 1.8);

        cairo_new_path (cr);
        cairo_move_to (cr, x0, y0);

        for (j = 1; j <= 4; j++)
        {
            gdouble t = j / 4.0;
            gdouble y = y0 - t * (h * 0.32);
            gdouble wobble = sin ((local_phase * 2.0 * M_PI) + (t * 4.0)) * (w * 0.045);
            cairo_line_to (cr, x0 + wobble, y);
        }
        cairo_stroke (cr);
    }
}

static gboolean
on_icon_draw (GtkWidget *widget, cairo_t *cr, gpointer user_data)
{
    CaffeinePlugin *caffeine = (CaffeinePlugin *) user_data;
    GtkAllocation alloc;
    GdkPixbuf *frame_to_draw = NULL;
    gtk_widget_get_allocation (widget, &alloc);

    gint w = alloc.width;
    gint h = alloc.height;

    /* prefer custom icons; fall back to the Cairo-drawn cup if missing */
    if (caffeine->icons != NULL)
    {
        if (caffeine->active && caffeine->icons->on_frame_count > 0)
            frame_to_draw = caffeine->icons->on_frames[caffeine->icon_frame_index % caffeine->icons->on_frame_count];
        else if (!caffeine->active && caffeine->icons->off_frame != NULL)
            frame_to_draw = caffeine->icons->off_frame;
    }

    if (frame_to_draw != NULL)
    {
        cairo_save (cr);
        gdk_cairo_set_source_pixbuf (cr, frame_to_draw, 0, 0);
        cairo_paint (cr);
        cairo_restore (cr);
        return FALSE;
    }

    cairo_save (cr);
    draw_cup (cr, w, h, caffeine->active);
    cairo_restore (cr);

    if (caffeine->active)
    {
        cairo_save (cr);
        draw_steam (cr, w, h, caffeine->animation_phase);
        cairo_restore (cr);
    }

    return FALSE;
}

static gboolean
on_animation_tick (gpointer user_data)
{
    CaffeinePlugin *caffeine = (CaffeinePlugin *) user_data;

    caffeine->animation_phase += 0.03;
    if (caffeine->animation_phase > 1.0)
        caffeine->animation_phase -= 1.0;

    if (caffeine->icons != NULL && caffeine->icons->on_frame_count > 0)
        caffeine->icon_frame_index = (caffeine->icon_frame_index + 1) % caffeine->icons->on_frame_count;

    gtk_widget_queue_draw (caffeine->icon_area);

    return G_SOURCE_CONTINUE;
}

/* ---------------------------------------------------------------------- */
/* Self-triggered lock+blank cycle (independent of caffeine on/off state) */
/* ---------------------------------------------------------------------- */

/* Mimics xfce4-power-manager's inactivity behaviour (lock, then blank a
 * few seconds later) but on a fixed schedule while caffeine stays on;
 * does not touch the inhibit cookies above. */

static gboolean
on_blank_after_lock (gpointer user_data)
{
    CaffeinePlugin *caffeine = (CaffeinePlugin *) user_data;
    GError *error = NULL;

    if (!g_spawn_command_line_async ("xset dpms force off", &error))
    {
        g_warning ("Caffeine: failed to blank monitor via xset: %s", error->message);
        g_clear_error (&error);
    }

    (void) caffeine;
    return G_SOURCE_REMOVE;
}

static gboolean
on_lock_cycle_tick (gpointer user_data)
{
    CaffeinePlugin *caffeine = (CaffeinePlugin *) user_data;
    GError *error = NULL;

    if (!g_spawn_command_line_async ("xflock4", &error))
    {
        g_warning ("Caffeine: failed to lock screen via xflock4: %s", error->message);
        g_clear_error (&error);
    }

    g_timeout_add_seconds (LOCK_TO_BLANK_DELAY_SEC, on_blank_after_lock, caffeine);

    return G_SOURCE_CONTINUE;
}

/* (Re)starts the lock cycle timer from current settings. No-op if mode
 * is NEVER or caffeine isn't active; always clears any existing timer. */
static void
caffeine_lock_cycle_restart (CaffeinePlugin *caffeine)
{
    guint interval_min;

    if (caffeine->lock_cycle_timer_id != 0)
    {
        g_source_remove (caffeine->lock_cycle_timer_id);
        caffeine->lock_cycle_timer_id = 0;
    }

    if (!caffeine->active)
        return;

    interval_min = caffeine_settings_get_interval_minutes (&caffeine->settings);
    if (interval_min == 0)
        return; /* NEVER, or invalid custom value */

    caffeine->lock_cycle_timer_id =
        g_timeout_add_seconds (interval_min * 60, on_lock_cycle_tick, caffeine);
}

static void
caffeine_lock_cycle_stop (CaffeinePlugin *caffeine)
{
    if (caffeine->lock_cycle_timer_id != 0)
    {
        g_source_remove (caffeine->lock_cycle_timer_id);
        caffeine->lock_cycle_timer_id = 0;
    }
}

/* ---------------------------------------------------------------------- */
/* Self-triggered screen-off-only timer (independent of the lock cycle)   */
/* ---------------------------------------------------------------------- */

/* Unlike the lock cycle, this never calls xflock4 - only DPMS force off.
 * Waking the monitor is left to normal DPMS/X input handling. */

static gboolean
on_screen_off_tick (gpointer user_data)
{
    CaffeinePlugin *caffeine = (CaffeinePlugin *) user_data;
    GError *error = NULL;

    if (!g_spawn_command_line_async ("xset dpms force off", &error))
    {
        g_warning ("Caffeine: failed to turn off screen via xset: %s", error->message);
        g_clear_error (&error);
    }

    (void) caffeine;

    return G_SOURCE_CONTINUE;
}

/* (Re)starts the screen-off timer from current settings. No-op if
 * disabled or caffeine isn't active; always clears any existing timer. */
static void
caffeine_screen_off_restart (CaffeinePlugin *caffeine)
{
    guint interval_min;

    if (caffeine->screen_off_timer_id != 0)
    {
        g_source_remove (caffeine->screen_off_timer_id);
        caffeine->screen_off_timer_id = 0;
    }

    if (!caffeine->active)
        return;

    interval_min = caffeine_settings_get_screen_off_interval_minutes (&caffeine->settings);
    if (interval_min == 0)
        return; /* disabled, or invalid custom value */

    caffeine->screen_off_timer_id =
        g_timeout_add_seconds (interval_min * 60, on_screen_off_tick, caffeine);
}

static void
caffeine_screen_off_stop (CaffeinePlugin *caffeine)
{
    if (caffeine->screen_off_timer_id != 0)
    {
        g_source_remove (caffeine->screen_off_timer_id);
        caffeine->screen_off_timer_id = 0;
    }
}

/* ---------------------------------------------------------------------- */
/* Toggle state on/off                                                    */
/* ---------------------------------------------------------------------- */

static void
caffeine_set_active (CaffeinePlugin *caffeine, gboolean active)
{
    if (caffeine->active == active)
        return;

    if (active)
    {
        if (!caffeine_dbus_inhibit (caffeine))
        {
            gtk_widget_set_tooltip_text (caffeine->button,
                "Caffeine: failed to inhibit screensaver/power manager (DBus error)");
            return;
        }

        caffeine->active = TRUE;
        caffeine->animation_phase = 0.0;
        caffeine->animation_timer_id =
            g_timeout_add (ANIM_INTERVAL_MS, on_animation_tick, caffeine);

        caffeine_lock_cycle_restart (caffeine);
        caffeine_screen_off_restart (caffeine);

        gtk_widget_set_tooltip_text (caffeine->button,
            "Caffeine: ON — screen will stay awake (click to disable)");
    }
    else
    {
        caffeine_dbus_uninhibit (caffeine);
        caffeine->active = FALSE;

        if (caffeine->animation_timer_id != 0)
        {
            g_source_remove (caffeine->animation_timer_id);
            caffeine->animation_timer_id = 0;
        }

        caffeine_lock_cycle_stop (caffeine);
        caffeine_screen_off_stop (caffeine);

        gtk_widget_set_tooltip_text (caffeine->button,
            "Caffeine: OFF — click to keep the screen awake");
    }

    gtk_widget_queue_draw (caffeine->icon_area);
}

static void
on_button_clicked (GtkWidget *widget, gpointer user_data)
{
    CaffeinePlugin *caffeine = (CaffeinePlugin *) user_data;
    caffeine_set_active (caffeine, !caffeine->active);
}

/* ---------------------------------------------------------------------- */
/* Plugin lifecycle                                                       */
/* ---------------------------------------------------------------------- */

static void
caffeine_free (XfcePanelPlugin *plugin, CaffeinePlugin *caffeine)
{
    if (caffeine->animation_timer_id != 0)
        g_source_remove (caffeine->animation_timer_id);

    if (caffeine->theme_notify_handler_id != 0)
    {
        GtkSettings *gtk_settings = gtk_settings_get_default ();
        if (gtk_settings != NULL)
            g_signal_handler_disconnect (gtk_settings, caffeine->theme_notify_handler_id);
        caffeine->theme_notify_handler_id = 0;
    }

    caffeine_lock_cycle_stop (caffeine);
    caffeine_screen_off_stop (caffeine);

    if (caffeine->active)
        caffeine_dbus_uninhibit (caffeine);

    g_free (caffeine->ss_backend);
    g_free (caffeine->ss_path);
    g_free (caffeine->ss_iface);
    g_free (caffeine->pm_backend);
    g_free (caffeine->pm_path);
    g_free (caffeine->pm_iface);
    g_free (caffeine->xfconf_channel_name);
    caffeine_icon_set_free (caffeine->icons);
    g_free (caffeine);
}

/* Reloads the custom icon set at the icon area's current pixel size.
 * Used on live theme changes, preference changes, and panel resizes. */
static void
caffeine_reload_icons (CaffeinePlugin *caffeine)
{
    /* uses the last size reported via size-changed, not
     * gtk_widget_get_allocation() which can still read 0 very early */
    if (caffeine->icon_pixel_size <= 0)
        return;

    caffeine_icon_set_free (caffeine->icons);
    caffeine->icons = caffeine_icons_load (caffeine->icon_pixel_size, caffeine->settings.icon_theme);
    caffeine->icon_frame_index = 0;
    gtk_widget_queue_draw (caffeine->icon_area);
}

static gboolean
caffeine_size_changed (XfcePanelPlugin *plugin, gint size, CaffeinePlugin *caffeine)
{
    gtk_widget_set_size_request (GTK_WIDGET (caffeine->button), size, size);
    gtk_widget_set_size_request (caffeine->icon_area, size, size);

    caffeine->icon_pixel_size = size;
    caffeine_reload_icons (caffeine);

    return TRUE;
}

/* Fires when GTK's "prefer dark theme" setting changes - covers both a
 * startup XSETTINGS race and the user switching themes live. Only acts
 * in AUTO mode; LIGHT/DARK stay pinned. */
static void
on_gtk_theme_notify (GObject *settings, GParamSpec *pspec, gpointer user_data)
{
    CaffeinePlugin *caffeine = (CaffeinePlugin *) user_data;

    (void) settings;
    (void) pspec;

    if (caffeine->settings.icon_theme == CAFFEINE_ICON_THEME_AUTO)
        caffeine_reload_icons (caffeine);
}

static void
caffeine_configure_plugin (XfcePanelPlugin *plugin, CaffeinePlugin *caffeine)
{
    if (caffeine_show_preferences (plugin, &caffeine->settings, caffeine->xfconf_channel_name))
    {
        /* restart timers immediately so new intervals take effect */
        caffeine_lock_cycle_restart (caffeine);
        caffeine_screen_off_restart (caffeine);
        caffeine_reload_icons (caffeine);
    }
}

void
caffeine_construct (XfcePanelPlugin *plugin)
{
    CaffeinePlugin *caffeine = g_new0 (CaffeinePlugin, 1);

    caffeine->plugin = plugin;
    caffeine->active = FALSE;
    caffeine->ss_cookie = 0;
    caffeine->ss_backend = NULL;
    caffeine->pm_cookie = 0;
    caffeine->pm_backend = NULL;
    caffeine->animation_timer_id = 0;
    caffeine->animation_phase = 0.0;
    caffeine->lock_cycle_timer_id = 0;
    caffeine->screen_off_timer_id = 0;
    caffeine->theme_notify_handler_id = 0;
    caffeine->icon_frame_index = 0;

    /* unique per instance so multiple panel copies don't clobber settings */
    caffeine->xfconf_channel_name =
        g_strdup_printf ("xfce4-caffeine-plugin-%d", xfce_panel_plugin_get_unique_id (plugin));

    caffeine_settings_load (&caffeine->settings, caffeine->xfconf_channel_name);

    /* real panel size right away, so icons don't stay blank waiting for
     * the first size-changed signal (some panels delay/skip it) */
    caffeine->icon_pixel_size = xfce_panel_plugin_get_size (plugin);
    if (caffeine->icon_pixel_size <= 0)
        caffeine->icon_pixel_size = 24;

    caffeine->icons = NULL;

    caffeine->button = xfce_panel_create_button ();
    gtk_button_set_relief (GTK_BUTTON (caffeine->button), GTK_RELIEF_NONE);
    gtk_widget_set_focus_on_click (caffeine->button, FALSE);

    caffeine->icon_area = gtk_drawing_area_new ();
    gtk_widget_set_size_request (caffeine->icon_area, caffeine->icon_pixel_size, caffeine->icon_pixel_size);
    gtk_container_add (GTK_CONTAINER (caffeine->button), caffeine->icon_area);

    g_signal_connect (caffeine->icon_area, "draw",
                       G_CALLBACK (on_icon_draw), caffeine);
    g_signal_connect (caffeine->button, "clicked",
                       G_CALLBACK (on_button_clicked), caffeine);

    gtk_widget_set_tooltip_text (caffeine->button,
        "Caffeine: OFF — click to keep the screen awake");

    gtk_widget_show_all (caffeine->button);

    gtk_container_add (GTK_CONTAINER (plugin), caffeine->button);
    xfce_panel_plugin_add_action_widget (plugin, caffeine->button);

    xfce_panel_plugin_menu_show_configure (plugin);
    g_signal_connect (plugin, "configure-plugin", G_CALLBACK (caffeine_configure_plugin), caffeine);

    g_signal_connect (plugin, "free-data", G_CALLBACK (caffeine_free), caffeine);
    g_signal_connect (plugin, "size-changed", G_CALLBACK (caffeine_size_changed), caffeine);

    /* keeps AUTO icon theme in sync with live system theme changes, and
     * corrects the initial load if XSETTINGS hadn't landed yet */
    {
        GtkSettings *gtk_settings = gtk_settings_get_default ();
        if (gtk_settings != NULL)
            caffeine->theme_notify_handler_id =
                g_signal_connect (gtk_settings, "notify::gtk-application-prefer-dark-theme",
                                   G_CALLBACK (on_gtk_theme_notify), caffeine);
    }

    caffeine_reload_icons (caffeine);
}

/* Register the plugin with the Xfce panel */
XFCE_PANEL_PLUGIN_REGISTER (caffeine_construct);