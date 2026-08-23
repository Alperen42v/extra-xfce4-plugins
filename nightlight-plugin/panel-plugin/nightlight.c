/*
 * nightlight.c - XFCE4 Panel Plugin
 * Night light (blue light reduction) plugin - core logic.
 *
 * The preferences dialog lives in nightlight-prefs.c.
 *
 * Build: see Makefile
 */

#include "nightlight.h"

#include <libxfce4util/libxfce4util.h>

#include <X11/Xlib.h>
#include <X11/extensions/Xrandr.h>

#include <math.h>
#include <time.h>
#include <string.h>
#include <stdlib.h>

/* ---------- Color temperature -> RGB (Tanner Helland approximation) ---------- */

static void
temp_to_rgb (gint kelvin, gdouble *r, gdouble *g, gdouble *b)
{
    gdouble temp = kelvin / 100.0;
    gdouble red, green, blue;

    if (temp <= 66.0)
        red = 255.0;
    else
    {
        red = 329.698727446 * pow (temp - 60.0, -0.1332047592);
        if (red < 0) red = 0;
        if (red > 255) red = 255;
    }

    if (temp <= 66.0)
    {
        green = 99.4708025861 * log (temp) - 161.1195681661;
    }
    else
    {
        green = 288.1221695283 * pow (temp - 60.0, -0.0755148492);
    }
    if (green < 0) green = 0;
    if (green > 255) green = 255;

    if (temp >= 66.0)
        blue = 255.0;
    else if (temp <= 19.0)
        blue = 0.0;
    else
    {
        blue = 138.5177312231 * log (temp - 10.0) - 305.0447927307;
        if (blue < 0) blue = 0;
        if (blue > 255) blue = 255;
    }

    *r = red / 255.0;
    *g = green / 255.0;
    *b = blue / 255.0;
}

/* ---------- Applying gamma via XRandR ---------- */

static void
apply_gamma_temperature (gint kelvin)
{
    Display *dpy;
    Window root;
    XRRScreenResources *res;
    gdouble rr, gg, bb;
    int screen;
    int i;

    dpy = XOpenDisplay (NULL);
    if (dpy == NULL)
        return;

    screen = DefaultScreen (dpy);
    root = RootWindow (dpy, screen);

    temp_to_rgb (kelvin, &rr, &gg, &bb);

    res = XRRGetScreenResourcesCurrent (dpy, root);
    if (res == NULL)
        res = XRRGetScreenResources (dpy, root);

    if (res == NULL)
    {
        XCloseDisplay (dpy);
        return;
    }

    for (i = 0; i < res->ncrtc; i++)
    {
        RRCrtc crtc = res->crtcs[i];
        int gamma_size = XRRGetCrtcGammaSize (dpy, crtc);
        XRRCrtcGamma *gamma;
        int j;

        if (gamma_size <= 0)
            continue;

        gamma = XRRAllocGamma (gamma_size);
        if (gamma == NULL)
            continue;

        for (j = 0; j < gamma_size; j++)
        {
            gdouble v = (gdouble) j / (gdouble) (gamma_size - 1);
            gamma->red[j]   = (unsigned short) CLAMP (v * rr * 65535.0, 0, 65535);
            gamma->green[j] = (unsigned short) CLAMP (v * gg * 65535.0, 0, 65535);
            gamma->blue[j]  = (unsigned short) CLAMP (v * bb * 65535.0, 0, 65535);
        }

        XRRSetCrtcGamma (dpy, crtc, gamma);
        XRRFreeGamma (gamma);
    }

    XRRFreeScreenResources (res);
    XSync (dpy, False);
    XCloseDisplay (dpy);
}

/* strength (0-100) -> kelvin. 0 = no effect (6500K), 100 = strongest (1000K) */
gint
strength_to_kelvin (gint strength)
{
    strength = CLAMP (strength, 0, 100);
    return NL_TEMP_MAX - (gint) ((NL_TEMP_MAX - NL_TEMP_MIN) * (strength / 100.0));
}

/* ---------- Panel icon ---------- */

static void
update_panel_icon (NightlightPlugin *nl)
{
    if (nl->icon_image == NULL)
        return;

    gtk_image_set_from_icon_name (GTK_IMAGE (nl->icon_image),
                                   nl->is_active ? "weather-clear-night-symbolic"
                                                 : "weather-clear-symbolic",
                                   GTK_ICON_SIZE_SMALL_TOOLBAR);

    gtk_widget_set_tooltip_text (nl->button,
                                  nl->is_active ? "Night Light: On" : "Night Light: Off");
}

/* ---------- Animation ---------- */

static gboolean
animation_tick (gpointer data)
{
    NightlightPlugin *nl = data;
    gint64 now = g_get_monotonic_time ();
    gdouble elapsed_ms = (now - nl->anim_started_at) / 1000.0;
    gdouble duration = MAX (nl->transition_ms, 1);
    gdouble t = elapsed_ms / duration;
    gint temp;

    if (t >= 1.0)
    {
        nl->current_temp = nl->anim_to;
        apply_gamma_temperature (nl->current_temp);
        nl->anim_source = 0;
        return G_SOURCE_REMOVE;
    }

    /* smooth (ease-in-out) transition */
    gdouble eased = t < 0.5 ? 2 * t * t : 1 - pow (-2 * t + 2, 2) / 2;
    temp = nl->anim_from + (gint) ((nl->anim_to - nl->anim_from) * eased);
    nl->current_temp = temp;
    apply_gamma_temperature (temp);

    return G_SOURCE_CONTINUE;
}

void
start_transition (NightlightPlugin *nl, gint target_temp)
{
    if (nl->anim_source != 0)
    {
        g_source_remove (nl->anim_source);
        nl->anim_source = 0;
    }

    if (nl->transition_ms <= 0)
    {
        nl->current_temp = target_temp;
        apply_gamma_temperature (target_temp);
        return;
    }

    nl->anim_from = nl->current_temp;
    nl->anim_to = target_temp;
    nl->anim_started_at = g_get_monotonic_time ();
    nl->anim_source = g_timeout_add (NL_ANIM_STEP_MS, animation_tick, nl);
}

/* ---------- On / off logic ---------- */

static void
set_active (NightlightPlugin *nl, gboolean active)
{
    gint target_kelvin;

    nl->is_active = active;
    target_kelvin = active ? strength_to_kelvin (nl->strength) : NL_TEMP_NEUTRAL;

    start_transition (nl, target_kelvin);
    update_panel_icon (nl);
}

/* is the current time within [start, end)? (wraps past midnight) */
gboolean
time_in_range (gint cur_min, gint start_min, gint end_min)
{
    if (start_min == end_min)
        return FALSE; /* treat as no range */

    if (start_min < end_min)
        return cur_min >= start_min && cur_min < end_min;
    else
        /* wraps past midnight, e.g. 22:00 -> 07:00 */
        return cur_min >= start_min || cur_min < end_min;
}

gboolean
scheduler_tick (gpointer data)
{
    NightlightPlugin *nl = data;
    time_t t;
    struct tm lt;
    gint cur_min, start_min, end_min;
    gboolean should_be_active;

    if (!nl->master_enabled)
        return G_SOURCE_CONTINUE;

    if (nl->manual_override)
    {
        /* the user is controlling this manually, leave it alone */
        return G_SOURCE_CONTINUE;
    }

    if (!nl->schedule_enabled)
        return G_SOURCE_CONTINUE;

    t = time (NULL);
    localtime_r (&t, &lt);
    cur_min = lt.tm_hour * 60 + lt.tm_min;
    start_min = nl->start_hour * 60 + nl->start_minute;
    end_min = nl->end_hour * 60 + nl->end_minute;

    should_be_active = time_in_range (cur_min, start_min, end_min);

    if (should_be_active != nl->is_active)
        set_active (nl, should_be_active);

    return G_SOURCE_CONTINUE;
}

/* Manual click: toggles the state and overrides the scheduler. Changing
 * anything in the preferences dialog clears the override so the scheduler
 * takes back control. */
static void
on_button_clicked (GtkWidget *widget, gpointer data)
{
    NightlightPlugin *nl = data;
    (void) widget;

    nl->manual_override = TRUE;
    nl->manual_state = !nl->is_active;
    set_active (nl, nl->manual_state);
}

/* ---------- Save / load settings (XfceRc) ---------- */

void
save_config (NightlightPlugin *nl)
{
    gchar *file;
    XfceRc *rc;

    file = xfce_panel_plugin_save_location (nl->plugin, TRUE);
    if (file == NULL)
        return;

    rc = xfce_rc_simple_open (file, FALSE);
    g_free (file);
    if (rc == NULL)
        return;

    xfce_rc_write_bool_entry (rc, "master_enabled", nl->master_enabled);
    xfce_rc_write_bool_entry (rc, "schedule_enabled", nl->schedule_enabled);
    xfce_rc_write_int_entry (rc, "strength", nl->strength);
    xfce_rc_write_int_entry (rc, "start_hour", nl->start_hour);
    xfce_rc_write_int_entry (rc, "start_minute", nl->start_minute);
    xfce_rc_write_int_entry (rc, "end_hour", nl->end_hour);
    xfce_rc_write_int_entry (rc, "end_minute", nl->end_minute);
    xfce_rc_write_bool_entry (rc, "use_24h", nl->use_24h);
    xfce_rc_write_int_entry (rc, "transition_ms", nl->transition_ms);

    xfce_rc_close (rc);
}

static void
load_config (NightlightPlugin *nl)
{
    gchar *file;
    XfceRc *rc;

    /* defaults */
    nl->master_enabled = TRUE;
    nl->schedule_enabled = FALSE;
    nl->strength = NL_DEFAULT_STRENGTH;
    nl->start_hour = 21; nl->start_minute = 0;
    nl->end_hour = 7; nl->end_minute = 0;
    nl->use_24h = TRUE;
    nl->transition_ms = NL_DEFAULT_TRANSITION_MS;

    file = xfce_panel_plugin_lookup_rc_file (nl->plugin);
    if (file == NULL)
        return;

    rc = xfce_rc_simple_open (file, TRUE);
    g_free (file);
    if (rc == NULL)
        return;

    nl->master_enabled = xfce_rc_read_bool_entry (rc, "master_enabled", nl->master_enabled);
    nl->schedule_enabled = xfce_rc_read_bool_entry (rc, "schedule_enabled", nl->schedule_enabled);
    nl->strength = xfce_rc_read_int_entry (rc, "strength", nl->strength);
    nl->start_hour = xfce_rc_read_int_entry (rc, "start_hour", nl->start_hour);
    nl->start_minute = xfce_rc_read_int_entry (rc, "start_minute", nl->start_minute);
    nl->end_hour = xfce_rc_read_int_entry (rc, "end_hour", nl->end_hour);
    nl->end_minute = xfce_rc_read_int_entry (rc, "end_minute", nl->end_minute);
    nl->use_24h = xfce_rc_read_bool_entry (rc, "use_24h", nl->use_24h);
    nl->transition_ms = xfce_rc_read_int_entry (rc, "transition_ms", nl->transition_ms);

    xfce_rc_close (rc);
}

/* ---------- Plugin lifecycle ---------- */

static void
nightlight_free (XfcePanelPlugin *plugin, NightlightPlugin *nl)
{
    (void) plugin;

    if (nl->scheduler_source != 0)
        g_source_remove (nl->scheduler_source);
    if (nl->anim_source != 0)
        g_source_remove (nl->anim_source);

    /* restore the screen to normal on exit */
    apply_gamma_temperature (NL_TEMP_NEUTRAL);

    g_free (nl);
}

static gboolean
nightlight_size_changed (XfcePanelPlugin *plugin, gint size, NightlightPlugin *nl)
{
    (void) nl;
    gtk_widget_set_size_request (GTK_WIDGET (plugin), size, size);
    return TRUE;
}

/* The "save" signal is emitted by the panel as (plugin, user_data); save_config
 * only takes (NightlightPlugin *), so this thin wrapper adapts the signature.
 * Without it, the plugin pointer gets misread as NightlightPlugin* and crashes. */
static void
nightlight_save (XfcePanelPlugin *plugin, NightlightPlugin *nl)
{
    (void) plugin;
    save_config (nl);
}

static void
nightlight_construct (XfcePanelPlugin *plugin)
{
    NightlightPlugin *nl = g_new0 (NightlightPlugin, 1);
    nl->plugin = plugin;
    nl->current_temp = NL_TEMP_NEUTRAL;
    nl->is_active = FALSE;

    load_config (nl);

    nl->button = gtk_button_new ();
    gtk_button_set_relief (GTK_BUTTON (nl->button), GTK_RELIEF_NONE);
    gtk_widget_set_can_focus (nl->button, FALSE);
    nl->icon_image = gtk_image_new ();
    gtk_container_add (GTK_CONTAINER (nl->button), nl->icon_image);
    gtk_container_add (GTK_CONTAINER (plugin), nl->button);
    gtk_widget_show_all (nl->button);

    update_panel_icon (nl);

    g_signal_connect (nl->button, "clicked", G_CALLBACK (on_button_clicked), nl);

    xfce_panel_plugin_add_action_widget (plugin, nl->button);

    g_signal_connect (plugin, "free-data", G_CALLBACK (nightlight_free), nl);
    g_signal_connect (plugin, "size-changed", G_CALLBACK (nightlight_size_changed), nl);
    g_signal_connect (plugin, "configure-plugin", G_CALLBACK (nightlight_show_settings), nl);
    g_signal_connect (plugin, "save", G_CALLBACK (nightlight_save), nl);

    xfce_panel_plugin_menu_show_configure (plugin);

    /* on startup, decide state based on current time (instant, no animation) */
    if (nl->master_enabled && nl->schedule_enabled)
    {
        time_t t = time (NULL);
        struct tm lt;
        localtime_r (&t, &lt);
        gint cur_min = lt.tm_hour * 60 + lt.tm_min;
        gint start_min = nl->start_hour * 60 + nl->start_minute;
        gint end_min = nl->end_hour * 60 + nl->end_minute;

        if (time_in_range (cur_min, start_min, end_min))
        {
            nl->is_active = TRUE;
            nl->current_temp = strength_to_kelvin (nl->strength);
            apply_gamma_temperature (nl->current_temp);
            update_panel_icon (nl);
        }
    }

    nl->scheduler_source = g_timeout_add_seconds (NL_SCHEDULER_INTERVAL_S, scheduler_tick, nl);
}

XFCE_PANEL_PLUGIN_REGISTER (nightlight_construct);