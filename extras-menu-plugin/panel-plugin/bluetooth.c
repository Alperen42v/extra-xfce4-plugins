#include "bluetooth.h"

#include <gio/gio.h>

#define BLUEZ_BUS_NAME        "org.bluez"
#define BLUEZ_ADAPTER_IFACE   "org.bluez.Adapter1"
#define BLUEZ_DEVICE_IFACE    "org.bluez.Device1"
#define BLUEZ_AGENT_MANAGER_IFACE "org.bluez.AgentManager1"
#define PROPERTIES_IFACE      "org.freedesktop.DBus.Properties"
#define OBJECT_MANAGER_IFACE  "org.freedesktop.DBus.ObjectManager"

/* Pairing and connecting can legitimately take a while (the remote
 * device has to answer), longer than GDBus's default 25 s. */
#define SLOW_CALL_TIMEOUT_MS  60000

/* How long to wait after a device change before reporting the list, so
 * the burst of changes at the start of a scan arrives as one callback. */
#define DEVICES_EMIT_DELAY_MS 120

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

/* Number of pairing attempts we've started that haven't finished. The
 * agent below only approves requests while this is non-zero, so it can
 * never wave through something the user didn't ask for. Process-wide
 * rather than per instance: BlueZ allows one agent per D-Bus client, and
 * all instances in this process share one. */
static guint pairing_in_progress = 0;
static guint agent_instance_counter = 0;

/* A cancelled GDBus call still runs its callback; see the struct comment. */
static gboolean
call_was_cancelled(const GError *error)
{
    return error != NULL && g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
}

/* --- device records -------------------------------------------------------- */

static void
device_info_free(gpointer data)
{
    DeviceInfo *device = data;
    if (device == NULL)
        return;

    g_free(device->path);
    g_free(device->alias);
    g_free(device->icon);
    g_free(device);
}

static gboolean
replace_string(gchar **field, const gchar *value)
{
    if (g_strcmp0(*field, value) == 0)
        return FALSE;

    g_free(*field);
    *field = g_strdup(value);
    return TRUE;
}

/* Copies the properties we care about out of an "a{sv}" dictionary
 * (either a full property set or just the changed ones) into the
 * record. Returns TRUE if anything we display actually changed. */
static gboolean
device_apply_properties(DeviceInfo *device, GVariant *properties)
{
    gboolean changed = FALSE;
    const gchar *text = NULL;
    gboolean flag = FALSE;

    if (g_variant_lookup(properties, "Alias", "&s", &text))
        changed |= replace_string(&device->alias, text);

    /* "Name" is only present when the device announced one; Alias alone
     * falls back to the bare address otherwise. */
    if (g_variant_lookup(properties, "Name", "&s", &text) && !device->has_name)
    {
        device->has_name = TRUE;
        changed = TRUE;
    }

    if (g_variant_lookup(properties, "Icon", "&s", &text))
        changed |= replace_string(&device->icon, text);

    if (g_variant_lookup(properties, "Paired", "b", &flag) && device->paired != flag)
    {
        device->paired = flag;
        changed = TRUE;
    }

    if (g_variant_lookup(properties, "Connected", "b", &flag) && device->connected != flag)
    {
        device->connected = flag;
        changed = TRUE;
    }

    return changed;
}

static DeviceInfo *
device_info_new(const gchar *path, GVariant *properties)
{
    DeviceInfo *device = g_new0(DeviceInfo, 1);
    device->path = g_strdup(path);
    device->alias = g_strdup("");
    device_apply_properties(device, properties);
    return device;
}

/* --- reporting ---------------------------------------------------------------- */

static void
emit_state(ExtrasMenuBluetooth *bt, gboolean available, gboolean powered)
{
    if (bt->callback != NULL)
        bt->callback(available, powered, bt->user_data);
}

static gboolean
device_is_listed(const DeviceInfo *device)
{
    return device->paired || device->has_name;
}

static gint
compare_devices(gconstpointer a, gconstpointer b)
{
    const DeviceInfo *da = a;
    const DeviceInfo *db = b;

    if (da->connected != db->connected)
        return da->connected ? -1 : 1;
    if (da->paired != db->paired)
        return da->paired ? -1 : 1;

    return g_utf8_collate(da->alias, db->alias);
}

static void
emit_devices_now(ExtrasMenuBluetooth *bt)
{
    if (bt->devices_callback == NULL)
        return;

    GList *listed = NULL;
    GList *all = g_hash_table_get_values(bt->devices);
    for (GList *l = all; l != NULL; l = l->next)
    {
        if (device_is_listed(l->data))
            listed = g_list_prepend(listed, l->data);
    }
    g_list_free(all);
    listed = g_list_sort(listed, compare_devices);

    guint count = g_list_length(listed);
    ExtrasMenuBluetoothDevice *snapshot = g_new0(ExtrasMenuBluetoothDevice, count > 0 ? count : 1);

    guint i = 0;
    for (GList *l = listed; l != NULL; l = l->next, i++)
    {
        const DeviceInfo *device = l->data;
        snapshot[i].path = device->path;
        snapshot[i].name = device->alias[0] != '\0' ? device->alias : device->path;
        snapshot[i].icon = device->icon;
        snapshot[i].paired = device->paired;
        snapshot[i].connected = device->connected;
    }

    bt->devices_callback(snapshot, count, bt->user_data);

    g_free(snapshot);
    g_list_free(listed);
}

static gboolean
on_devices_emit_timeout(gpointer user_data)
{
    ExtrasMenuBluetooth *bt = user_data;

    bt->devices_emit_source_id = 0;
    emit_devices_now(bt);
    return G_SOURCE_REMOVE;
}

static void
schedule_devices_emit(ExtrasMenuBluetooth *bt)
{
    if (bt->devices_callback != NULL && bt->devices_emit_source_id == 0)
    {
        bt->devices_emit_source_id = g_timeout_add(DEVICES_EMIT_DELAY_MS,
                                                    on_devices_emit_timeout, bt);
    }
}

/* --- errors ---------------------------------------------------------------------- */

/* Turns a D-Bus error from BlueZ into a sentence for the user. */
static gchar *
describe_error(GError *error)
{
    gchar *remote = g_dbus_error_get_remote_error(error);
    gchar *result = NULL;

    if (g_strcmp0(remote, "org.bluez.Error.AuthenticationFailed") == 0 ||
        g_strcmp0(remote, "org.bluez.Error.AuthenticationRejected") == 0 ||
        g_strcmp0(remote, "org.bluez.Error.AuthenticationCanceled") == 0 ||
        g_strcmp0(remote, "org.bluez.Error.AuthenticationTimeout") == 0)
    {
        result = g_strdup("Pairing failed. Devices that ask for a PIN or passkey can't be "
                          "paired from here yet -- pair them once with bluetoothctl or blueman.");
    }
    else if (g_strcmp0(remote, "org.bluez.Error.NotReady") == 0)
    {
        result = g_strdup("Bluetooth isn't ready yet. Try again in a moment.");
    }
    else if (g_strcmp0(remote, "org.bluez.Error.InProgress") == 0)
    {
        result = g_strdup("Another Bluetooth operation is already in progress.");
    }
    else
    {
        g_dbus_error_strip_remote_error(error);
        result = g_strdup(error->message != NULL && error->message[0] != '\0'
                              ? error->message : "Unknown error");
    }

    g_free(remote);
    return result;
}

static gboolean
error_is(GError *error, const gchar *remote_name)
{
    gchar *remote = g_dbus_error_get_remote_error(error);
    gboolean matches = g_strcmp0(remote, remote_name) == 0;
    g_free(remote);
    return matches;
}

/* --- the pairing agent --------------------------------------------------------------
 *
 * BlueZ asks an "agent" to approve pairing steps. Ours declares the
 * NoInputNoOutput capability, which makes BlueZ use "Just Works" pairing
 * (no PIN, no number comparison) -- right for headphones, speakers,
 * mice. It is registered but NOT made the default agent, so it is only
 * ever used for pairings this process starts (BlueZ picks the agent
 * belonging to whoever called Pair()); other agents on the system, such
 * as blueman's, are untouched. */

static const gchar agent_introspection_xml[] =
    "<node>"
    "  <interface name='org.bluez.Agent1'>"
    "    <method name='Release'/>"
    "    <method name='RequestPinCode'>"
    "      <arg type='o' name='device' direction='in'/>"
    "      <arg type='s' name='pincode' direction='out'/>"
    "    </method>"
    "    <method name='DisplayPinCode'>"
    "      <arg type='o' name='device' direction='in'/>"
    "      <arg type='s' name='pincode' direction='in'/>"
    "    </method>"
    "    <method name='RequestPasskey'>"
    "      <arg type='o' name='device' direction='in'/>"
    "      <arg type='u' name='passkey' direction='out'/>"
    "    </method>"
    "    <method name='DisplayPasskey'>"
    "      <arg type='o' name='device' direction='in'/>"
    "      <arg type='u' name='passkey' direction='in'/>"
    "      <arg type='q' name='entered' direction='in'/>"
    "    </method>"
    "    <method name='RequestConfirmation'>"
    "      <arg type='o' name='device' direction='in'/>"
    "      <arg type='u' name='passkey' direction='in'/>"
    "    </method>"
    "    <method name='RequestAuthorization'>"
    "      <arg type='o' name='device' direction='in'/>"
    "    </method>"
    "    <method name='AuthorizeService'>"
    "      <arg type='o' name='device' direction='in'/>"
    "      <arg type='s' name='uuid' direction='in'/>"
    "    </method>"
    "    <method name='Cancel'/>"
    "  </interface>"
    "</node>";

static void
agent_method_call(GDBusConnection *connection, const gchar *sender,
                  const gchar *object_path, const gchar *interface_name,
                  const gchar *method_name, GVariant *parameters,
                  GDBusMethodInvocation *invocation, gpointer user_data)
{
    (void) connection;
    (void) sender;
    (void) object_path;
    (void) interface_name;
    (void) parameters;
    (void) user_data;

    if (g_strcmp0(method_name, "Release") == 0 ||
        g_strcmp0(method_name, "Cancel") == 0 ||
        g_strcmp0(method_name, "DisplayPinCode") == 0 ||
        g_strcmp0(method_name, "DisplayPasskey") == 0)
    {
        /* Nothing to do: we have no display to show a code on. */
        g_dbus_method_invocation_return_value(invocation, NULL);
    }
    else if (g_strcmp0(method_name, "RequestAuthorization") == 0 ||
             g_strcmp0(method_name, "AuthorizeService") == 0)
    {
        /* Approve only while the user has an attempt running. */
        if (pairing_in_progress > 0)
            g_dbus_method_invocation_return_value(invocation, NULL);
        else
            g_dbus_method_invocation_return_dbus_error(invocation, "org.bluez.Error.Rejected",
                                                        "Not expecting an authorization request");
    }
    else
    {
        /* RequestPinCode, RequestPasskey, RequestConfirmation: these
         * need the user to type or compare a code, which "Just Works"
         * pairing is specifically meant to avoid. Rather than guess a
         * PIN or auto-approve a comparison (which would defeat its
         * point), refuse -- the attempt then fails with a clear message. */
        g_dbus_method_invocation_return_dbus_error(invocation, "org.bluez.Error.Rejected",
                                                    "This device needs a PIN or passkey");
    }
}

static const GDBusInterfaceVTable agent_vtable = {
    agent_method_call, NULL, NULL, { NULL }
};

static GDBusNodeInfo *
agent_node_info(void)
{
    static GDBusNodeInfo *info = NULL;

    if (info == NULL)
        info = g_dbus_node_info_new_for_xml(agent_introspection_xml, NULL);

    return info;
}

static void
on_register_agent_finished(GObject *source, GAsyncResult *result, gpointer user_data)
{
    (void) user_data;
    GError *error = NULL;

    GVariant *reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), result, &error);
    if (reply != NULL)
        g_variant_unref(reply);

    /* Failing (no AgentManager, or "AlreadyExists" because another
     * instance in this process already registered one) just means
     * pairing may not work from here; nothing to tell the user yet, and
     * nothing here may touch the struct since it might be gone. */
    g_clear_error(&error);
}

/* Exports our agent object on the bus; called once, when we get the bus. */
static void
export_agent(ExtrasMenuBluetooth *bt)
{
    GDBusNodeInfo *info = agent_node_info();
    if (info == NULL)
        return;

    bt->agent_path = g_strdup_printf("/org/extras_menu/agent%u", ++agent_instance_counter);
    bt->agent_registration_id = g_dbus_connection_register_object(
        bt->system_bus, bt->agent_path, info->interfaces[0], &agent_vtable, NULL, NULL, NULL);

    if (bt->agent_registration_id == 0)
    {
        g_free(bt->agent_path);
        bt->agent_path = NULL;
    }
}

/* Tells BlueZ about the exported agent; called whenever an adapter is
 * adopted (BlueZ forgets agents when bluetoothd restarts). */
static void
register_agent_with_bluez(ExtrasMenuBluetooth *bt)
{
    if (bt->agent_path == NULL)
        return;

    g_dbus_connection_call(
        bt->system_bus, BLUEZ_BUS_NAME, "/org/bluez", BLUEZ_AGENT_MANAGER_IFACE,
        "RegisterAgent",
        g_variant_new("(os)", bt->agent_path, "NoInputNoOutput"),
        NULL, G_DBUS_CALL_FLAGS_NONE, -1, bt->cancellable,
        on_register_agent_finished, NULL);
}

/* --- reading the adapter's Powered property ----------------------------- */

static void apply_discovery(ExtrasMenuBluetooth *bt);

/* Records and reports a new Powered value. */
static void
set_powered_state(ExtrasMenuBluetooth *bt, gboolean powered)
{
    bt->powered = powered;

    /* BlueZ ends any scan when the adapter powers down. */
    if (!powered)
        bt->discovery_active = FALSE;

    emit_state(bt, TRUE, powered);
    apply_discovery(bt);
    schedule_devices_emit(bt);
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
     * apply_discovery() to try again. */
    if (!error_is(error, "org.bluez.Error.InProgress"))
        bt->discovery_active = FALSE;

    g_clear_error(&error);
}

/* Brings the real scanning state in line with what's wanted: scanning
 * runs only while the UI wants it AND the adapter is powered. */
static void
apply_discovery(ExtrasMenuBluetooth *bt)
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
        if (device != NULL && device_apply_properties(device, changed))
            schedule_devices_emit(bt);
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

    emit_state(bt, FALSE, FALSE);
    schedule_devices_emit(bt);
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
                g_hash_table_replace(bt->devices, g_strdup(path), device_info_new(path, device_props));
                schedule_devices_emit(bt);
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
                    schedule_devices_emit(bt);
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
        emit_state(bt, FALSE, FALSE);
        schedule_devices_emit(bt);
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
        emit_state(bt, FALSE, FALSE);
        schedule_devices_emit(bt);
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
                g_hash_table_replace(bt->devices, g_strdup(path), device_info_new(path, device_props));
            }
            g_variant_unref(device_props);
        }
        g_variant_unref(interfaces);
    }

    g_variant_unref(objects);
    g_variant_unref(reply);

    register_agent_with_bluez(bt);
    request_powered_state(bt);
    schedule_devices_emit(bt);
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
        emit_state(bt, FALSE, FALSE);
        return;
    }

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

    export_agent(bt);
    find_adapter(bt);
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
    bt->devices = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, device_info_free);

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
    apply_discovery(bt);
}

/* --- connect / disconnect / forget ----------------------------------------------- */

typedef struct
{
    ExtrasMenuBluetooth *bt;
    gchar *device_path;
    ExtrasMenuBluetoothResultFunc callback;
    gpointer user_data;
} OperationCtx;

static OperationCtx *
operation_ctx_new(ExtrasMenuBluetooth *bt, const gchar *device_path,
                  ExtrasMenuBluetoothResultFunc callback, gpointer user_data)
{
    OperationCtx *ctx = g_new0(OperationCtx, 1);
    ctx->bt = bt;
    ctx->device_path = g_strdup(device_path);
    ctx->callback = callback;
    ctx->user_data = user_data;
    return ctx;
}

static void
operation_ctx_free(OperationCtx *ctx)
{
    g_free(ctx->device_path);
    g_free(ctx);
}

/* A request can't even be started (no adapter, no bus): still answer
 * through the callback, so a caller waiting on it never waits forever. */
static void
report_unavailable(ExtrasMenuBluetoothResultFunc callback, gpointer user_data, const gchar *message)
{
    if (callback != NULL)
        callback(FALSE, message, user_data);
}

/* Reports an operation's outcome and releases it. `error` may be NULL
 * for success; it is consumed. */
static void
finish_operation(OperationCtx *ctx, GError *error)
{
    if (ctx->callback != NULL)
    {
        if (error == NULL)
        {
            ctx->callback(TRUE, NULL, ctx->user_data);
        }
        else
        {
            gchar *message = describe_error(error);
            ctx->callback(FALSE, message, ctx->user_data);
            g_free(message);
        }
    }

    g_clear_error(&error);
    operation_ctx_free(ctx);
}

/* Shared "(call finished)" handler for Connect, Disconnect and
 * RemoveDevice. `ignorable_error` is a BlueZ error name that counts as
 * success (e.g. already connected), or NULL. */
static void
finish_simple_call(GObject *source, GAsyncResult *result, OperationCtx *ctx,
                   const gchar *ignorable_error)
{
    GError *error = NULL;

    GVariant *reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), result, &error);
    if (reply == NULL && call_was_cancelled(error))
    {
        g_clear_error(&error);
        operation_ctx_free(ctx);
        return;
    }

    if (reply != NULL)
        g_variant_unref(reply);

    if (error != NULL && ignorable_error != NULL && error_is(error, ignorable_error))
        g_clear_error(&error);

    finish_operation(ctx, error);
}

static void
on_connect_finished(GObject *source, GAsyncResult *result, gpointer user_data)
{
    finish_simple_call(source, result, user_data, "org.bluez.Error.AlreadyConnected");
}

static void
on_disconnect_finished(GObject *source, GAsyncResult *result, gpointer user_data)
{
    finish_simple_call(source, result, user_data, "org.bluez.Error.NotConnected");
}

static void
on_remove_finished(GObject *source, GAsyncResult *result, gpointer user_data)
{
    finish_simple_call(source, result, user_data, "org.bluez.Error.DoesNotExist");
}

static void
start_connect(OperationCtx *ctx)
{
    ExtrasMenuBluetooth *bt = ctx->bt;

    g_dbus_connection_call(
        bt->system_bus, BLUEZ_BUS_NAME, ctx->device_path, BLUEZ_DEVICE_IFACE,
        "Connect", NULL, NULL, G_DBUS_CALL_FLAGS_NONE, SLOW_CALL_TIMEOUT_MS,
        bt->cancellable, on_connect_finished, ctx);
}

static void
on_pair_finished(GObject *source, GAsyncResult *result, gpointer user_data)
{
    OperationCtx *ctx = user_data;
    GError *error = NULL;

    if (pairing_in_progress > 0)
        pairing_in_progress--;

    GVariant *reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), result, &error);
    if (reply == NULL && call_was_cancelled(error))
    {
        g_clear_error(&error);
        operation_ctx_free(ctx);
        return;
    }

    if (reply != NULL)
        g_variant_unref(reply);

    /* Paired in the meantime (e.g. by another tool) is as good as success. */
    if (error != NULL && error_is(error, "org.bluez.Error.AlreadyExists"))
        g_clear_error(&error);

    if (error != NULL)
    {
        finish_operation(ctx, error);
        return;
    }

    /* Trust the device so it reconnects by itself from now on (this is
     * what "bluetoothctl trust" does). The reply doesn't matter, and
     * messages to BlueZ are handled in order, so Connect below sees it. */
    ExtrasMenuBluetooth *bt = ctx->bt;
    g_dbus_connection_call(
        bt->system_bus, BLUEZ_BUS_NAME, ctx->device_path, PROPERTIES_IFACE,
        "Set",
        g_variant_new("(ssv)", BLUEZ_DEVICE_IFACE, "Trusted", g_variant_new_boolean(TRUE)),
        NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL, NULL);

    start_connect(ctx);
}

void
extras_menu_bluetooth_connect_device(ExtrasMenuBluetooth *bt, const gchar *device_path,
                                     ExtrasMenuBluetoothResultFunc result_callback,
                                     gpointer result_user_data)
{
    if (bt == NULL || bt->system_bus == NULL || bt->adapter_path == NULL || device_path == NULL)
    {
        report_unavailable(result_callback, result_user_data, "Bluetooth isn't available.");
        return;
    }

    /* Only devices the adapter currently knows about. One that vanished
     * since the list was drawn (forgotten, or its adapter replugged)
     * should fail plainly, not go on to a confusing D-Bus error. */
    const DeviceInfo *device = g_hash_table_lookup(bt->devices, device_path);
    if (device == NULL)
    {
        report_unavailable(result_callback, result_user_data, "That device is no longer available.");
        return;
    }

    OperationCtx *ctx = operation_ctx_new(bt, device_path, result_callback, result_user_data);

    if (device->paired)
    {
        start_connect(ctx);
        return;
    }

    /* Not paired: pair first. The agent only approves while this
     * counter is non-zero (see agent_method_call()). */
    pairing_in_progress++;
    g_dbus_connection_call(
        bt->system_bus, BLUEZ_BUS_NAME, device_path, BLUEZ_DEVICE_IFACE,
        "Pair", NULL, NULL, G_DBUS_CALL_FLAGS_NONE, SLOW_CALL_TIMEOUT_MS,
        bt->cancellable, on_pair_finished, ctx);
}

void
extras_menu_bluetooth_disconnect_device(ExtrasMenuBluetooth *bt, const gchar *device_path,
                                        ExtrasMenuBluetoothResultFunc result_callback,
                                        gpointer result_user_data)
{
    if (bt == NULL || bt->system_bus == NULL || bt->adapter_path == NULL || device_path == NULL)
    {
        report_unavailable(result_callback, result_user_data, "Bluetooth isn't available.");
        return;
    }

    if (g_hash_table_lookup(bt->devices, device_path) == NULL)
    {
        report_unavailable(result_callback, result_user_data, "That device is no longer available.");
        return;
    }

    OperationCtx *ctx = operation_ctx_new(bt, device_path, result_callback, result_user_data);

    g_dbus_connection_call(
        bt->system_bus, BLUEZ_BUS_NAME, device_path, BLUEZ_DEVICE_IFACE,
        "Disconnect", NULL, NULL, G_DBUS_CALL_FLAGS_NONE, -1,
        bt->cancellable, on_disconnect_finished, ctx);
}

void
extras_menu_bluetooth_remove_device(ExtrasMenuBluetooth *bt, const gchar *device_path,
                                    ExtrasMenuBluetoothResultFunc result_callback,
                                    gpointer result_user_data)
{
    if (bt == NULL || bt->system_bus == NULL || bt->adapter_path == NULL || device_path == NULL)
    {
        report_unavailable(result_callback, result_user_data, "Bluetooth isn't available.");
        return;
    }

    OperationCtx *ctx = operation_ctx_new(bt, device_path, result_callback, result_user_data);

    g_dbus_connection_call(
        bt->system_bus, BLUEZ_BUS_NAME, bt->adapter_path, BLUEZ_ADAPTER_IFACE,
        "RemoveDevice", g_variant_new("(o)", device_path), NULL,
        G_DBUS_CALL_FLAGS_NONE, -1,
        bt->cancellable, on_remove_finished, ctx);
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

        if (bt->agent_registration_id != 0)
        {
            g_dbus_connection_call(
                bt->system_bus, BLUEZ_BUS_NAME, "/org/bluez", BLUEZ_AGENT_MANAGER_IFACE,
                "UnregisterAgent", g_variant_new("(o)", bt->agent_path), NULL,
                G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL, NULL);
            g_dbus_connection_unregister_object(bt->system_bus, bt->agent_registration_id);
        }

        g_object_unref(bt->system_bus);
    }

    g_hash_table_destroy(bt->devices);
    g_object_unref(bt->cancellable);
    g_free(bt->agent_path);
    g_free(bt->adapter_path);
    g_free(bt);
}