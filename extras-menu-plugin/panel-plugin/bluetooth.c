#include "bluetooth.h"

#include <gio/gio.h>

#define BLUEZ_BUS_NAME    "org.bluez"
#define BLUEZ_ADAPTER_IFACE "org.bluez.Adapter1"

struct _ExtrasMenuBluetooth
{
    GDBusConnection *system_bus;

    /* Object path of the first adapter we found, e.g. "/org/bluez/hci0".
     * NULL until (and unless) one is found. */
    gchar *adapter_path;

    guint properties_changed_subscription_id;

    ExtrasMenuBluetoothChangedFunc callback;
    gpointer user_data;
};

/* --- helpers ------------------------------------------------------------ */

static void
emit_state(ExtrasMenuBluetooth *bt, gboolean available, gboolean powered)
{
    if (bt->callback != NULL)
        bt->callback(available, powered, bt->user_data);
}

/* --- reading the adapter's Powered property ----------------------------- */

static void
on_get_powered_finished(GObject *source, GAsyncResult *result, gpointer user_data)
{
    ExtrasMenuBluetooth *bt = user_data;
    GError *error = NULL;

    GVariant *reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), result, &error);
    if (reply == NULL)
    {
        /* Adapter may have disappeared (unplugged dongle, bluetoothd
         * restarted...); report unavailable rather than leaving stale
         * state in the UI. */
        g_clear_error(&error);
        emit_state(bt, FALSE, FALSE);
        return;
    }

    /* Properties.Get returns "(v)" -- a single variant wrapping the
     * actual boolean. */
    GVariant *boxed_value = NULL;
    g_variant_get(reply, "(v)", &boxed_value);
    gboolean powered = g_variant_get_boolean(boxed_value);
    g_variant_unref(boxed_value);
    g_variant_unref(reply);

    emit_state(bt, TRUE, powered);
}

static void
request_powered_state(ExtrasMenuBluetooth *bt)
{
    if (bt->adapter_path == NULL)
        return;

    g_dbus_connection_call(
        bt->system_bus,
        BLUEZ_BUS_NAME,
        bt->adapter_path,
        "org.freedesktop.DBus.Properties",
        "Get",
        g_variant_new("(ss)", BLUEZ_ADAPTER_IFACE, "Powered"),
        G_VARIANT_TYPE("(v)"),
        G_DBUS_CALL_FLAGS_NONE,
        -1, NULL,
        on_get_powered_finished, bt);
}

/* --- live change notifications ------------------------------------------ */

/* Fired by the bus whenever any property on the adapter object changes.
 * We don't inspect which property changed -- just re-read Powered,
 * which is cheap and keeps the logic simple. */
static void
on_properties_changed(GDBusConnection *connection, const gchar *sender_name,
                       const gchar *object_path, const gchar *interface_name,
                       const gchar *signal_name, GVariant *parameters,
                       gpointer user_data)
{
    ExtrasMenuBluetooth *bt = user_data;
    (void) connection;
    (void) sender_name;
    (void) interface_name;
    (void) signal_name;
    (void) parameters;

    if (bt->adapter_path != NULL && g_strcmp0(object_path, bt->adapter_path) == 0)
        request_powered_state(bt);
}

/* --- finding the first adapter ------------------------------------------- */

static void
on_get_managed_objects_finished(GObject *source, GAsyncResult *result, gpointer user_data)
{
    ExtrasMenuBluetooth *bt = user_data;
    GError *error = NULL;

    GVariant *reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), result, &error);
    if (reply == NULL)
    {
        /* bluetoothd not running, or no D-Bus access -- no Bluetooth
         * support available on this system. */
        g_clear_error(&error);
        emit_state(bt, FALSE, FALSE);
        return;
    }

    /* GetManagedObjects returns "(a{oa{sa{sv}}})": a dict from object
     * path to a dict of interface name -> properties dict. We just
     * need the first object path that implements org.bluez.Adapter1. */
    GVariant *objects = NULL;
    g_variant_get(reply, "(@a{oa{sa{sv}}})", &objects);

    GVariantIter iter;
    g_variant_iter_init(&iter, objects);

    const gchar *path = NULL;
    GVariant *interfaces = NULL;

    while (g_variant_iter_loop(&iter, "{&o@a{sa{sv}}}", &path, &interfaces))
    {
        if (g_variant_lookup_value(interfaces, BLUEZ_ADAPTER_IFACE, NULL) != NULL)
        {
            bt->adapter_path = g_strdup(path);
            break;
        }
    }

    g_variant_unref(objects);
    g_variant_unref(reply);

    if (bt->adapter_path == NULL)
    {
        /* No Bluetooth adapter present. */
        emit_state(bt, FALSE, FALSE);
        return;
    }

    /* Now that we know the adapter's path, subscribe to its property
     * changes and read the current Powered state. */
    bt->properties_changed_subscription_id = g_dbus_connection_signal_subscribe(
        bt->system_bus,
        BLUEZ_BUS_NAME,
        "org.freedesktop.DBus.Properties",
        "PropertiesChanged",
        bt->adapter_path,
        NULL,
        G_DBUS_SIGNAL_FLAGS_NONE,
        on_properties_changed, bt, NULL);

    request_powered_state(bt);
}

static void
find_adapter(ExtrasMenuBluetooth *bt)
{
    g_dbus_connection_call(
        bt->system_bus,
        BLUEZ_BUS_NAME,
        "/",
        "org.freedesktop.DBus.ObjectManager",
        "GetManagedObjects",
        NULL,
        G_VARIANT_TYPE("(a{oa{sa{sv}}})"),
        G_DBUS_CALL_FLAGS_NONE,
        -1, NULL,
        on_get_managed_objects_finished, bt);
}

/* --- bus connection lifecycle --------------------------------------------- */

static void
on_bus_get_finished(GObject *source, GAsyncResult *result, gpointer user_data)
{
    ExtrasMenuBluetooth *bt = user_data;
    (void) source;

    GError *error = NULL;
    bt->system_bus = g_bus_get_finish(result, &error);

    if (bt->system_bus == NULL)
    {
        /* No system bus access -- treat Bluetooth as entirely
         * unavailable rather than retrying indefinitely. */
        g_clear_error(&error);
        emit_state(bt, FALSE, FALSE);
        return;
    }

    find_adapter(bt);
}

/* --- public API ------------------------------------------------------------ */

ExtrasMenuBluetooth *
extras_menu_bluetooth_new(ExtrasMenuBluetoothChangedFunc callback, gpointer user_data)
{
    ExtrasMenuBluetooth *bt = g_new0(ExtrasMenuBluetooth, 1);
    bt->callback = callback;
    bt->user_data = user_data;
    bt->adapter_path = NULL;
    bt->properties_changed_subscription_id = 0;

    g_bus_get(G_BUS_TYPE_SYSTEM, NULL, on_bus_get_finished, bt);

    return bt;
}

void
extras_menu_bluetooth_set_powered(ExtrasMenuBluetooth *bt, gboolean powered)
{
    if (bt == NULL || bt->system_bus == NULL || bt->adapter_path == NULL)
        return;

    g_dbus_connection_call(
        bt->system_bus,
        BLUEZ_BUS_NAME,
        bt->adapter_path,
        "org.freedesktop.DBus.Properties",
        "Set",
        g_variant_new("(ssv)", BLUEZ_ADAPTER_IFACE, "Powered",
                      g_variant_new_boolean(powered)),
        NULL,
        G_DBUS_CALL_FLAGS_NONE,
        -1, NULL,
        NULL, NULL); /* fire-and-forget: PropertiesChanged brings the
                       * confirmed state back to us either way */
}

void
extras_menu_bluetooth_free(ExtrasMenuBluetooth *bt)
{
    if (bt == NULL)
        return;

    if (bt->system_bus != NULL)
    {
        if (bt->properties_changed_subscription_id != 0)
            g_dbus_connection_signal_unsubscribe(bt->system_bus,
                                                  bt->properties_changed_subscription_id);
        g_object_unref(bt->system_bus);
    }

    g_free(bt->adapter_path);
    g_free(bt);
}