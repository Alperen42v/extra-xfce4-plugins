/*
 * Bluetooth backend: bus connection, and the simple public entry points
 * (new, free, set_powered, set_discovering). See bluetooth-private.h for
 * how the backend is split across files.
 */

#include "bluetooth-private.h"

/* --- reporting ---------------------------------------------------------------- */

void
bluetooth_emit_state(ExtrasMenuBluetooth *bt, gboolean available, gboolean powered)
{
    if (bt->callback != NULL)
        bt->callback(available, powered, bt->user_data);
}

/* --- bus connection lifecycle --------------------------------------------- */

static void
on_bus_get_finished(GObject *source, GAsyncResult *result, gpointer user_data)
{
    ExtrasMenuBluetooth *bt = user_data;
    (void) source;

    GError *error = NULL;
    GDBusConnection *bus = g_bus_get_finish(result, &error);

    /* Cancelled means extras_menu_bluetooth_free() already ran and `bt`
     * no longer exists, so it can't be written to here. */
    if (bus == NULL && call_was_cancelled(error))
    {
        g_clear_error(&error);
        return;
    }

    bt->system_bus = bus;

    if (bt->system_bus == NULL)
    {
        /* No system bus access -- treat Bluetooth as entirely
         * unavailable rather than retrying indefinitely. */
        g_clear_error(&error);
        bluetooth_emit_state(bt, FALSE, FALSE);
        return;
    }

    /* Export our pairing agent first, so it exists by the time the
     * adapter is found and the agent gets registered with BlueZ; then
     * start listening to BlueZ and look for the adapter. */
    bluetooth_agent_export(bt);
    bluetooth_start_tracking(bt);
}

/* --- public API ------------------------------------------------------------ */

ExtrasMenuBluetooth *
extras_menu_bluetooth_new(ExtrasMenuBluetoothChangedFunc callback,
                          ExtrasMenuBluetoothDevicesChangedFunc devices_callback,
                          gpointer user_data)
{
    ExtrasMenuBluetooth *bt = g_new0(ExtrasMenuBluetooth, 1);
    bt->callback = callback;
    bt->devices_callback = devices_callback;
    bt->user_data = user_data;
    bt->cancellable = g_cancellable_new();
    bt->devices = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, bluetooth_device_info_free);

    g_bus_get(G_BUS_TYPE_SYSTEM, bt->cancellable, on_bus_get_finished, bt);

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
        PROPERTIES_IFACE,
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
extras_menu_bluetooth_set_discovering(ExtrasMenuBluetooth *bt, gboolean discovering)
{
    if (bt == NULL)
        return;

    bt->discovery_wanted = discovering;
    bluetooth_apply_discovery(bt);
}

void
extras_menu_bluetooth_free(ExtrasMenuBluetooth *bt)
{
    if (bt == NULL)
        return;

    /* Cancel everything still in flight first; see the cancellable's
     * comment in the struct for why the callbacks check for this. */
    g_cancellable_cancel(bt->cancellable);

    if (bt->devices_emit_source_id != 0)
        g_source_remove(bt->devices_emit_source_id);

    if (bt->system_bus != NULL)
    {
        /* Leave no scan running behind us. (BlueZ would also end it when
         * our bus connection closes, but that may be a long way off.) */
        if (bt->discovery_active && bt->powered && bt->adapter_path != NULL)
        {
            g_dbus_connection_call(
                bt->system_bus, BLUEZ_BUS_NAME, bt->adapter_path, BLUEZ_ADAPTER_IFACE,
                "StopDiscovery", NULL, NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL,
                NULL, NULL);
        }

        if (bt->properties_changed_subscription_id != 0)
            g_dbus_connection_signal_unsubscribe(bt->system_bus,
                                                  bt->properties_changed_subscription_id);
        if (bt->objects_changed_subscription_id != 0)
            g_dbus_connection_signal_unsubscribe(bt->system_bus,
                                                  bt->objects_changed_subscription_id);

        bluetooth_agent_unexport(bt);

        g_object_unref(bt->system_bus);
    }

    g_hash_table_destroy(bt->devices);
    g_object_unref(bt->cancellable);
    g_free(bt->agent_path);
    g_free(bt->adapter_path);
    g_free(bt);
}