#include "battery.h"

#include <gio/gio.h>
#include <math.h>

#define UPOWER_BUS_NAME     "org.freedesktop.UPower"
#define UPOWER_DISPLAY_PATH "/org/freedesktop/UPower/devices/DisplayDevice"
#define UPOWER_DEVICE_IFACE "org.freedesktop.UPower.Device"

struct _ExtrasMenuBattery
{
    ExtrasMenuBatteryChangedFunc callback;
    gpointer user_data;

    GCancellable *cancellable;
    GDBusProxy *proxy;
};

/* UPower's State property -> our simplified enum. */
static ExtrasMenuBatteryState
state_from_upower(guint32 value)
{
    switch (value)
    {
    case 1:  return EXTRAS_MENU_BATTERY_STATE_CHARGING;    /* charging */
    case 2:                                                /* discharging */
    case 3:                                                /* empty */
    case 6:  return EXTRAS_MENU_BATTERY_STATE_DISCHARGING; /* pending discharge */
    case 4:  return EXTRAS_MENU_BATTERY_STATE_FULL;        /* fully charged */
    case 5:  return EXTRAS_MENU_BATTERY_STATE_PLUGGED;     /* pending charge */
    default: return EXTRAS_MENU_BATTERY_STATE_UNKNOWN;
    }
}

/* Reads the proxy's cached properties and reports them. The proxy
 * keeps its cache up to date by itself from PropertiesChanged
 * signals, so this never does a D-Bus round trip. */
static void
report(ExtrasMenuBattery *battery)
{
    gboolean present = FALSE;
    guint percent = 0;
    ExtrasMenuBatteryState state = EXTRAS_MENU_BATTERY_STATE_UNKNOWN;
    gint64 seconds = 0;

    gchar *owner = g_dbus_proxy_get_name_owner(battery->proxy);
    if (owner != NULL)
    {
        GVariant *v = g_dbus_proxy_get_cached_property(battery->proxy, "IsPresent");
        if (v != NULL)
        {
            present = g_variant_get_boolean(v);
            g_variant_unref(v);
        }

        if (present)
        {
            v = g_dbus_proxy_get_cached_property(battery->proxy, "Percentage");
            if (v != NULL)
            {
                gdouble d = g_variant_get_double(v);
                percent = (guint) CLAMP(floor(d + 0.5), 0.0, 100.0);
                g_variant_unref(v);
            }

            v = g_dbus_proxy_get_cached_property(battery->proxy, "State");
            if (v != NULL)
            {
                state = state_from_upower(g_variant_get_uint32(v));
                g_variant_unref(v);
            }

            const gchar *time_property =
                (state == EXTRAS_MENU_BATTERY_STATE_CHARGING) ? "TimeToFull"
                : (state == EXTRAS_MENU_BATTERY_STATE_DISCHARGING) ? "TimeToEmpty"
                : NULL;
            if (time_property != NULL)
            {
                v = g_dbus_proxy_get_cached_property(battery->proxy, time_property);
                if (v != NULL)
                {
                    seconds = g_variant_get_int64(v);
                    g_variant_unref(v);
                }
            }
        }
    }
    g_free(owner);

    battery->callback(present, percent, state, seconds, battery->user_data);
}

static void
on_properties_changed(GDBusProxy *proxy, GVariant *changed, GStrv invalidated,
                      gpointer user_data)
{
    (void) proxy;
    (void) changed;
    (void) invalidated;
    report(user_data);
}

/* UPower stopped/started (or crashed and restarted). */
static void
on_name_owner_changed(GObject *object, GParamSpec *pspec, gpointer user_data)
{
    (void) object;
    (void) pspec;
    report(user_data);
}

static void
on_proxy_ready(GObject *source, GAsyncResult *result, gpointer user_data)
{
    (void) source;

    GError *error = NULL;
    GDBusProxy *proxy = g_dbus_proxy_new_for_bus_finish(result, &error);

    /* On cancellation the backend may already be freed, so don't
     * touch user_data in that case. */
    if (proxy == NULL)
    {
        if (!g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        {
            /* No UPower: the badge simply stays hidden. */
            g_debug("extras-menu: UPower unavailable: %s", error->message);
        }
        g_clear_error(&error);
        return;
    }

    ExtrasMenuBattery *battery = user_data;
    battery->proxy = proxy;

    g_signal_connect(proxy, "g-properties-changed",
                      G_CALLBACK(on_properties_changed), battery);
    g_signal_connect(proxy, "notify::g-name-owner",
                      G_CALLBACK(on_name_owner_changed), battery);

    report(battery);
}

ExtrasMenuBattery *
extras_menu_battery_new(ExtrasMenuBatteryChangedFunc callback, gpointer user_data)
{
    ExtrasMenuBattery *battery = g_new0(ExtrasMenuBattery, 1);
    battery->callback = callback;
    battery->user_data = user_data;
    battery->cancellable = g_cancellable_new();

    g_dbus_proxy_new_for_bus(G_BUS_TYPE_SYSTEM, G_DBUS_PROXY_FLAGS_NONE, NULL,
                              UPOWER_BUS_NAME, UPOWER_DISPLAY_PATH,
                              UPOWER_DEVICE_IFACE, battery->cancellable,
                              on_proxy_ready, battery);

    return battery;
}

void
extras_menu_battery_free(ExtrasMenuBattery *battery)
{
    if (battery == NULL)
        return;

    g_cancellable_cancel(battery->cancellable);
    g_object_unref(battery->cancellable);

    if (battery->proxy != NULL)
    {
        g_signal_handlers_disconnect_by_data(battery->proxy, battery);
        g_object_unref(battery->proxy);
    }

    g_free(battery);
}

gchar *
extras_menu_battery_icon_name(guint percent, ExtrasMenuBatteryState state)
{
    if (state == EXTRAS_MENU_BATTERY_STATE_FULL)
        return g_strdup("battery-full-charged-symbolic");

    const gchar *level;
    if (percent >= 80)
        level = "full";
    else if (percent >= 50)
        level = "good";
    else if (percent >= 20)
        level = "low";
    else if (percent >= 8)
        level = "caution";
    else
        level = "empty";

    return g_strdup_printf("battery-%s%s-symbolic", level,
                            state == EXTRAS_MENU_BATTERY_STATE_CHARGING ? "-charging" : "");
}

/* 4500 -> "1 h 15 min", 1200 -> "20 min". */
static gchar *
format_duration(gint64 seconds)
{
    gint64 minutes = (seconds + 30) / 60;
    if (minutes < 1)
        minutes = 1;

    if (minutes >= 60)
        return g_strdup_printf("%d h %d min", (gint) (minutes / 60), (gint) (minutes % 60));

    return g_strdup_printf("%d min", (gint) minutes);
}

gchar *
extras_menu_battery_describe(guint percent, ExtrasMenuBatteryState state, gint64 seconds)
{
    gchar *duration = seconds > 0 ? format_duration(seconds) : NULL;
    gchar *text;

    switch (state)
    {
    case EXTRAS_MENU_BATTERY_STATE_CHARGING:
        text = duration != NULL
            ? g_strdup_printf("Charging (%u%%) - %s until full", percent, duration)
            : g_strdup_printf("Charging (%u%%)", percent);
        break;
    case EXTRAS_MENU_BATTERY_STATE_DISCHARGING:
        text = duration != NULL
            ? g_strdup_printf("On battery (%u%%) - %s remaining", percent, duration)
            : g_strdup_printf("On battery (%u%%)", percent);
        break;
    case EXTRAS_MENU_BATTERY_STATE_FULL:
        text = g_strdup("Fully charged");
        break;
    case EXTRAS_MENU_BATTERY_STATE_PLUGGED:
        text = g_strdup_printf("Plugged in, not charging (%u%%)", percent);
        break;
    default:
        text = g_strdup_printf("Battery (%u%%)", percent);
        break;
    }

    g_free(duration);
    return text;
}