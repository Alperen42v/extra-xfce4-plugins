/*
 * Caffeine Plugin for the Xfce Panel
 *
 * Left-click toggles "keep awake" mode using the standard
 * org.freedesktop.ScreenSaver DBus Inhibit/UnInhibit interface.
 */

#include <math.h>
#include <gio/gio.h>

#include "caffeine.h"
#include "caffeine-prefs.h"
#include "caffeine-icons.h"

#define ANIM_INTERVAL_MS 60          /* ~16 fps */
#define LOCK_TO_BLANK_DELAY_SEC 7    /* gap between xflock4 and forcing DPMS off */
#define DBUS_CALL_TIMEOUT_MS 2000    /* Inhibit/UnInhibit run on the panel's main loop: keep a hung service cheap */
#define ICON_RELOAD_DEBOUNCE_MS 200  /* icon theme "changed" tends to arrive in bursts */

/* ---------------------------------------------------------------------- */
/* Small helpers                                                          */
/* ---------------------------------------------------------------------- */

/* Removes a GSource if it is running and marks the id as unused. */
static void
clear_source (guint *source_id)
{
    if (*source_id != 0)
    {
        g_source_remove (*source_id);
        *source_id = 0;
    }
}

/* Fire-and-forget command; GLib reaps the child itself. */
static void
run_command (const gchar *command_line, const gchar *purpose)
{
    GError *error = NULL;

    if (!g_spawn_command_line_async (command_line, &error))
    {
        g_warning ("Caffeine: failed to %s via '%s': %s",
                   purpose, command_line, error->message);
        g_clear_error (&error);
    }
}

/* ---------------------------------------------------------------------- */
/* DBus screensaver inhibit helpers                                       */
/* ---------------------------------------------------------------------- */

/*
 * Different desktops expose Inhibit/UnInhibit under different bus
 * names/paths/interfaces (xfce4-power-manager in particular does not
 * mirror its bus name into its object path). We inhibit both the
 * screensaver and the power manager's DPMS independently, and remember
 * which target worked for each so UnInhibit hits the same one.
 *
 * Calls use NO_AUTO_START: if a backend isn't already running we want a
 * quick "no", not Caffeine silently launching some other desktop's
 * screensaver daemon just to try it.
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
                                               G_DBUS_CALL_FLAGS_NO_AUTO_START,
                                               DBUS_CALL_TIMEOUT_MS, NULL, &error);

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
    GVariant *result;
    GError   *error = NULL;

    if (cookie == 0 || backend == NULL || object_path == NULL || interface_name == NULL)
        return;

    result = g_dbus_connection_call_sync (conn,
                                           backend,
                                           object_path,
                                           interface_name,
                                           "UnInhibit",
                                           g_variant_new ("(u)", cookie),
                                           NULL,
                                           G_DBUS_CALL_FLAGS_NO_AUTO_START,
                                           DBUS_CALL_TIMEOUT_MS, NULL, &error);

    if (result != NULL)
        g_variant_unref (result);

    if (error != NULL)
    {
        g_warning ("Caffeine: UnInhibit via %s (%s, %s) failed: %s",
                   backend, object_path, interface_name, error->message);
        g_clear_error (&error);
    }
}

/* Forgets both inhibits (cookies + remembered targets). */
static void
caffeine_inhibit_state_clear (CaffeinePlugin *caffeine)
{
    caffeine->ss_cookie = 0;
    caffeine->pm_cookie = 0;

    g_clear_pointer (&caffeine->ss_backend, g_free);
    g_clear_pointer (&caffeine->ss_path, g_free);
    g_clear_pointer (&caffeine->ss_iface, g_free);
    g_clear_pointer (&caffeine->pm_backend, g_free);
    g_clear_pointer (&caffeine->pm_path, g_free);
    g_clear_pointer (&caffeine->pm_iface, g_free);
}

static void
caffeine_dbus_uninhibit (CaffeinePlugin *caffeine)
{
    if (caffeine->ss_cookie != 0 || caffeine->pm_cookie != 0)
    {
        GDBusConnection *conn;
        GError          *error = NULL;

        conn = g_bus_get_sync (G_BUS_TYPE_SESSION, NULL, &error);
        if (conn != NULL)
        {
            try_uninhibit (conn, caffeine->ss_cookie, caffeine->ss_backend,
                           caffeine->ss_path, caffeine->ss_iface);
            try_uninhibit (conn, caffeine->pm_cookie, caffeine->pm_backend,
                           caffeine->pm_path, caffeine->pm_iface);
            g_object_unref (conn);
        }
        else
        {
            g_warning ("Caffeine: could not connect to session bus: %s", error->message);
            g_clear_error (&error);
        }
    }

    /* Always forget the old state, even if the bus was unreachable: the
     * inhibits die with our bus connection anyway, and keeping stale
     * cookies around would only leak/confuse the next Inhibit. */
    caffeine_inhibit_state_clear (caffeine);
}

/* Returns TRUE if at least one of screensaver/power-management inhibit
 * succeeded; logs a warning for whichever one failed. */
static gboolean
caffeine_dbus_inhibit (CaffeinePlugin *caffeine)
{
    GDBusConnection *conn;
    GError          *error = NULL;
    gboolean         ss_ok, pm_ok;

    /* drop anything left over from an earlier attempt first */
    caffeine_dbus_uninhibit (caffeine);

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
    GdkPixbuf *frame_to_draw = NULL;
    gint w = gtk_widget_get_allocated_width (widget);
    gint h = gtk_widget_get_allocated_height (widget);

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
        /* centered in the real allocation: themes with large button
         * padding/borders can make it differ from the icon's own size,
         * and drawing at (0,0) would push the icon off to one side */
        gint x = (w - gdk_pixbuf_get_width (frame_to_draw)) / 2;
        gint y = (h - gdk_pixbuf_get_height (frame_to_draw)) / 2;

        gdk_cairo_set_source_pixbuf (cr, frame_to_draw, x, y);
        cairo_paint (cr);
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

/* ---------------------------------------------------------------------- */
/* Animation                                                              */
/* ---------------------------------------------------------------------- */

/* The timer only exists while something is actually moving:
 *  - Cairo fallback (no ON frames loaded): steam wobble;
 *  - custom/system icons with 2+ ON frames: frame cycling.
 * A single static ON frame (the system-theme case, or one PNG) needs no
 * redraws at all, so no wake-ups every 60 ms for nothing. */
static gboolean
caffeine_needs_animation (CaffeinePlugin *caffeine)
{
    if (!caffeine->active)
        return FALSE;

    if (caffeine->icons == NULL || caffeine->icons->on_frame_count == 0)
        return TRUE;

    return caffeine->icons->on_frame_count > 1;
}

static gboolean
on_animation_tick (gpointer user_data)
{
    CaffeinePlugin *caffeine = (CaffeinePlugin *) user_data;

    if (caffeine->icons != NULL && caffeine->icons->on_frame_count > 1)
    {
        caffeine->icon_frame_index = (caffeine->icon_frame_index + 1) % caffeine->icons->on_frame_count;
    }
    else
    {
        caffeine->animation_phase += 0.03;
        if (caffeine->animation_phase > 1.0)
            caffeine->animation_phase -= 1.0;
    }

    gtk_widget_queue_draw (caffeine->icon_area);

    return G_SOURCE_CONTINUE;
}

/* Starts/stops the animation timer to match the current state. Safe to
 * call any time (state change, icon reload); does nothing if the timer
 * already matches. */
static void
caffeine_update_animation (CaffeinePlugin *caffeine)
{
    gboolean needed = caffeine_needs_animation (caffeine);

    if (needed && caffeine->animation_timer_id == 0)
    {
        caffeine->animation_timer_id =
            g_timeout_add (ANIM_INTERVAL_MS, on_animation_tick, caffeine);
    }
    else if (!needed)
    {
        clear_source (&caffeine->animation_timer_id);
    }
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

    caffeine->blank_timer_id = 0; /* this source is about to be removed */
    run_command ("xset dpms force off", "blank the monitor");

    return G_SOURCE_REMOVE;
}

static gboolean
on_lock_cycle_tick (gpointer user_data)
{
    CaffeinePlugin *caffeine = (CaffeinePlugin *) user_data;

    run_command ("xflock4", "lock the screen");

    /* tracked (and replaced, never stacked) so switching caffeine off, or
     * removing the plugin, within the delay cancels the pending blank */
    clear_source (&caffeine->blank_timer_id);
    caffeine->blank_timer_id =
        g_timeout_add_seconds (LOCK_TO_BLANK_DELAY_SEC, on_blank_after_lock, caffeine);

    return G_SOURCE_CONTINUE;
}

static void
caffeine_lock_cycle_stop (CaffeinePlugin *caffeine)
{
    clear_source (&caffeine->lock_cycle_timer_id);
    clear_source (&caffeine->blank_timer_id);
}

/* (Re)starts the lock cycle timer from current settings. No-op if mode
 * is NEVER or caffeine isn't active; always clears any existing timer. */
static void
caffeine_lock_cycle_restart (CaffeinePlugin *caffeine)
{
    guint interval_min;

    caffeine_lock_cycle_stop (caffeine);

    if (!caffeine->active)
        return;

    interval_min = caffeine_settings_get_interval_minutes (&caffeine->settings);
    if (interval_min == 0)
        return; /* NEVER, or invalid custom value */

    caffeine->lock_cycle_timer_id =
        g_timeout_add_seconds (interval_min * 60, on_lock_cycle_tick, caffeine);
}

/* ---------------------------------------------------------------------- */
/* Self-triggered screen-off-only timer (independent of the lock cycle)   */
/* ---------------------------------------------------------------------- */

/* Unlike the lock cycle, this never calls xflock4 - only DPMS force off.
 * Waking the monitor is left to normal DPMS/X input handling. */

static gboolean
on_screen_off_tick (gpointer user_data)
{
    (void) user_data;

    run_command ("xset dpms force off", "turn off the screen");

    return G_SOURCE_CONTINUE;
}

static void
caffeine_screen_off_stop (CaffeinePlugin *caffeine)
{
    clear_source (&caffeine->screen_off_timer_id);
}

/* (Re)starts the screen-off timer from current settings. No-op if
 * disabled or caffeine isn't active; always clears any existing timer. */
static void
caffeine_screen_off_restart (CaffeinePlugin *caffeine)
{
    guint interval_min;

    caffeine_screen_off_stop (caffeine);

    if (!caffeine->active)
        return;

    interval_min = caffeine_settings_get_screen_off_interval_minutes (&caffeine->settings);
    if (interval_min == 0)
        return; /* disabled, or invalid custom value */

    caffeine->screen_off_timer_id =
        g_timeout_add_seconds (interval_min * 60, on_screen_off_tick, caffeine);
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
                _("Caffeine: failed to inhibit screensaver/power manager (DBus error)"));
            return;
        }

        caffeine->active = TRUE;
        caffeine->animation_phase = 0.0;
        caffeine->icon_frame_index = 0;
        caffeine_update_animation (caffeine);

        caffeine_lock_cycle_restart (caffeine);
        caffeine_screen_off_restart (caffeine);

        gtk_widget_set_tooltip_text (caffeine->button,
            _("Caffeine: ON — screen will stay awake (click to disable)"));
    }
    else
    {
        caffeine_dbus_uninhibit (caffeine);
        caffeine->active = FALSE;
        caffeine_update_animation (caffeine); /* needed == FALSE now: stops the timer */

        caffeine_lock_cycle_stop (caffeine);
        caffeine_screen_off_stop (caffeine);

        gtk_widget_set_tooltip_text (caffeine->button,
            _("Caffeine: OFF — click to keep the screen awake"));
    }

    gtk_widget_queue_draw (caffeine->icon_area);
}

static void
on_button_clicked (GtkWidget *widget, gpointer user_data)
{
    CaffeinePlugin *caffeine = (CaffeinePlugin *) user_data;

    (void) widget;
    caffeine_set_active (caffeine, !caffeine->active);
}

/* ---------------------------------------------------------------------- */
/* Translations                                                           */
/* ---------------------------------------------------------------------- */

/* Points gettext at the right translation directory and follows the
 * system language automatically (LANGUAGE / LC_ALL / LC_MESSAGES / LANG,
 * exactly what the rest of the desktop uses). Runs every time the plugin
 * loads, so after the system language changes a panel restart (or a new
 * login) is all it takes.
 *
 * Two places are checked, so it works no matter how the plugin was
 * installed: the user's own directory (~/.local/share/locale, used by
 * a non-root `make install`) wins if it has a translation for the
 * current language; otherwise the system directory (LOCALEDIR, used by
 * `sudo make install` or a package) is used. */
static void
caffeine_init_translations (void)
{
    const gchar * const *languages = g_get_language_names ();
    gchar               *user_localedir;
    const gchar         *localedir = LOCALEDIR;
    gint                 i;

    user_localedir = g_build_filename (g_get_user_data_dir (), "locale", NULL);

    for (i = 0; languages[i] != NULL; i++)
    {
        gchar    *mo_path = g_build_filename (user_localedir, languages[i],
                                               "LC_MESSAGES", GETTEXT_PACKAGE ".mo", NULL);
        gboolean  found = g_file_test (mo_path, G_FILE_TEST_IS_REGULAR);

        g_free (mo_path);

        if (found)
        {
            localedir = user_localedir;
            break;
        }
    }

    bindtextdomain (GETTEXT_PACKAGE, localedir);
    bind_textdomain_codeset (GETTEXT_PACKAGE, "UTF-8");

    g_free (user_localedir); /* bindtextdomain() keeps its own copy */
}

/* ---------------------------------------------------------------------- */
/* Icons and sizing                                                       */
/* ---------------------------------------------------------------------- */

/* Reloads the icon set at the icon area's current pixel size.
 *
 * Unless `force` is set, this is skipped when the result would be the
 * same as what's already loaded (same size, source, light/dark variant
 * and - for system symbolic icons - same foreground colour). That makes
 * it cheap to call from every place that *might* change the icons:
 * startup, theme/style updates, panel resizes. `force` is for events
 * where the files or the theme itself changed. */
static void
caffeine_reload_icons (CaffeinePlugin *caffeine, gboolean force)
{
    GtkStyleContext   *context;
    CaffeineIconTheme  variant;
    GdkRGBA            fg;

    /* uses the last size reported via size-changed, not
     * gtk_widget_get_allocation() which can still read 0 very early */
    if (caffeine->icon_pixel_size <= 0)
        return;

    context = gtk_widget_get_style_context (caffeine->button);
    variant = caffeine_icons_resolve_theme (caffeine->settings.icon_theme);

    /* NORMAL state on purpose: hover/pressed colours must not look like
     * a theme change and trigger reloads while the pointer moves around */
    gtk_style_context_get_color (context, GTK_STATE_FLAG_NORMAL, &fg);

    if (!force && caffeine->icons != NULL &&
        caffeine->icons_loaded_size == caffeine->icon_pixel_size &&
        caffeine->icons_loaded_source == caffeine->settings.icon_source &&
        caffeine->icons_loaded_variant == variant &&
        (caffeine->settings.icon_source != CAFFEINE_ICON_SOURCE_SYSTEM ||
         gdk_rgba_equal (&fg, &caffeine->icons_loaded_fg)))
        return;

    caffeine_icon_set_free (caffeine->icons);
    caffeine->icons = NULL;

    /* SYSTEM source: use the icon theme's caffeine icons if it has them;
     * otherwise (or for the PLUGIN source) use Caffeine's own icons,
     * which themselves fall back to the Cairo cup */
    if (caffeine->settings.icon_source == CAFFEINE_ICON_SOURCE_SYSTEM)
        caffeine->icons = caffeine_icons_load_from_system_theme (caffeine->icon_pixel_size, context);

    if (caffeine->icons == NULL)
        caffeine->icons = caffeine_icons_load (caffeine->icon_pixel_size, caffeine->settings.icon_theme);

    caffeine->icons_loaded_size = caffeine->icon_pixel_size;
    caffeine->icons_loaded_source = caffeine->settings.icon_source;
    caffeine->icons_loaded_variant = variant;
    caffeine->icons_loaded_fg = fg;

    caffeine->icon_frame_index = 0;

    /* the new set may or may not animate (1 frame vs many vs Cairo) */
    caffeine_update_animation (caffeine);
    gtk_widget_queue_draw (caffeine->icon_area);
}

static gboolean
on_icon_reload_timeout (gpointer user_data)
{
    CaffeinePlugin *caffeine = (CaffeinePlugin *) user_data;

    caffeine->icon_reload_timer_id = 0;
    caffeine_reload_icons (caffeine, TRUE);

    return G_SOURCE_REMOVE;
}

/* Coalesces a burst of icon-theme "changed" signals into one reload. */
static void
caffeine_schedule_icon_reload (CaffeinePlugin *caffeine)
{
    if (caffeine->icon_reload_timer_id == 0)
        caffeine->icon_reload_timer_id =
            g_timeout_add (ICON_RELOAD_DEBOUNCE_MS, on_icon_reload_timeout, caffeine);
}

/* The icon has to fit INSIDE the button: panel size minus the theme's
 * padding and border on the button. Giving the icon the full panel size
 * (as before) makes the button grow past the panel row on themes with
 * thick button padding, which shifted the icon out of place. */
static gint
caffeine_get_icon_size (CaffeinePlugin *caffeine, gint panel_size)
{
    GtkStyleContext *context = gtk_widget_get_style_context (caffeine->button);
    GtkBorder        padding;
    GtkBorder        border;
    gint             extra;

    /* NORMAL state, not the live one: some themes change padding while
     * the button is pressed/hovered, which would make the icon size (and
     * with it a full icon reload) flip on every click */
    gtk_style_context_get_padding (context, GTK_STATE_FLAG_NORMAL, &padding);
    gtk_style_context_get_border (context, GTK_STATE_FLAG_NORMAL, &border);

    extra = MAX (padding.left + padding.right + border.left + border.right,
                 padding.top + padding.bottom + border.top + border.bottom);

    return MAX (8, panel_size - extra);
}

/* Applies a panel size: pins the whole plugin to a panel_size square
 * (the button fills it) and reloads the icons if the usable icon size
 * changed. Also called when the button's theme style changes. */
static void
caffeine_apply_panel_size (CaffeinePlugin *caffeine, gint panel_size)
{
    gint icon_size = caffeine_get_icon_size (caffeine, panel_size);

    caffeine->panel_size = panel_size;
    gtk_widget_set_size_request (GTK_WIDGET (caffeine->plugin), panel_size, panel_size);

    if (icon_size == caffeine->icon_pixel_size)
        return;

    caffeine->icon_pixel_size = icon_size;
    gtk_widget_set_size_request (caffeine->icon_area, icon_size, icon_size);
    caffeine_reload_icons (caffeine, FALSE);
}

static gboolean
caffeine_size_changed (XfcePanelPlugin *plugin, gint size, CaffeinePlugin *caffeine)
{
    (void) plugin;
    caffeine_apply_panel_size (caffeine, size);
    return TRUE;
}

/* The theme (and so the button's padding and colours) can change at
 * runtime, or only settle after the button joins the panel - recompute
 * the icon size, and reload if the symbolic icon colour went stale. */
static void
on_button_style_updated (GtkWidget *button, gpointer user_data)
{
    CaffeinePlugin *caffeine = (CaffeinePlugin *) user_data;

    (void) button;
    if (caffeine->panel_size > 0)
    {
        caffeine_apply_panel_size (caffeine, caffeine->panel_size);
        caffeine_reload_icons (caffeine, FALSE);
    }
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
        caffeine_reload_icons (caffeine, FALSE);
}

/* Fires when the GTK icon theme changes or is modified on disk. Only
 * matters for the SYSTEM icon source. */
static void
on_icon_theme_changed (GtkIconTheme *icon_theme, gpointer user_data)
{
    CaffeinePlugin *caffeine = (CaffeinePlugin *) user_data;

    (void) icon_theme;

    if (caffeine->settings.icon_source == CAFFEINE_ICON_SOURCE_SYSTEM)
        caffeine_schedule_icon_reload (caffeine);
}

/* ---------------------------------------------------------------------- */
/* Plugin lifecycle                                                       */
/* ---------------------------------------------------------------------- */

static void
caffeine_free (XfcePanelPlugin *plugin, CaffeinePlugin *caffeine)
{
    /* Stop everything that could call back into `caffeine` first... */
    clear_source (&caffeine->animation_timer_id);
    clear_source (&caffeine->icon_reload_timer_id);
    caffeine_lock_cycle_stop (caffeine);   /* also cancels a pending blank */
    caffeine_screen_off_stop (caffeine);

    /* ...then make sure no signal can reach the memory freed below, even
     * if a widget emits something while it is being torn down. */
    g_signal_handlers_disconnect_by_data (plugin, caffeine);
    g_signal_handlers_disconnect_by_data (caffeine->button, caffeine);
    g_signal_handlers_disconnect_by_data (caffeine->icon_area, caffeine);

    if (caffeine->theme_notify_handler_id != 0)
    {
        GtkSettings *gtk_settings = gtk_settings_get_default ();
        if (gtk_settings != NULL)
            g_signal_handler_disconnect (gtk_settings, caffeine->theme_notify_handler_id);
        caffeine->theme_notify_handler_id = 0;
    }

    if (caffeine->icon_theme_changed_handler_id != 0)
    {
        GtkIconTheme *icon_theme = gtk_icon_theme_get_default ();
        if (icon_theme != NULL)
            g_signal_handler_disconnect (icon_theme, caffeine->icon_theme_changed_handler_id);
        caffeine->icon_theme_changed_handler_id = 0;
    }

    /* no-op if nothing is inhibited; releases the inhibits otherwise */
    caffeine_dbus_uninhibit (caffeine);

    g_free (caffeine->xfconf_channel_name);
    caffeine_icon_set_free (caffeine->icons);
    g_free (caffeine);
}

/* The user removed this plugin instance from the panel for good (not
 * just a panel restart): don't leave its xfconf channel behind. */
static void
caffeine_removed (XfcePanelPlugin *plugin, CaffeinePlugin *caffeine)
{
    (void) plugin;
    caffeine_settings_remove (caffeine->xfconf_channel_name);
}

static void
caffeine_configure_plugin (XfcePanelPlugin *plugin, CaffeinePlugin *caffeine)
{
    if (caffeine_show_preferences (plugin, &caffeine->settings, caffeine->xfconf_channel_name))
    {
        /* restart timers immediately so new intervals take effect */
        caffeine_lock_cycle_restart (caffeine);
        caffeine_screen_off_restart (caffeine);
        caffeine_reload_icons (caffeine, TRUE);
    }
}

void
caffeine_construct (XfcePanelPlugin *plugin)
{
    CaffeinePlugin *caffeine = g_new0 (CaffeinePlugin, 1); /* everything starts at 0/NULL/FALSE */

    /* must run before the first _() call below */
    caffeine_init_translations ();

    caffeine->plugin = plugin;

    /* unique per instance so multiple panel copies don't clobber settings */
    caffeine->xfconf_channel_name =
        g_strdup_printf ("xfce4-caffeine-plugin-%d", xfce_panel_plugin_get_unique_id (plugin));

    caffeine_settings_load (&caffeine->settings, caffeine->xfconf_channel_name);

    /* real panel size right away, so icons don't stay blank waiting for
     * the first size-changed signal (some panels delay/skip it) */
    caffeine->panel_size = xfce_panel_plugin_get_size (plugin);
    if (caffeine->panel_size <= 0)
        caffeine->panel_size = 24;
    caffeine->icon_pixel_size = 0; /* computed below, once the button exists */

    caffeine->button = xfce_panel_create_button ();
    gtk_button_set_relief (GTK_BUTTON (caffeine->button), GTK_RELIEF_NONE);
    gtk_widget_set_focus_on_click (caffeine->button, FALSE);

    caffeine->icon_area = gtk_drawing_area_new ();
    gtk_container_add (GTK_CONTAINER (caffeine->button), caffeine->icon_area);

    /* sizes the plugin, button content and icons from the panel size and
     * the theme's button padding; follows later theme changes too */
    caffeine_apply_panel_size (caffeine, caffeine->panel_size);
    g_signal_connect (caffeine->button, "style-updated",
                       G_CALLBACK (on_button_style_updated), caffeine);

    g_signal_connect (caffeine->icon_area, "draw",
                       G_CALLBACK (on_icon_draw), caffeine);
    g_signal_connect (caffeine->button, "clicked",
                       G_CALLBACK (on_button_clicked), caffeine);

    gtk_widget_set_tooltip_text (caffeine->button,
        _("Caffeine: OFF — click to keep the screen awake"));

    gtk_widget_show_all (caffeine->button);

    gtk_container_add (GTK_CONTAINER (plugin), caffeine->button);
    xfce_panel_plugin_add_action_widget (plugin, caffeine->button);

    xfce_panel_plugin_menu_show_configure (plugin);
    g_signal_connect (plugin, "configure-plugin", G_CALLBACK (caffeine_configure_plugin), caffeine);

    g_signal_connect (plugin, "free-data", G_CALLBACK (caffeine_free), caffeine);
    g_signal_connect (plugin, "removed", G_CALLBACK (caffeine_removed), caffeine);
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

    /* keeps the SYSTEM icon source in sync with live icon theme changes */
    {
        GtkIconTheme *icon_theme = gtk_icon_theme_get_default ();
        if (icon_theme != NULL)
            caffeine->icon_theme_changed_handler_id =
                g_signal_connect (icon_theme, "changed",
                                   G_CALLBACK (on_icon_theme_changed), caffeine);
    }

    /* normally a no-op: apply_panel_size() above already loaded the icons
     * for this exact size/theme (the signature check skips the duplicate) */
    caffeine_reload_icons (caffeine, FALSE);
}

/* Register the plugin with the Xfce panel */
XFCE_PANEL_PLUGIN_REGISTER (caffeine_construct);