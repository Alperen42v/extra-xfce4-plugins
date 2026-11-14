/*
 * The Caffeine pill: keeps the screen on and the computer awake while it
 * is switched on. See caffeine.h.
 */

#include "pills/caffeine.h"
#include "preferences/caffeine-config.h"

#include <gdk/gdkx.h>
#include <gio/gio.h>
#include <X11/extensions/sync.h>

/* The D-Bus calls are made synchronously from the click handler. A
 * service that isn't running fails at once (we never ask the bus to
 * start one), so the timeout only matters for one that hangs. */
#define DBUS_CALL_TIMEOUT_MS 2000

#define INHIBIT_APP_NAME "xfce4-extras-menu"
#define INHIBIT_REASON   "Caffeine is switched on in the Extras Menu"

/* How often a running countdown looks at the clock. The deadline is a
 * wall-clock time and not a timer length because a monotonic timer
 * doesn't tick while the computer is suspended, which would stretch
 * the countdown by however long it slept. */
#define EXPIRY_CHECK_SECONDS 15

/* How often the idle time is looked at while the screen-off feature is
 * waiting (the screen goes off at most this much after the time is up). */
#define BLANK_CHECK_SECONDS 5

#define BLANK_COMMAND "xset dpms force off"

typedef struct
{
    const gchar *bus_name;
    const gchar *object_path;
    const gchar *interface_name;
} InhibitTarget;

/* Screensavers, tried in this order until one accepts. */
static const InhibitTarget screensaver_targets[] = {
    { "org.freedesktop.ScreenSaver", "/org/freedesktop/ScreenSaver", "org.freedesktop.ScreenSaver" },
    { "org.xfce.ScreenSaver",        "/org/xfce/ScreenSaver",        "org.xfce.ScreenSaver" },
    { "org.gnome.ScreenSaver",       "/org/gnome/ScreenSaver",       "org.gnome.ScreenSaver" },
};

/* Power managers (xfce4-power-manager answers on this one). */
static const InhibitTarget power_targets[] = {
    { "org.freedesktop.PowerManagement", "/org/freedesktop/PowerManagement/Inhibit",
      "org.freedesktop.PowerManagement.Inhibit" },
};

/* One accepted Inhibit() request: the service that accepted it is
 * remembered, because UnInhibit() has to go to that same one. It counts
 * as held while bus_name is set (a cookie of 0 is not known to be
 * invalid, so it can't be the marker). */
typedef struct
{
    guint cookie;
    gchar *bus_name;
    gchar *object_path;
    gchar *interface_name;
} Inhibit;

/* The cup icons some icon themes and the Caffeine app ship, and the
 * stock icon used when the theme doesn't have both of them. */
#define ICON_CUP_EMPTY "caffeine-cup-empty"
#define ICON_CUP_FULL  "caffeine-cup-full"
#define ICON_FALLBACK  "preferences-desktop-screensaver-symbolic"

struct _ExtrasMenuCaffeine
{
    GtkWidget *pill; /* holds a reference */
    GtkWidget *icon; /* the GtkImage inside the pill, holds a reference; may be NULL */
    gulong clicked_id;

    gboolean active;
    Inhibit screensaver;
    Inhibit power;

    guint expiry_id;  /* the countdown's clock check; 0 when there is none */
    gint64 deadline;  /* g_get_real_time() when it runs out; 0 = never */

    /* The screen-off feature. blank_after_ms is the saved setting (0 =
     * switched off), kept here so the check doesn't read the file every
     * few seconds. blank_id is the periodic idle check (0 = not
     * running). screen_is_off is TRUE from the moment the screen was
     * turned off until the user has been active again, so it is turned
     * off once per absence and not every few seconds. */
    gint64 blank_after_ms;
    guint blank_id;
    gboolean screen_is_off;

    guint config_listener;

    ExtrasMenuCaffeineMessageFunc message_func;
    gpointer user_data;
};

static void set_active(ExtrasMenuCaffeine *self, gboolean active);

/* --- the icon -------------------------------------------------------------------- */

const gchar *
extras_menu_caffeine_icon_name(gboolean active)
{
    GtkIconTheme *theme = gtk_icon_theme_get_default();

    if (gtk_icon_theme_has_icon(theme, ICON_CUP_EMPTY) &&
        gtk_icon_theme_has_icon(theme, ICON_CUP_FULL))
        return active ? ICON_CUP_FULL : ICON_CUP_EMPTY;

    return ICON_FALLBACK;
}

/* The pill's icon image: the first GtkImage in the box popover.c puts
 * inside the toggle button. NULL if the pill is built differently. */
static GtkWidget *
find_pill_icon(GtkWidget *pill)
{
    GtkWidget *box = gtk_bin_get_child(GTK_BIN(pill));
    GtkWidget *image = NULL;

    if (box == NULL || !GTK_IS_CONTAINER(box))
        return NULL;

    GList *children = gtk_container_get_children(GTK_CONTAINER(box));
    for (GList *l = children; l != NULL && image == NULL; l = l->next)
    {
        if (GTK_IS_IMAGE(l->data))
            image = l->data;
    }
    g_list_free(children);

    return image;
}

/* --- inhibiting ----------------------------------------------------------------- */

static void
inhibit_forget(Inhibit *inhibit)
{
    inhibit->cookie = 0;
    g_clear_pointer(&inhibit->bus_name, g_free);
    g_clear_pointer(&inhibit->object_path, g_free);
    g_clear_pointer(&inhibit->interface_name, g_free);
}

/* Asks each target in turn to inhibit; the first that accepts is
 * remembered in `out`. Returns whether one did. */
static gboolean
inhibit_acquire(GDBusConnection *connection, const InhibitTarget *targets, guint n_targets,
                Inhibit *out)
{
    for (guint i = 0; i < n_targets; i++)
    {
        GError *error = NULL;
        GVariant *result = g_dbus_connection_call_sync(
            connection,
            targets[i].bus_name, targets[i].object_path, targets[i].interface_name,
            "Inhibit",
            g_variant_new("(ss)", INHIBIT_APP_NAME, INHIBIT_REASON),
            G_VARIANT_TYPE("(u)"),
            G_DBUS_CALL_FLAGS_NO_AUTO_START,
            DBUS_CALL_TIMEOUT_MS, NULL, &error);

        if (result == NULL)
        {
            g_debug("extras-menu: caffeine: %s refused: %s", targets[i].bus_name,
                    error != NULL ? error->message : "unknown error");
            g_clear_error(&error);
            continue;
        }

        guint32 cookie = 0;
        g_variant_get(result, "(u)", &cookie);
        g_variant_unref(result);

        out->cookie = cookie;
        out->bus_name = g_strdup(targets[i].bus_name);
        out->object_path = g_strdup(targets[i].object_path);
        out->interface_name = g_strdup(targets[i].interface_name);
        return TRUE;
    }

    return FALSE;
}

/* Lets go of a held request. `connection` may be NULL (the bus can't be
 * reached any more); the request is forgotten either way. */
static void
inhibit_release(GDBusConnection *connection, Inhibit *inhibit)
{
    if (connection != NULL && inhibit->bus_name != NULL)
    {
        GError *error = NULL;
        GVariant *result = g_dbus_connection_call_sync(
            connection,
            inhibit->bus_name, inhibit->object_path, inhibit->interface_name,
            "UnInhibit",
            g_variant_new("(u)", inhibit->cookie),
            NULL,
            G_DBUS_CALL_FLAGS_NO_AUTO_START,
            DBUS_CALL_TIMEOUT_MS, NULL, &error);

        if (result != NULL)
        {
            g_variant_unref(result);
        }
        else
        {
            g_warning("extras-menu: caffeine: couldn't let go of %s: %s", inhibit->bus_name,
                      error != NULL ? error->message : "unknown error");
            g_clear_error(&error);
        }
    }

    inhibit_forget(inhibit);
}

static void
release_all(ExtrasMenuCaffeine *self)
{
    if (self->screensaver.bus_name == NULL && self->power.bus_name == NULL)
        return;

    GError *error = NULL;
    GDBusConnection *connection = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &error);

    if (connection == NULL)
    {
        g_warning("extras-menu: caffeine: no session bus: %s",
                  error != NULL ? error->message : "unknown error");
        g_clear_error(&error);
    }

    inhibit_release(connection, &self->screensaver);
    inhibit_release(connection, &self->power);

    g_clear_object(&connection);
}

/* Inhibits the screensaver and the power manager. TRUE if at least one
 * of them accepted. */
static gboolean
acquire_all(ExtrasMenuCaffeine *self)
{
    release_all(self);

    GError *error = NULL;
    GDBusConnection *connection = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &error);

    if (connection == NULL)
    {
        g_warning("extras-menu: caffeine: no session bus: %s",
                  error != NULL ? error->message : "unknown error");
        g_clear_error(&error);
        return FALSE;
    }

    gboolean screensaver_ok = inhibit_acquire(connection, screensaver_targets,
                                              G_N_ELEMENTS(screensaver_targets),
                                              &self->screensaver);
    gboolean power_ok = inhibit_acquire(connection, power_targets,
                                        G_N_ELEMENTS(power_targets), &self->power);
    g_object_unref(connection);

    /* One of the two is enough to be useful; say so in the log if only
     * half of it works. */
    if (screensaver_ok != power_ok)
    {
        g_warning("extras-menu: caffeine: only the %s accepted the request",
                  screensaver_ok ? "screensaver" : "power manager");
    }

    return screensaver_ok || power_ok;
}

/* --- the countdown ---------------------------------------------------------------- */

static void
clear_expiry(ExtrasMenuCaffeine *self)
{
    if (self->expiry_id != 0)
    {
        g_source_remove(self->expiry_id);
        self->expiry_id = 0;
    }

    self->deadline = 0;
}

static gboolean
on_expiry_check(gpointer user_data)
{
    ExtrasMenuCaffeine *self = user_data;

    if (g_get_real_time() < self->deadline)
        return G_SOURCE_CONTINUE;

    /* This source is going away by itself; set_active() must not try to
     * remove it as well. */
    self->expiry_id = 0;
    set_active(self, FALSE);

    return G_SOURCE_REMOVE;
}

/* (Re)starts the countdown from now, if Caffeine is on and the saved
 * duration isn't "unlimited"; otherwise just stops any countdown. */
static void
arm_expiry(ExtrasMenuCaffeine *self)
{
    clear_expiry(self);

    if (!self->active)
        return;

    guint minutes = extras_menu_caffeine_config_get_duration();
    if (minutes == EXTRAS_MENU_CAFFEINE_UNLIMITED)
        return;

    self->deadline = g_get_real_time() + (gint64) minutes * 60 * G_USEC_PER_SEC;
    self->expiry_id = g_timeout_add_seconds(EXPIRY_CHECK_SECONDS, on_expiry_check, self);
}

/* --- idle time ---------------------------------------------------------------------- */

/* The X server's "IDLETIME" counter (SYNC extension, libXext) counts the
 * milliseconds since the keyboard or mouse was last touched; it is the
 * number the screensaver and power manager go by, and needs no daemon of
 * its own. It is not reset by a screen being turned off or by a program
 * playing video, only by input. Its id doesn't change while the X server
 * runs, so it is looked up once, on first use. */
static gboolean idle_lookup_done = FALSE;
static XSyncCounter idle_counter = None;

static void
find_idle_counter(Display *xdisplay)
{
    int event_base, error_base, major, minor, count = 0;

    idle_lookup_done = TRUE;

    if (!XSyncQueryExtension(xdisplay, &event_base, &error_base) ||
        !XSyncInitialize(xdisplay, &major, &minor))
        return;

    XSyncSystemCounter *counters = XSyncListSystemCounters(xdisplay, &count);
    if (counters == NULL)
        return;

    for (int i = 0; i < count; i++)
    {
        if (g_strcmp0(counters[i].name, "IDLETIME") == 0)
        {
            idle_counter = counters[i].counter;
            break;
        }
    }

    XSyncFreeSystemCounterList(counters);
}

/* Milliseconds since the user last touched the keyboard or mouse, or -1
 * if that can't be told (not running under X11, or the X server has no
 * idle counter). */
static gint64
idle_time_ms(void)
{
    GdkDisplay *display = gdk_display_get_default();

    if (display == NULL || !GDK_IS_X11_DISPLAY(display))
        return -1;

    Display *xdisplay = GDK_DISPLAY_XDISPLAY(display);

    if (!idle_lookup_done)
        find_idle_counter(xdisplay);

    if (idle_counter == None)
        return -1;

    /* An X error here would make GDK abort the whole panel; trap it. */
    XSyncValue value;
    gdk_x11_display_error_trap_push(display);
    Status ok = XSyncQueryCounter(xdisplay, idle_counter, &value);
    gint error = gdk_x11_display_error_trap_pop(display);

    if (!ok || error != 0)
        return -1;

    return ((gint64) XSyncValueHigh32(value) << 32) | (gint64) XSyncValueLow32(value);
}

/* --- turning the screen off -------------------------------------------------------- */

static void
load_blank_setting(ExtrasMenuCaffeine *self)
{
    if (extras_menu_caffeine_config_get_screen_off_enabled())
        self->blank_after_ms = (gint64) extras_menu_caffeine_config_get_screen_off_minutes() * 60 * 1000;
    else
        self->blank_after_ms = 0;
}

static void
stop_blanking(ExtrasMenuCaffeine *self)
{
    if (self->blank_id != 0)
    {
        g_source_remove(self->blank_id);
        self->blank_id = 0;
    }

    self->screen_is_off = FALSE;
}

/* Switches the display off, and nothing else (no lock, no suspend).
 * Fire and forget; GLib reaps the child. */
static void
blank_screen(void)
{
    GError *error = NULL;

    if (!g_spawn_command_line_async(BLANK_COMMAND, &error))
    {
        g_warning("extras-menu: caffeine: couldn't turn the screen off with '%s': %s",
                  BLANK_COMMAND, error != NULL ? error->message : "unknown error");
        g_clear_error(&error);
    }
}

static gboolean
on_blank_check(gpointer user_data)
{
    ExtrasMenuCaffeine *self = user_data;
    gint64 idle = idle_time_ms();

    if (idle < 0)
    {
        g_warning("extras-menu: caffeine: can't tell how long the computer has been idle "
                  "(this needs X11), so the screen will not be turned off");
        self->blank_id = 0;
        return G_SOURCE_REMOVE;
    }

    if (idle < self->blank_after_ms)
    {
        /* Somebody is there (or has just come back): the next absence
         * gets its own turn-off. */
        self->screen_is_off = FALSE;
        return G_SOURCE_CONTINUE;
    }

    if (!self->screen_is_off)
    {
        self->screen_is_off = TRUE;
        blank_screen();
    }

    return G_SOURCE_CONTINUE;
}

/* (Re)starts watching for idle time, if Caffeine is on and the feature
 * is switched on; otherwise just stops any watching. */
static void
start_blanking(ExtrasMenuCaffeine *self)
{
    stop_blanking(self);

    if (!self->active || self->blank_after_ms == 0)
        return;

    self->blank_id = g_timeout_add_seconds(BLANK_CHECK_SECONDS, on_blank_check, self);
}

/* --- the pill --------------------------------------------------------------------- */

static void
show_message(ExtrasMenuCaffeine *self, const gchar *primary, const gchar *secondary)
{
    if (self->message_func != NULL)
        self->message_func(primary, secondary, self->user_data);
    else
        g_warning("extras-menu: %s %s", primary, secondary != NULL ? secondary : "");
}

/* "15 minutes", "1 hour", "1 hour 30 minutes". */
static gchar *
format_duration(guint minutes)
{
    guint hours = minutes / 60;
    guint rest = minutes % 60;

    if (hours == 0)
        return g_strdup_printf(rest == 1 ? "%u minute" : "%u minutes", rest);

    if (rest == 0)
        return g_strdup_printf(hours == 1 ? "%u hour" : "%u hours", hours);

    return g_strdup_printf("%u %s %u %s", hours, hours == 1 ? "hour" : "hours",
                           rest, rest == 1 ? "minute" : "minutes");
}

/* Makes the pill show the real state: lit while Caffeine is on. Also
 * what undoes the flip a click gives a toggle button, and what keeps the
 * tooltip up to date. */
static void
refresh(ExtrasMenuCaffeine *self)
{
    /* GTK3's gtk_toggle_button_set_active() emits "clicked" too, exactly
     * as a real click does, so our own handler must not hear this one
     * (it would switch Caffeine again). */
    g_signal_handler_block(self->pill, self->clicked_id);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(self->pill), self->active);
    g_signal_handler_unblock(self->pill, self->clicked_id);

    /* A full cup while on, an empty one while off (or the same stock
     * icon for both, see extras_menu_caffeine_icon_name()). */
    if (self->icon != NULL)
    {
        gtk_image_set_from_icon_name(GTK_IMAGE(self->icon),
                                     extras_menu_caffeine_icon_name(self->active),
                                     GTK_ICON_SIZE_BUTTON);
    }

    gchar *tooltip;

    if (self->active)
    {
        if (self->deadline != 0)
        {
            GDateTime *end = g_date_time_new_from_unix_local(self->deadline / G_USEC_PER_SEC);
            gchar *when = g_date_time_format(end, "%H:%M");

            tooltip = g_strdup_printf("Caffeine is on until %s\nClick to turn it off now", when);

            g_free(when);
            g_date_time_unref(end);
        }
        else
        {
            tooltip = g_strdup("Caffeine is on until you turn it off\nClick to turn it off now");
        }
    }
    else
    {
        guint minutes = extras_menu_caffeine_config_get_duration();

        if (minutes == EXTRAS_MENU_CAFFEINE_UNLIMITED)
        {
            tooltip = g_strdup("Caffeine is off\n"
                               "Click to keep the screen on and stop sleep until you turn it off");
        }
        else
        {
            gchar *length = format_duration(minutes);

            tooltip = g_strdup_printf("Caffeine is off\n"
                                      "Click to keep the screen on and stop sleep for %s", length);
            g_free(length);
        }
    }

    /* While it is on, say if the screen will be turned off too. */
    if (self->active && self->blank_after_ms != 0)
    {
        gchar *length = format_duration((guint) (self->blank_after_ms / 60000));
        gchar *with_screen = g_strdup_printf("%s\nThe screen turns off after %s without activity",
                                             tooltip, length);

        g_free(length);
        g_free(tooltip);
        tooltip = with_screen;
    }

    gtk_widget_set_tooltip_text(self->pill, tooltip);
    g_free(tooltip);
}

static void
set_active(ExtrasMenuCaffeine *self, gboolean active)
{
    if (active && !self->active)
    {
        if (!acquire_all(self))
        {
            show_message(self, "Caffeine could not be turned on",
                         "Neither the screensaver nor the power manager accepted the request. "
                         "Is xfce4-power-manager running?");
            refresh(self);
            return;
        }

        self->active = TRUE;
        arm_expiry(self);
        start_blanking(self);
    }
    else if (!active && self->active)
    {
        clear_expiry(self);
        stop_blanking(self);
        release_all(self);
        self->active = FALSE;
    }

    refresh(self);
}

static void
on_pill_clicked(GtkButton *button, gpointer user_data)
{
    (void) button;
    ExtrasMenuCaffeine *self = user_data;

    /* The click flipped the toggle button; set_active() ends with
     * refresh(), which makes it show the real state again. */
    set_active(self, !self->active);
}

/* A saved setting changed: a running countdown starts again with the
 * new duration, and the screen-off watching with the new setting. */
static void
on_config_changed(gpointer user_data)
{
    ExtrasMenuCaffeine *self = user_data;

    load_blank_setting(self);

    if (self->active)
    {
        arm_expiry(self);
        start_blanking(self);
    }

    refresh(self);
}

/* --- setup and teardown --------------------------------------------------------------- */

ExtrasMenuCaffeine *
extras_menu_caffeine_new(GtkWidget *pill, ExtrasMenuCaffeineMessageFunc message_func,
                         gpointer user_data)
{
    if (pill == NULL || !GTK_IS_TOGGLE_BUTTON(pill))
        return NULL;

    ExtrasMenuCaffeine *self = g_new0(ExtrasMenuCaffeine, 1);
    self->pill = g_object_ref(pill);
    self->icon = find_pill_icon(pill);
    if (self->icon != NULL)
        g_object_ref(self->icon);
    self->message_func = message_func;
    self->user_data = user_data;

    /* Clicking turns Caffeine on or off, so what the toggle does by
     * itself is not wanted: only "clicked" is used, and refresh() sets
     * the look. */
    self->clicked_id = g_signal_connect(pill, "clicked", G_CALLBACK(on_pill_clicked), self);
    self->config_listener = extras_menu_caffeine_config_add_listener(on_config_changed, self);
    load_blank_setting(self);

    refresh(self);
    return self;
}

void
extras_menu_caffeine_free(ExtrasMenuCaffeine *self)
{
    if (self == NULL)
        return;

    extras_menu_caffeine_config_remove_listener(self->config_listener);

    /* Don't leave the computer unable to sleep behind us. */
    clear_expiry(self);
    stop_blanking(self);
    release_all(self);

    g_signal_handler_disconnect(self->pill, self->clicked_id);
    g_clear_object(&self->icon);
    g_object_unref(self->pill);

    g_free(self);
}
