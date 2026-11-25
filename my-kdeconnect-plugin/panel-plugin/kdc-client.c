#include "kdc-client.h"

#include <string.h>
#include <glib/gstdio.h>

#define KDC_BUS_NAME      "org.kde.kdeconnect"
#define KDC_DAEMON_PATH   "/modules/kdeconnect"
#define KDC_DAEMON_IFACE  "org.kde.kdeconnect.daemon"
#define KDC_DEVICE_IFACE  "org.kde.kdeconnect.device"
#define KDC_BATTERY_IFACE "org.kde.kdeconnect.device.battery"
#define KDC_PROPS_IFACE   "org.freedesktop.DBus.Properties"
#define READ_TIMEOUT_MS   1000
#define CALL_TIMEOUT_MS   10000
#define RETRY_SECONDS     5
#define MAX_RETRIES       6

struct _KdcClient
{
  GDBusConnection *bus;
  GCancellable    *cancellable;
  guint            watch_id;
  guint            sub_id;
  guint            refresh_source;
  guint            poll_source;
  guint            retry_source;
  guint            retry_count;
  gboolean         daemon_running;
  GPtrArray       *devices;
  KdcChangedFunc   changed;
  KdcErrorFunc     error;
  gpointer         user_data;
};

/* ------------------------------------------------------------------ */
/* helpers                                                            */
/* ------------------------------------------------------------------ */

static void
kdc_device_free (KdcDevice *d)
{
  g_free (d->id);
  g_free (d->name);
  g_free (d->type);
  g_free (d);
}

static void
report_error (KdcClient *c, const gchar *message)
{
  g_warning ("xfce4-kdeconnect: %s", message);
  if (c->error != NULL)
    c->error (c, message, c->user_data);
}

static gchar *
cache_root (void)
{
  return g_build_filename (g_get_user_cache_dir (), "xfce4-kdeconnect-plugin", NULL);
}

/* Remove zip files left over from previous sessions (root/zip-XXXXXX/file.zip). */
static void
cleanup_stale_zips (void)
{
  gchar *root = cache_root ();
  GDir *dir = g_dir_open (root, 0, NULL);
  const gchar *sub;

  if (dir != NULL)
    {
      while ((sub = g_dir_read_name (dir)) != NULL)
        {
          gchar *subpath = g_build_filename (root, sub, NULL);
          GDir *sd = g_dir_open (subpath, 0, NULL);
          const gchar *f;

          if (sd != NULL)
            {
              while ((f = g_dir_read_name (sd)) != NULL)
                {
                  gchar *fp = g_build_filename (subpath, f, NULL);
                  g_remove (fp);
                  g_free (fp);
                }
              g_dir_close (sd);
            }
          g_rmdir (subpath);
          g_free (subpath);
        }
      g_dir_close (dir);
    }
  g_free (root);
}

/* ------------------------------------------------------------------ */
/* reading device state (short synchronous calls on the session bus)  */
/* ------------------------------------------------------------------ */

static GVariant *
get_property (KdcClient *c, const gchar *path, const gchar *iface, const gchar *prop)
{
  GVariant *ret, *inner = NULL;

  ret = g_dbus_connection_call_sync (c->bus, KDC_BUS_NAME, path, KDC_PROPS_IFACE, "Get",
                                     g_variant_new ("(ss)", iface, prop),
                                     G_VARIANT_TYPE ("(v)"),
                                     G_DBUS_CALL_FLAGS_NO_AUTO_START,
                                     READ_TIMEOUT_MS, c->cancellable, NULL);
  if (ret == NULL)
    return NULL;
  g_variant_get (ret, "(v)", &inner);
  g_variant_unref (ret);
  return inner;
}

/* Property first, then a plain method of the same name (older KDE Connect). */
static GVariant *
read_value (KdcClient *c, const gchar *path, const gchar *iface, const gchar *name)
{
  GVariant *v = get_property (c, path, iface, name);
  GVariant *ret;

  if (v != NULL)
    return v;

  ret = g_dbus_connection_call_sync (c->bus, KDC_BUS_NAME, path, iface, name, NULL, NULL,
                                     G_DBUS_CALL_FLAGS_NO_AUTO_START,
                                     READ_TIMEOUT_MS, c->cancellable, NULL);
  if (ret == NULL)
    return NULL;
  v = g_variant_get_child_value (ret, 0);
  g_variant_unref (ret);
  return v;
}

static gchar *
read_string (KdcClient *c, const gchar *path, const gchar *iface,
             const gchar *name, const gchar *fallback)
{
  GVariant *v = get_property (c, path, iface, name);
  gchar *s = NULL;

  if (v != NULL && g_variant_is_of_type (v, G_VARIANT_TYPE_STRING))
    s = g_variant_dup_string (v, NULL);
  if (v != NULL)
    g_variant_unref (v);
  return s != NULL ? s : g_strdup (fallback);
}

static void schedule_refresh (KdcClient *c);

static gboolean
retry_cb (gpointer data)
{
  KdcClient *c = data;
  c->retry_source = 0;
  schedule_refresh (c);
  return G_SOURCE_REMOVE;
}

/* Something was missing (daemon busy, battery not reported yet): look again soon. */
static void
schedule_retry (KdcClient *c)
{
  if (c->retry_source == 0 && c->retry_count < MAX_RETRIES)
    {
      c->retry_count++;
      c->retry_source = g_timeout_add_seconds (RETRY_SECONDS, retry_cb, c);
    }
}

/*
 * prev = what we knew about this device before (or NULL). A failed or empty
 * read must never wipe known values, otherwise a single hiccup of the daemon
 * turns the battery into "unknown" although nothing changed.
 */
static KdcDevice *
read_device (KdcClient *c, const gchar *id, const KdcDevice *prev)
{
  KdcDevice *d = g_new0 (KdcDevice, 1);
  gchar *path = g_strdup_printf ("/modules/kdeconnect/devices/%s", id);
  gchar *bpath = g_strdup_printf ("%s/battery", path);
  GVariant *v;
  gint charge = -1;

  d->id = g_strdup (id);
  d->name = read_string (c, path, KDC_DEVICE_IFACE, "name", prev != NULL ? prev->name : id);
  d->type = read_string (c, path, KDC_DEVICE_IFACE, "type", prev != NULL ? prev->type : "smartphone");
  d->battery = prev != NULL ? prev->battery : -1;
  d->charging = prev != NULL ? prev->charging : FALSE;

  v = read_value (c, bpath, KDC_BATTERY_IFACE, "charge");
  if (v != NULL)
    {
      if (g_variant_is_of_type (v, G_VARIANT_TYPE_INT32))
        charge = g_variant_get_int32 (v);
      g_variant_unref (v);
    }

  if (charge >= 0)
    {
      d->battery = charge;
      v = read_value (c, bpath, KDC_BATTERY_IFACE, "isCharging");
      if (v != NULL)
        {
          if (g_variant_is_of_type (v, G_VARIANT_TYPE_BOOLEAN))
            d->charging = g_variant_get_boolean (v);
          g_variant_unref (v);
        }
    }
  else
    g_debug ("xfce4-kdeconnect: battery of %s not readable right now, keeping %d", id, d->battery);

  g_free (path);
  g_free (bpath);
  return d;
}

static void
refresh_now (KdcClient *c)
{
  GPtrArray *devs = g_ptr_array_new_with_free_func ((GDestroyNotify) kdc_device_free);
  gboolean any_unknown = FALSE, new_device = FALSE;

  if (c->bus != NULL && c->daemon_running)
    {
      GError *err = NULL;
      /* devices(onlyReachable = true, onlyPaired = true) */
      GVariant *ret = g_dbus_connection_call_sync (c->bus, KDC_BUS_NAME, KDC_DAEMON_PATH,
                                                   KDC_DAEMON_IFACE, "devices",
                                                   g_variant_new ("(bb)", TRUE, TRUE),
                                                   G_VARIANT_TYPE ("(as)"),
                                                   G_DBUS_CALL_FLAGS_NO_AUTO_START,
                                                   READ_TIMEOUT_MS, c->cancellable, &err);
      if (ret == NULL)
        {
          /* Transient failure: keep showing what we have and try again shortly. */
          g_debug ("xfce4-kdeconnect: devices() failed: %s", err != NULL ? err->message : "?");
          g_clear_error (&err);
          g_ptr_array_unref (devs);
          schedule_retry (c);
          return;
        }
      else
        {
          GVariantIter *iter;
          const gchar *id;

          g_variant_get (ret, "(as)", &iter);
          while (g_variant_iter_next (iter, "&s", &id))
            {
              const KdcDevice *prev = kdc_client_find_device (c, id);
              KdcDevice *d;

              if (prev == NULL)
                new_device = TRUE;
              d = read_device (c, id, prev);
              if (d->battery < 0)
                any_unknown = TRUE;
              g_ptr_array_add (devs, d);
            }
          g_variant_iter_free (iter);
          g_variant_unref (ret);
        }
    }

  g_ptr_array_unref (c->devices);
  c->devices = devs;

  if (new_device)
    c->retry_count = 0;
  if (any_unknown)
    schedule_retry (c);
  else
    c->retry_count = 0;

  if (c->changed != NULL)
    c->changed (c, c->user_data);
}

static gboolean
refresh_cb (gpointer data)
{
  KdcClient *c = data;
  c->refresh_source = 0;
  refresh_now (c);
  return G_SOURCE_REMOVE;
}

static void
schedule_refresh (KdcClient *c)
{
  if (c->refresh_source == 0)
    c->refresh_source = g_timeout_add (300, refresh_cb, c);   /* debounce signal bursts */
}

static gboolean
poll_cb (gpointer data)
{
  schedule_refresh (data);
  return G_SOURCE_CONTINUE;
}

/* Battery signals carry the new values: apply them directly, no re-reading. */
static gboolean
apply_battery_signal (KdcClient *c, const gchar *path, const gchar *signal, GVariant *params)
{
  const gchar *prefix = "/modules/kdeconnect/devices/";
  KdcDevice *d;
  gchar *id, *slash;

  if (!g_str_has_prefix (path, prefix))
    return FALSE;

  id = g_strdup (path + strlen (prefix));
  slash = strchr (id, '/');
  if (slash != NULL)
    *slash = '\0';
  d = (KdcDevice *) kdc_client_find_device (c, id);
  g_free (id);
  if (d == NULL)
    return FALSE;

  if (g_strcmp0 (signal, "refreshed") == 0 && g_variant_is_of_type (params, G_VARIANT_TYPE ("(bi)")))
    {
      gboolean charging;
      gint charge;

      g_variant_get (params, "(bi)", &charging, &charge);
      if (charge >= 0)
        {
          d->battery = charge;
          d->charging = charging;
        }
      return TRUE;
    }
  if (g_strcmp0 (signal, "chargeChanged") == 0 && g_variant_is_of_type (params, G_VARIANT_TYPE ("(i)")))
    {
      gint charge;

      g_variant_get (params, "(i)", &charge);
      if (charge >= 0)
        d->battery = charge;
      return TRUE;
    }
  if (g_strcmp0 (signal, "stateChanged") == 0 && g_variant_is_of_type (params, G_VARIANT_TYPE ("(b)")))
    {
      g_variant_get (params, "(b)", &d->charging);
      return TRUE;
    }
  return FALSE;
}

static void
on_signal (GDBusConnection *conn, const gchar *sender, const gchar *path,
           const gchar *iface, const gchar *signal, GVariant *params, gpointer data)
{
  KdcClient *c = data;

  if (g_strcmp0 (iface, KDC_BATTERY_IFACE) == 0 &&
      apply_battery_signal (c, path, signal, params))
    {
      if (c->changed != NULL)
        c->changed (c, c->user_data);
      return;
    }

  if (g_str_has_prefix (iface, "org.kde.kdeconnect") ||
      g_strcmp0 (iface, KDC_PROPS_IFACE) == 0)
    schedule_refresh (c);
}

static void
on_name_appeared (GDBusConnection *conn, const gchar *name, const gchar *owner, gpointer data)
{
  KdcClient *c = data;
  c->daemon_running = TRUE;
  schedule_refresh (c);
}

static void
on_name_vanished (GDBusConnection *conn, const gchar *name, gpointer data)
{
  KdcClient *c = data;
  c->daemon_running = FALSE;
  schedule_refresh (c);
}

/* ------------------------------------------------------------------ */
/* public: lifetime and queries                                       */
/* ------------------------------------------------------------------ */

KdcClient *
kdc_client_new (KdcChangedFunc changed, KdcErrorFunc error, gpointer user_data)
{
  KdcClient *c = g_new0 (KdcClient, 1);
  GError *err = NULL;

  c->changed = changed;
  c->error = error;
  c->user_data = user_data;
  c->cancellable = g_cancellable_new ();
  c->devices = g_ptr_array_new_with_free_func ((GDestroyNotify) kdc_device_free);

  cleanup_stale_zips ();

  c->bus = g_bus_get_sync (G_BUS_TYPE_SESSION, NULL, &err);
  if (c->bus == NULL)
    {
      g_warning ("xfce4-kdeconnect: cannot connect to the session bus: %s", err->message);
      g_error_free (err);
      return c;
    }

  c->sub_id = g_dbus_connection_signal_subscribe (c->bus, KDC_BUS_NAME, NULL, NULL, NULL, NULL,
                                                  G_DBUS_SIGNAL_FLAGS_NONE, on_signal, c, NULL);
  c->watch_id = g_bus_watch_name_on_connection (c->bus, KDC_BUS_NAME,
                                                G_BUS_NAME_WATCHER_FLAGS_NONE,
                                                on_name_appeared, on_name_vanished, c, NULL);
  c->poll_source = g_timeout_add_seconds (120, poll_cb, c);   /* safety net */
  return c;
}

void
kdc_client_free (KdcClient *c)
{
  if (c == NULL)
    return;

  g_cancellable_cancel (c->cancellable);   /* pending async callbacks will see CANCELLED */
  if (c->refresh_source != 0) g_source_remove (c->refresh_source);
  if (c->poll_source != 0)    g_source_remove (c->poll_source);
  if (c->retry_source != 0)   g_source_remove (c->retry_source);
  if (c->watch_id != 0)       g_bus_unwatch_name (c->watch_id);
  if (c->sub_id != 0 && c->bus != NULL)
    g_dbus_connection_signal_unsubscribe (c->bus, c->sub_id);

  g_ptr_array_unref (c->devices);
  g_clear_object (&c->bus);
  g_object_unref (c->cancellable);
  g_free (c);
}

const GPtrArray *
kdc_client_get_devices (KdcClient *c)
{
  return c->devices;
}

const KdcDevice *
kdc_client_find_device (KdcClient *c, const gchar *id)
{
  for (guint i = 0; i < c->devices->len; i++)
    {
      const KdcDevice *d = g_ptr_array_index (c->devices, i);
      if (g_strcmp0 (d->id, id) == 0)
        return d;
    }
  return NULL;
}

gboolean
kdc_client_daemon_running (KdcClient *c)
{
  return c->daemon_running;
}

/* ------------------------------------------------------------------ */
/* actions                                                            */
/* ------------------------------------------------------------------ */

static void
call_done (GObject *src, GAsyncResult *res, gpointer data)
{
  GError *err = NULL;
  GVariant *ret = g_dbus_connection_call_finish (G_DBUS_CONNECTION (src), res, &err);

  if (ret != NULL)
    {
      g_variant_unref (ret);
      return;
    }

  /* On cancellation the client may already be freed: do not touch it. */
  if (!g_error_matches (err, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    {
      g_dbus_error_strip_remote_error (err);
      report_error ((KdcClient *) data, err->message);
    }
  g_error_free (err);
}

/* plugin = "ping", "share", "clipboard", "findmyphone" ... */
static void
device_call (KdcClient *c, const gchar *id, const gchar *plugin,
             const gchar *method, GVariant *params)
{
  gchar *path, *iface;

  if (c->bus == NULL)
    {
      if (params != NULL)
        g_variant_unref (g_variant_ref_sink (params));
      return;
    }

  path = g_strdup_printf ("/modules/kdeconnect/devices/%s/%s", id, plugin);
  iface = g_strdup_printf ("org.kde.kdeconnect.device.%s", plugin);
  g_dbus_connection_call (c->bus, KDC_BUS_NAME, path, iface, method, params, NULL,
                          G_DBUS_CALL_FLAGS_NONE, CALL_TIMEOUT_MS,
                          c->cancellable, call_done, c);
  g_free (path);
  g_free (iface);
}

void
kdc_client_send_clipboard (KdcClient *c, const gchar *id, const gchar *text)
{
  device_call (c, id, "clipboard", "sendClipboard", g_variant_new ("(s)", text));
}

void
kdc_client_send_ping (KdcClient *c, const gchar *id)
{
  device_call (c, id, "ping", "sendPing", NULL);
}

void
kdc_client_find_phone (KdcClient *c, const gchar *id)
{
  device_call (c, id, "findmyphone", "ring", NULL);
}

void
kdc_client_share_url (KdcClient *c, const gchar *id, const gchar *url)
{
  device_call (c, id, "share", "shareUrl", g_variant_new ("(s)", url));
}

static gboolean
looks_like_url (const gchar *text)
{
  if (strstr (text, "://") == NULL || g_uri_peek_scheme (text) == NULL)
    return FALSE;
  for (const gchar *p = text; *p != '\0'; p++)
    if (g_ascii_isspace (*p))
      return FALSE;
  return TRUE;
}

void
kdc_client_send_text (KdcClient *c, const gchar *id, const gchar *text)
{
  gchar *t = g_strstrip (g_strdup (text));

  if (*t != '\0')
    {
      if (looks_like_url (t))
        kdc_client_share_url (c, id, t);
      else
        device_call (c, id, "share", "shareText", g_variant_new ("(s)", t));
    }
  g_free (t);
}

/* ------------------------------------------------------------------ */
/* folders: KDE Connect cannot send directories, so zip them first    */
/* ------------------------------------------------------------------ */

typedef struct
{
  KdcClient   *client;
  gchar       *device_id;
  gchar       *zip_path;
  GSubprocess *proc;
} ZipJob;

static void
zip_job_free (ZipJob *j)
{
  g_free (j->device_id);
  g_free (j->zip_path);
  g_clear_object (&j->proc);
  g_free (j);
}

static void
zip_done (GObject *src, GAsyncResult *res, gpointer data)
{
  ZipJob *j = data;
  GError *err = NULL;

  if (!g_subprocess_wait_finish (G_SUBPROCESS (src), res, &err))
    {
      if (!g_error_matches (err, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        report_error (j->client, err->message);
      g_error_free (err);
    }
  else if (g_subprocess_get_successful (j->proc))
    {
      gchar *uri = g_filename_to_uri (j->zip_path, NULL, NULL);
      if (uri != NULL)
        kdc_client_share_url (j->client, j->device_id, uri);
      g_free (uri);
    }
  else
    report_error (j->client, "Could not compress the folder (bsdtar failed).");

  zip_job_free (j);
}

static void
share_folder (KdcClient *c, const gchar *id, const gchar *path)
{
  gchar *root = cache_root ();
  gchar *tmpl, *dir, *base, *parent, *zname, *zip;
  GError *err = NULL;
  GSubprocess *proc;

  g_mkdir_with_parents (root, 0700);
  tmpl = g_build_filename (root, "zip-XXXXXX", NULL);
  dir = g_mkdtemp (tmpl);
  if (dir == NULL)
    {
      report_error (c, "Could not create a temporary folder for compression.");
      g_free (tmpl);
      g_free (root);
      return;
    }

  base = g_path_get_basename (path);
  parent = g_path_get_dirname (path);
  zname = g_strconcat (base, ".zip", NULL);
  zip = g_build_filename (dir, zname, NULL);

  {
    const gchar *argv[] = { "bsdtar", "--format", "zip", "-cf", zip, "-C", parent, base, NULL };
    proc = g_subprocess_newv (argv, G_SUBPROCESS_FLAGS_STDOUT_SILENCE |
                                    G_SUBPROCESS_FLAGS_STDERR_SILENCE, &err);
  }

  if (proc == NULL)
    {
      gchar *msg = g_strdup_printf ("Cannot compress the folder (is bsdtar installed?): %s", err->message);
      report_error (c, msg);
      g_free (msg);
      g_error_free (err);
      g_free (zip);
    }
  else
    {
      ZipJob *j = g_new0 (ZipJob, 1);
      j->client = c;
      j->device_id = g_strdup (id);
      j->zip_path = zip;           /* ownership moves to the job */
      j->proc = proc;
      g_subprocess_wait_async (proc, c->cancellable, zip_done, j);
    }

  g_free (zname);
  g_free (parent);
  g_free (base);
  g_free (tmpl);
  g_free (root);
}

void
kdc_client_share_uris (KdcClient *c, const gchar *id, gchar **uris)
{
  for (; uris != NULL && *uris != NULL; uris++)
    {
      gchar *path = NULL;

      if (g_str_has_prefix (*uris, "file://"))
        path = g_filename_from_uri (*uris, NULL, NULL);

      if (path != NULL && g_file_test (path, G_FILE_TEST_IS_DIR))
        share_folder (c, id, path);
      else
        kdc_client_share_url (c, id, *uris);

      g_free (path);
    }
}