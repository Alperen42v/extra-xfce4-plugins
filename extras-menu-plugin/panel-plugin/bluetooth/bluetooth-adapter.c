/*
 * Bluetooth backend: everything about the adapter -- finding it (and
 * noticing when it appears or goes away), its Powered state, scanning
 * for nearby devices, and the live change notifications from BlueZ that
 * keep all of that, and the device records, up to date.
 */

#include "bluetooth-private.h"

/* --- reading the adapter's Powered property ----------------------------- */

/* Records and reports a new Powered value. */
static void
set_powered_state(ExtrasMenuBluetooth *bt, gboolean powered)
{
    bt->powered = powered;

    /* BlueZ ends any scan when the adapter powers down. */
    if (!powered)
        bt->discovery_active = FALSE;

    bluetooth_emit_state(bt, TRUE, powered);
    bluetooth_apply_discovery(bt);
    bluetooth_schedule_devices_emit(bt);
}

static void
on_get_powered_finished(GObject *source, GAsyncResult *result, gpointer user_data)
{
    ExtrasMenuBluetooth *bt = user_data;
    GError *error = NULL;

    GVariant *reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), result, &error);
    if (reply == NULL && call_was_cancelled(error))
    {
        g_clear_error(&error);
        return;
    }

    if (reply == NULL)
    {
        /* Adapter may have disappeared (unplugged dongle, bluetoothd
         * restarted...); report unavailable rather than leaving stale
         * state in the UI. */
        g_clear_error(&error);
        bluetooth_emit_state(bt, FALSE, FALSE);
        return;
    }

    /* Properties.Get returns "(v)" -- a single variant wrapping the
     * actual boolean. */
    GVariant *boxed_value = NULL;
    g_variant_get(reply, "(v)", &boxed_value);
    gboolean powered = g_variant_get_boolean(boxed_value);
    g_variant_unref(boxed_value);
    g_variant_unref(reply);

    set_powered_state(bt, powered);
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
        PROPERTIES_IFACE,
        "Get",
        g_variant_new("(ss)", BLUEZ_ADAPTER_IFACE, "Powered"),
        G_VARIANT_TYPE("(v)"),
        G_DBUS_CALL_FLAGS_NONE,
        -1, bt->cancellable,
        on_get_powered_finished, bt);
}

/* --- scanning for nearby devices ------------------------------------------------ */

static void
on_start_discovery_finished(GObject *source, GAsyncResult *result, gpointer user_data)
{
    ExtrasMenuBluetooth *bt = user_data;
    GError *error = NULL;

    GVariant *reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), result, &error);
    if (reply == NULL && call_was_cancelled(error))
    {
        g_clear_error(&error);
        return;
    }

    if (reply != NULL)
    {
        g_variant_unref(reply);
        return;
    }

    /* "InProgress" means a scan is already running, which is what we
     * wanted anyway. Anything else: it isn't running, so allow a later
     * bluetooth_apply_discovery() to try again. */
    if (!error_is(error, "org.bluez.Error.InProgress"))
        bt->discovery_active = FALSE;

    g_clear_error(&error);
}

/* Brings the real scanning state in line with what's wanted: scanning
 * runs only while the UI wants it AND the adapter is powered. */
void
bluetooth_apply_discovery(ExtrasMenuBluetooth *bt)
{
    if (bt->system_bus == NULL || bt->adapter_path == NULL)
        return;

    gboolean should_run = bt->discovery_wanted && bt->powered;

    if (should_run && !bt->discovery_active)
    {
        bt->discovery_active = TRUE;
        g_dbus_connection_call(
            bt->system_bus, BLUEZ_BUS_NAME, bt->adapter_path, BLUEZ_ADAPTER_IFACE,
            "StartDiscovery", NULL, NULL, G_DBUS_CALL_FLAGS_NONE, -1, bt->cancellable,
            on_start_discovery_finished, bt);
    }
    else if (!should_run && bt->discovery_active)
    {
        bt->discovery_active = FALSE;

        /* Powered off already stopped it. Otherwise ask BlueZ to; the
         * reply isn't interesting, and a scan that outlives us is
         * stopped by BlueZ itself when our bus connection goes away. */
        if (bt->powered)
        {
            g_dbus_connection_call(
                bt->system_bus, BLUEZ_BUS_NAME, bt->adapter_path, BLUEZ_ADAPTER_IFACE,
                "StopDiscovery", NULL, NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL,
                NULL, NULL);
        }
    }
}

/* --- live change notifications ------------------------------------------ */

/* Fired for every PropertiesChanged BlueZ emits. Parameters are
 * "(sa{sv}as)": interface name, changed properties, invalidated names. */
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

    const gchar *changed_interface = NULL;
    GVariant *changed = NULL;
    g_variant_get(parameters, "(&s@a{sv}as)", &changed_interface, &changed, NULL);

    if (g_strcmp0(changed_interface, BLUEZ_ADAPTER_IFACE) == 0)
    {
        gboolean powered = FALSE;
        if (bt->adapter_path != NULL && g_strcmp0(object_path, bt->adapter_path) == 0 &&
            g_variant_lookup(changed, "Powered", "b", &powered))
        {
            set_powered_state(bt, powered);
        }
    }
    else if (g_strcmp0(changed_interface, BLUEZ_DEVICE_IFACE) == 0)
    {
        DeviceInfo *device = g_hash_table_lookup(bt->devices, object_path);
        if (device != NULL && bluetooth_device_apply_properties(device, changed))
            bluetooth_schedule_devices_emit(bt);
    }

    g_variant_unref(changed);
}

static void find_adapter(ExtrasMenuBluetooth *bt);

/* The adapter went away (dongle unplugged): forget everything about it
 * and report that Bluetooth is unavailable. */
static void
lose_adapter(ExtrasMenuBluetooth *bt)
{
    g_clear_pointer(&bt->adapter_path, g_free);
    bt->powered = FALSE;
    bt->discovery_active = FALSE;
    g_hash_table_remove_all(bt->devices);

    bluetooth_emit_state(bt, FALSE, FALSE);
    bluetooth_schedule_devices_emit(bt);
}

/* Fired when BlueZ objects appear or disappear: devices (found by a
 * scan, or forgotten) and adapters (hotplug). InterfacesAdded carries
 * "(oa{sa{sv}})", InterfacesRemoved "(oas)". */
static void
on_objects_changed(GDBusConnection *connection, const gchar *sender_name,
                    const gchar *object_path, const gchar *interface_name,
                    const gchar *signal_name, GVariant *parameters,
                    gpointer user_data)
{
    ExtrasMenuBluetooth *bt = user_data;
    (void) connection;
    (void) sender_name;
    (void) object_path;
    (void) interface_name;

    if (g_strcmp0(signal_name, "InterfacesAdded") == 0)
    {
        const gchar *path = NULL;
        GVariant *interfaces = NULL;
        g_variant_get(parameters, "(&o@a{sa{sv}})", &path, &interfaces);

        GVariant *device_props = g_variant_lookup_value(interfaces, BLUEZ_DEVICE_IFACE,
                                                         G_VARIANT_TYPE("a{sv}"));
        if (device_props != NULL)
        {
            const gchar *owner = NULL;
            if (bt->adapter_path != NULL &&
                g_variant_lookup(device_props, "Adapter", "&o", &owner) &&
                g_strcmp0(owner, bt->adapter_path) == 0)
            {
                g_hash_table_replace(bt->devices, g_strdup(path), bluetooth_device_info_new(path, device_props));
                bluetooth_schedule_devices_emit(bt);
            }
            g_variant_unref(device_props);
        }

        /* An adapter appeared and we don't have one yet: look again. */
        if (bt->adapter_path == NULL)
        {
            GVariant *adapter_props = g_variant_lookup_value(interfaces, BLUEZ_ADAPTER_IFACE,
                                                              G_VARIANT_TYPE("a{sv}"));
            if (adapter_props != NULL)
            {
                g_variant_unref(adapter_props);
                find_adapter(bt);
            }
        }

        g_variant_unref(interfaces);
    }
    else if (g_strcmp0(signal_name, "InterfacesRemoved") == 0)
    {
        const gchar *path = NULL;
        GVariant *removed = NULL;
        g_variant_get(parameters, "(&o@as)", &path, &removed);

        const gchar **names = g_variant_get_strv(removed, NULL);
        for (guint i = 0; names[i] != NULL; i++)
        {
            if (g_strcmp0(names[i], BLUEZ_DEVICE_IFACE) == 0)
            {
                if (g_hash_table_remove(bt->devices, path))
                    bluetooth_schedule_devices_emit(bt);
            }
            else if (g_strcmp0(names[i], BLUEZ_ADAPTER_IFACE) == 0 &&
                     g_strcmp0(path, bt->adapter_path) == 0)
            {
                lose_adapter(bt);
            }
        }

        g_free(names);
        g_variant_unref(removed);
    }
}

/* --- finding the first adapter ------------------------------------------- */

static void
on_get_managed_objects_finished(GObject *source, GAsyncResult *result, gpointer user_data)
{
    ExtrasMenuBluetooth *bt = user_data;
    GError *error = NULL;

    GVariant *reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), result, &error);
    if (reply == NULL && call_was_cancelled(error))
    {
        g_clear_error(&error);
        return;
    }

    if (reply == NULL)
    {
        /* bluetoothd not running, or no D-Bus access -- no Bluetooth
         * support available on this system. */
        g_clear_error(&error);
        bluetooth_emit_state(bt, FALSE, FALSE);
        bluetooth_schedule_devices_emit(bt);
        return;
    }

    /* GetManagedObjects returns "(a{oa{sa{sv}}})": a dict from object
     * path to a dict of interface name -> properties dict. First pass:
     * the first object that implements org.bluez.Adapter1. */
    GVariant *objects = NULL;
    g_variant_get(reply, "(@a{oa{sa{sv}}})", &objects);

    GVariantIter iter;
    const gchar *path = NULL;
    GVariant *interfaces = NULL;

    if (bt->adapter_path == NULL)
    {
        g_variant_iter_init(&iter, objects);
        while (bt->adapter_path == NULL &&
               g_variant_iter_next(&iter, "{&o@a{sa{sv}}}", &path, &interfaces))
        {
            GVariant *adapter_props = g_variant_lookup_value(interfaces, BLUEZ_ADAPTER_IFACE,
                                                              G_VARIANT_TYPE("a{sv}"));
            if (adapter_props != NULL)
            {
                bt->adapter_path = g_strdup(path);
                g_variant_unref(adapter_props);
            }
            g_variant_unref(interfaces);
        }
    }

    if (bt->adapter_path == NULL)
    {
        /* No Bluetooth adapter present. */
        g_variant_unref(objects);
        g_variant_unref(reply);
        bluetooth_emit_state(bt, FALSE, FALSE);
        bluetooth_schedule_devices_emit(bt);
        return;
    }

    /* Second pass: every device that belongs to that adapter. */
    g_hash_table_remove_all(bt->devices);
    g_variant_iter_init(&iter, objects);
    while (g_variant_iter_next(&iter, "{&o@a{sa{sv}}}", &path, &interfaces))
    {
        GVariant *device_props = g_variant_lookup_value(interfaces, BLUEZ_DEVICE_IFACE,
                                                         G_VARIANT_TYPE("a{sv}"));
        if (device_props != NULL)
        {
            const gchar *owner = NULL;
            if (g_variant_lookup(device_props, "Adapter", "&o", &owner) &&
                g_strcmp0(owner, bt->adapter_path) == 0)
            {
                g_hash_table_replace(bt->devices, g_strdup(path), bluetooth_device_info_new(path, device_props));
            }
            g_variant_unref(device_props);
        }
        g_variant_unref(interfaces);
    }

    g_variant_unref(objects);
    g_variant_unref(reply);

    bluetooth_agent_register_with_bluez(bt);
    request_powered_state(bt);
    bluetooth_schedule_devices_emit(bt);
}

static void
find_adapter(ExtrasMenuBluetooth *bt)
{
    g_dbus_connection_call(
        bt->system_bus,
        BLUEZ_BUS_NAME,
        "/",
        OBJECT_MANAGER_IFACE,
        "GetManagedObjects",
        NULL,
        G_VARIANT_TYPE("(a{oa{sa{sv}}})"),
        G_DBUS_CALL_FLAGS_NONE,
        -1, bt->cancellable,
        on_get_managed_objects_finished, bt);
}

/* --- starting it all ------------------------------------------------------- */

void
bluetooth_start_tracking(ExtrasMenuBluetooth *bt)
{
    /* Subscribe before asking for the current state, so nothing that
     * happens in between is missed. */
    bt->properties_changed_subscription_id = g_dbus_connection_signal_subscribe(
        bt->system_bus, BLUEZ_BUS_NAME, PROPERTIES_IFACE, "PropertiesChanged",
        NULL, NULL, G_DBUS_SIGNAL_FLAGS_NONE,
        on_properties_changed, bt, NULL);

    bt->objects_changed_subscription_id = g_dbus_connection_signal_subscribe(
        bt->system_bus, BLUEZ_BUS_NAME, OBJECT_MANAGER_IFACE, NULL,
        "/", NULL, G_DBUS_SIGNAL_FLAGS_NONE,
        on_objects_changed, bt, NULL);

    find_adapter(bt);
}