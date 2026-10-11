/*
 * Bluetooth backend: connecting to, disconnecting from and forgetting a
 * device, plus the pairing agent that connecting to an unpaired device
 * needs.
 */

#include "bluetooth-private.h"

/* Pairing and connecting can legitimately take a while (the remote
 * device has to answer), longer than GDBus's default 25 s. */
#define SLOW_CALL_TIMEOUT_MS  60000

/* Number of pairing attempts we've started that haven't finished. The
 * agent below only approves requests while this is non-zero, so it can
 * never wave through something the user didn't ask for. Process-wide
 * rather than per instance: BlueZ allows one agent per D-Bus client, and
 * all instances in this process share one. */
static guint pairing_in_progress = 0;
static guint agent_instance_counter = 0;

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
void
bluetooth_agent_export(ExtrasMenuBluetooth *bt)
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
void
bluetooth_agent_register_with_bluez(ExtrasMenuBluetooth *bt)
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

/* Takes our agent off BlueZ and the bus again; called from
 * extras_menu_bluetooth_free() while the bus connection still exists. */
void
bluetooth_agent_unexport(ExtrasMenuBluetooth *bt)
{
    if (bt->agent_registration_id == 0)
        return;

    g_dbus_connection_call(
        bt->system_bus, BLUEZ_BUS_NAME, "/org/bluez", BLUEZ_AGENT_MANAGER_IFACE,
        "UnregisterAgent", g_variant_new("(o)", bt->agent_path), NULL,
        G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL, NULL);
    g_dbus_connection_unregister_object(bt->system_bus, bt->agent_registration_id);
    bt->agent_registration_id = 0;
}