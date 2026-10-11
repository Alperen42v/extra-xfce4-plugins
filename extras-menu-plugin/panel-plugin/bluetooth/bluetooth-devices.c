/*
 * Bluetooth backend: the device records (what we remember about each
 * device BlueZ knows) and the device list reported to the UI --
 * filtered, sorted, and debounced so a burst of changes becomes one
 * callback.
 */

#include "bluetooth-private.h"

/* How long to wait after a device change before reporting the list, so
 * the burst of changes at the start of a scan arrives as one callback. */
#define DEVICES_EMIT_DELAY_MS 120

/* --- device records -------------------------------------------------------- */

void
bluetooth_device_info_free(gpointer data)
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
gboolean
bluetooth_device_apply_properties(DeviceInfo *device, GVariant *properties)
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

DeviceInfo *
bluetooth_device_info_new(const gchar *path, GVariant *properties)
{
    DeviceInfo *device = g_new0(DeviceInfo, 1);
    device->path = g_strdup(path);
    device->alias = g_strdup("");
    bluetooth_device_apply_properties(device, properties);
    return device;
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

void
bluetooth_schedule_devices_emit(ExtrasMenuBluetooth *bt)
{
    if (bt->devices_callback != NULL && bt->devices_emit_source_id == 0)
    {
        bt->devices_emit_source_id = g_timeout_add(DEVICES_EMIT_DELAY_MS,
                                                    on_devices_emit_timeout, bt);
    }
}