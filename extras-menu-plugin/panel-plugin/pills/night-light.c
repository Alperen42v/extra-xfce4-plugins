/*
 * The Night Light pill: warms the screen's colors and back again, with a
 * smooth transition. See night-light.h.
 *
 * How it works: the X server applies a "gamma ramp" (three lookup tables,
 * red/green/blue) to every output. Scaling the green and blue tables down
 * a little makes the picture warmer. A color temperature is turned into
 * three scale factors (see multipliers()), and a transition is just a
 * timer that moves the temperature from one value to the other a few
 * dozen times a second.
 */

#include "pills/night-light.h"
#include "preferences/night-light-config.h"

#include <math.h>

#include <gdk/gdkx.h>
#include <X11/Xlib.h>
#include <X11/extensions/Xrandr.h>

#define NEUTRAL_K ((gdouble) EXTRAS_MENU_NIGHT_LIGHT_NEUTRAL_K)

/* One step of a transition every this many milliseconds (~60 per second).
 * The progress is worked out from the clock, not from counting steps, so
 * a transition lasts as long as asked even if a few steps come late. */
#define FRAME_MS 16

/* The gamma ramp one output had before Night Light touched it. */
typedef struct
{
    RRCrtc crtc;
    XRRCrtcGamma *gamma;
} SavedGamma;

struct _ExtrasMenuNightLight
{
    GtkWidget *pill; /* holds a reference */
    gulong toggled_id;
    guint config_listener;

    /* The X connection GDK already has; not ours to close. */
    GdkDisplay *gdk_display;
    Display *display;
    Window root;
    gboolean available; /* X11 with RandR 1.2 or newer */

    /* Where the screen is right now, in Kelvin (NEUTRAL_K = unchanged),
     * and the transition that is moving it, if timer_id != 0. */
    gdouble current_k;
    gdouble from_k;
    gdouble to_k;
    gint64 start_us;
    guint duration_ms;
    guint timer_id;

    /* SavedGamma, one per output. Not empty exactly while the screen is
     * (or is being moved) away from its normal colors. */
    GArray *saved;
};

/* --- color temperature -> RGB ---------------------------------------------------- */

/* The usual curve-fit of a black body's color (after Tanner Helland):
 * the red, green and blue strength, 0..1, of light at `kelvin`. Good
 * between roughly 1000 and 40000 K, which covers what we use. */
static void
kelvin_to_rgb(gdouble kelvin, gdouble rgb[3])
{
    gdouble t = kelvin / 100.0;
    gdouble r, g, b;

    if (t <= 66.0)
        r = 255.0;
    else
        r = 329.698727446 * pow(t - 60.0, -0.1332047592);

    if (t <= 66.0)
        g = 99.4708025861 * log(t) - 161.1195681661;
    else
        g = 288.1221695283 * pow(t - 60.0, -0.0755148492);

    if (t >= 66.0)
        b = 255.0;
    else if (t <= 19.0)
        b = 0.0;
    else
        b = 138.5177312231 * log(t - 10.0) - 305.0447927307;

    rgb[0] = CLAMP(r, 0.0, 255.0) / 255.0;
    rgb[1] = CLAMP(g, 0.0, 255.0) / 255.0;
    rgb[2] = CLAMP(b, 0.0, 255.0) / 255.0;
}

/* How much to scale each channel at `kelvin` so that NEUTRAL_K leaves
 * the picture exactly as it is. (The curve's own value at 6500 K is not
 * quite white; dividing by it is what makes "no change" really mean no
 * change.) */
static void
multipliers(gdouble kelvin, gdouble m[3])
{
    gdouble color[3];
    gdouble neutral[3];

    kelvin_to_rgb(kelvin, color);
    kelvin_to_rgb(NEUTRAL_K, neutral);

    for (gint i = 0; i < 3; i++)
        m[i] = MIN(1.0, color[i] / neutral[i]);
}

/* --- gamma ramps ------------------------------------------------------------------- */

static void
saved_gamma_clear(gpointer data)
{
    SavedGamma *saved = data;

    XRRFreeGamma(saved->gamma);
}

/* The ramp `crtc` had before we changed it. Remembered the first time
 * we get here for it, which is before its first change. */
static const XRRCrtcGamma *
saved_gamma_for(ExtrasMenuNightLight *self, RRCrtc crtc)
{
    for (guint i = 0; i < self->saved->len; i++)
    {
        SavedGamma *saved = &g_array_index(self->saved, SavedGamma, i);

        if (saved->crtc == crtc)
            return saved->gamma;
    }

    XRRCrtcGamma *gamma = XRRGetCrtcGamma(self->display, crtc);
    if (gamma == NULL)
        return NULL;

    SavedGamma saved = { crtc, gamma };
    g_array_append_val(self->saved, saved);
    return gamma;
}

/* Sets every output's ramp to its original one scaled for `kelvin`.
 * Every call looks up the outputs again, so a monitor plugged in during
 * a transition is picked up. X errors (an output that vanished between
 * two requests, say) are caught and ignored: GDK would otherwise abort
 * the whole panel on one. */
static void
apply_temperature(ExtrasMenuNightLight *self, gdouble kelvin)
{
    gdouble m[3];
    multipliers(kelvin, m);

    gdk_x11_display_error_trap_push(self->gdk_display);

    XRRScreenResources *resources = XRRGetScreenResourcesCurrent(self->display, self->root);

    for (gint i = 0; resources != NULL && i < resources->ncrtc; i++)
    {
        RRCrtc crtc = resources->crtcs[i];
        gint size = XRRGetCrtcGammaSize(self->display, crtc);

        if (size <= 0) /* output that is switched off */
            continue;

        const XRRCrtcGamma *original = saved_gamma_for(self, crtc);
        if (original != NULL && original->size != size)
            original = NULL;

        XRRCrtcGamma *ramp = XRRAllocGamma(size);

        for (gint j = 0; j < size; j++)
        {
            gdouble linear = size > 1 ? (gdouble) j / (size - 1) * 65535.0 : 65535.0;
            gdouble red = original != NULL ? original->red[j] : linear;
            gdouble green = original != NULL ? original->green[j] : linear;
            gdouble blue = original != NULL ? original->blue[j] : linear;

            ramp->red[j] = (unsigned short) CLAMP(red * m[0] + 0.5, 0.0, 65535.0);
            ramp->green[j] = (unsigned short) CLAMP(green * m[1] + 0.5, 0.0, 65535.0);
            ramp->blue[j] = (unsigned short) CLAMP(blue * m[2] + 0.5, 0.0, 65535.0);
        }

        XRRSetCrtcGamma(self->display, crtc, ramp);
        XRRFreeGamma(ramp);
    }

    if (resources != NULL)
        XRRFreeScreenResources(resources);

    XFlush(self->display);
    gdk_x11_display_error_trap_pop_ignored(self->gdk_display);
}

/* Puts back, unchanged, the ramps that were in place before Night Light
 * touched them, and forgets them. Does nothing if there are none. */
static void
restore_originals(ExtrasMenuNightLight *self)
{
    if (self->saved->len == 0)
        return;

    gdk_x11_display_error_trap_push(self->gdk_display);

    for (guint i = 0; i < self->saved->len; i++)
    {
        SavedGamma *saved = &g_array_index(self->saved, SavedGamma, i);

        XRRSetCrtcGamma(self->display, saved->crtc, saved->gamma);
    }

    XFlush(self->display);
    gdk_x11_display_error_trap_pop_ignored(self->gdk_display);

    g_array_remove_range(self->saved, 0, self->saved->len);
}

/* Moves the screen to `kelvin` at once. */
static void
set_temperature_now(ExtrasMenuNightLight *self, gdouble kelvin)
{
    self->current_k = kelvin;

    if (kelvin >= NEUTRAL_K)
        restore_originals(self); /* exactly as before, not "scaled by 1" */
    else
        apply_temperature(self, kelvin);
}

/* --- transitions ----------------------------------------------------------------------- */

static gboolean
on_transition_tick(gpointer user_data)
{
    ExtrasMenuNightLight *self = user_data;
    gint64 elapsed_us = g_get_monotonic_time() - self->start_us;
    gdouble t = self->duration_ms > 0
                    ? (gdouble) elapsed_us / ((gdouble) self->duration_ms * 1000.0)
                    : 1.0;

    if (t >= 1.0)
    {
        self->timer_id = 0;
        set_temperature_now(self, self->to_k);
        return G_SOURCE_REMOVE;
    }

    /* Smoothstep: starts and ends gently instead of at full speed. */
    gdouble eased = t * t * (3.0 - 2.0 * t);

    set_temperature_now(self, self->from_k + (self->to_k - self->from_k) * eased);
    return G_SOURCE_CONTINUE;
}

/* Moves the screen from wherever it is now to `target_k`, taking the
 * saved transition time. A transition already under way is replaced; the
 * new one starts from the screen's current state, so reversing half-way
 * doesn't jump. */
static void
start_transition(ExtrasMenuNightLight *self, gdouble target_k)
{
    if (!self->available)
        return;

    if (self->timer_id != 0)
    {
        g_source_remove(self->timer_id);
        self->timer_id = 0;
    }

    guint duration_ms = extras_menu_night_light_config_get_transition_ms();

    if (target_k == self->current_k)
        return;

    if (duration_ms == 0)
    {
        set_temperature_now(self, target_k);
        return;
    }

    self->from_k = self->current_k;
    self->to_k = target_k;
    self->duration_ms = duration_ms;
    self->start_us = g_get_monotonic_time();
    self->timer_id = g_timeout_add(FRAME_MS, on_transition_tick, self);
}

/* --- the pill ---------------------------------------------------------------------------- */

static void
update_tooltip(ExtrasMenuNightLight *self)
{
    if (!self->available)
    {
        gtk_widget_set_tooltip_text(self->pill,
                                    "Night Light unavailable\n"
                                    "(it needs an X11 session with the RandR extension)");
        return;
    }

    guint temperature = extras_menu_night_light_config_get_temperature();
    gchar *tooltip;

    if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(self->pill)))
        tooltip = g_strdup_printf("Night Light: on (%u K)\nClick to turn off", temperature);
    else
        tooltip = g_strdup_printf("Night Light: off\nClick to warm the screen (%u K)", temperature);

    gtk_widget_set_tooltip_text(self->pill, tooltip);
    g_free(tooltip);
}

static void
on_pill_toggled(GtkToggleButton *button, gpointer user_data)
{
    ExtrasMenuNightLight *self = user_data;
    gboolean on = gtk_toggle_button_get_active(button);

    start_transition(self, on ? (gdouble) extras_menu_night_light_config_get_temperature()
                              : NEUTRAL_K);
    update_tooltip(self);
}

/* The preferences editor saved: if Night Light is on, move to the (maybe
 * new) temperature. Off, there is nothing to move; the tooltip still
 * learns the new value. */
static void
on_config_changed(gpointer user_data)
{
    ExtrasMenuNightLight *self = user_data;

    if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(self->pill)))
        start_transition(self, (gdouble) extras_menu_night_light_config_get_temperature());

    update_tooltip(self);
}

/* --- setup and teardown --------------------------------------------------------------------- */

ExtrasMenuNightLight *
extras_menu_night_light_new(GtkWidget *pill)
{
    if (pill == NULL || !GTK_IS_TOGGLE_BUTTON(pill))
        return NULL;

    ExtrasMenuNightLight *self = g_new0(ExtrasMenuNightLight, 1);
    self->pill = g_object_ref(pill);
    self->current_k = NEUTRAL_K;

    self->saved = g_array_new(FALSE, FALSE, sizeof(SavedGamma));
    g_array_set_clear_func(self->saved, saved_gamma_clear);

    GdkDisplay *display = gdk_display_get_default();

    if (display != NULL && GDK_IS_X11_DISPLAY(display))
    {
        Display *xdisplay = GDK_DISPLAY_XDISPLAY(display);
        gint event_base = 0;
        gint error_base = 0;
        gint major = 0;
        gint minor = 0;

        /* Per-output gamma came with RandR 1.2. */
        if (XRRQueryExtension(xdisplay, &event_base, &error_base) &&
            XRRQueryVersion(xdisplay, &major, &minor) &&
            (major > 1 || (major == 1 && minor >= 2)))
        {
            self->gdk_display = display;
            self->display = xdisplay;
            self->root = DefaultRootWindow(xdisplay);
            self->available = TRUE;
        }
    }

    if (!self->available)
        g_warning("extras-menu: Night Light needs an X11 session with RandR 1.2 or newer");

    gtk_widget_set_sensitive(pill, self->available);

    self->toggled_id = g_signal_connect(pill, "toggled", G_CALLBACK(on_pill_toggled), self);
    self->config_listener =
        extras_menu_night_light_config_add_listener(on_config_changed, self);

    update_tooltip(self);
    return self;
}

void
extras_menu_night_light_free(ExtrasMenuNightLight *self)
{
    if (self == NULL)
        return;

    if (self->timer_id != 0)
        g_source_remove(self->timer_id);

    extras_menu_night_light_config_remove_listener(self->config_listener);
    g_signal_handler_disconnect(self->pill, self->toggled_id);

    /* Whatever state the screen was left in (even half-way through a
     * transition), it goes back to normal now. */
    if (self->available)
        restore_originals(self);

    g_array_free(self->saved, TRUE);
    g_object_unref(self->pill);
    g_free(self);
}