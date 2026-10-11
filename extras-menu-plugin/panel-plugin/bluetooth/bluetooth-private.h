#ifndef EXTRAS_MENU_BLUETOOTH_PRIVATE_H
#define EXTRAS_MENU_BLUETOOTH_PRIVATE_H

/*
 * Internals shared by the Bluetooth backend's source files. Nothing
 * outside the bluetooth/ folder includes this; the public API is
 * bluetooth.h.
 *
 *   bluetooth.c          bus connection, new/free, set_powered,
 *                        set_discovering
 *   bluetooth-adapter.c  finding the adapter, its Powered state, scanning
 *                        for nearby devices, live change notifications
 *   bluetooth-devices.c  device records and the (debounced) device list
 *                        reported to the UI
 *   bluetooth-connect.c  connect / disconnect / forget, and the pairing
 *                        agent they rely on
 *
 * Functions shared between those files are not static and carry a
 * bluetooth_ prefix, since they end up in the plugin's symbol table.
 */

#include "bluetooth.h"

#include <gio/gio.h>

#define BLUEZ_BUS_NAME        "org.bluez"
#define BLUEZ_ADAPTER_IFACE   "org.bluez.Adapter1"
#define BLUEZ_DEVICE_IFACE    "org.bluez.Device1"
#define BLUEZ_AGENT_MANAGER_IFACE "org.bluez.AgentManager1"
#define PROPERTIES_IFACE      "org.freedesktop.DBus.Properties"
#define OBJECT_MANAGER_IFACE  "org.freedesktop.DBus.ObjectManager"

typedef struct
{
    gchar *path;
    gchar *alias;       /* BlueZ's display name: the user-set alias, else the name, else the address */
    gboolean has_name;  /* the device itself announced a name (alias is more than a bare address) */
    gchar *icon;        /* may be NULL */
    gboolean paired;
    gboolean connected;
} DeviceInfo;

struct _ExtrasMenuBluetooth
{
    GDBusConnection *system_bus;

    /* Passed to every asynchronous D-Bus call and cancelled in
     * extras_menu_bluetooth_free(). A cancelled call still runs its
     * callback (with G_IO_ERROR_CANCELLED), by which time this struct
     * is gone, so every callback checks call_was_cancelled() before
     * touching it. */
    GCancellable *cancellable;

    /* Object path of the first adapter we found, e.g. "/org/bluez/hci0".
     * NULL until (and unless) one is found. */
    gchar *adapter_path;
    gboolean powered;

    /* One subscription for every PropertiesChanged BlueZ sends (adapter
     * and devices alike -- the handler sorts them out), and one for
     * ObjectManager's InterfacesAdded/Removed (devices and adapters
     * appearing/disappearing). */
    guint properties_changed_subscription_id;
    guint objects_changed_subscription_id;

    ExtrasMenuBluetoothChangedFunc callback;
    ExtrasMenuBluetoothDevicesChangedFunc devices_callback;
    gpointer user_data;

    GHashTable *devices;           /* object path (owned) -> DeviceInfo* (owned) */
    guint devices_emit_source_id;  /* pending debounced devices callback, or 0 */

    gboolean discovery_wanted;     /* the UI wants scanning on */
    gboolean discovery_active;     /* we have started a scan that hasn't been stopped */

    gchar *agent_path;             /* where our pairing agent is exported */
    guint agent_registration_id;   /* GDBus object registration, or 0 */
};

/* A cancelled GDBus call still runs its callback; see the struct comment. */
static inline gboolean
call_was_cancelled(const GError *error)
{
    return error != NULL && g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
}

/* TRUE if `error` is the BlueZ error with this D-Bus name. */
static inline gboolean
error_is(GError *error, const gchar *remote_name)
{
    gchar *remote = g_dbus_error_get_remote_error(error);
    gboolean matches = g_strcmp0(remote, remote_name) == 0;
    g_free(remote);
    return matches;
}

/* --- defined in bluetooth.c --- */

/* Reports adapter availability / Powered state to the UI callback. */
void
bluetooth_emit_state(ExtrasMenuBluetooth *bt, gboolean available, gboolean powered);

/* --- defined in bluetooth-adapter.c --- */

/* Subscribes to BlueZ's change signals and looks for the first adapter.
 * Called once, as soon as the system bus connection exists. */
void
bluetooth_start_tracking(ExtrasMenuBluetooth *bt);

/* Brings the real scanning state in line with what's wanted: scanning
 * runs only while the UI wants it AND the adapter is powered. */
void
bluetooth_apply_discovery(ExtrasMenuBluetooth *bt);

/* --- defined in bluetooth-devices.c --- */

void
bluetooth_device_info_free(gpointer data);

DeviceInfo *
bluetooth_device_info_new(const gchar *path, GVariant *properties);

/* Copies the properties we care about out of an "a{sv}" dictionary into
 * the record. Returns TRUE if anything we display actually changed. */
gboolean
bluetooth_device_apply_properties(DeviceInfo *device, GVariant *properties);

/* Asks for the device list to be reported (debounced). */
void
bluetooth_schedule_devices_emit(ExtrasMenuBluetooth *bt);

/* --- defined in bluetooth-connect.c --- */

void
bluetooth_agent_export(ExtrasMenuBluetooth *bt);

void
bluetooth_agent_register_with_bluez(ExtrasMenuBluetooth *bt);

void
bluetooth_agent_unexport(ExtrasMenuBluetooth *bt);

#endif /* EXTRAS_MENU_BLUETOOTH_PRIVATE_H */