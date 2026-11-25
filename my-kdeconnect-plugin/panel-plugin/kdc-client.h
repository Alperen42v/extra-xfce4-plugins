/* D-Bus client for the KDE Connect daemon (org.kde.kdeconnect). */
#ifndef KDC_CLIENT_H
#define KDC_CLIENT_H

#include <gio/gio.h>

G_BEGIN_DECLS

typedef struct
{
  gchar   *id;
  gchar   *name;
  gchar   *type;      /* "smartphone", "tablet", "laptop", "desktop", "tv" */
  gint     battery;   /* 0-100, or -1 when unknown */
  gboolean charging;
} KdcDevice;

typedef struct _KdcClient KdcClient;

typedef void (*KdcChangedFunc) (KdcClient *client, gpointer user_data);
typedef void (*KdcErrorFunc)   (KdcClient *client, const gchar *message, gpointer user_data);

KdcClient       *kdc_client_new               (KdcChangedFunc changed,
                                               KdcErrorFunc   error,
                                               gpointer       user_data);
void             kdc_client_free              (KdcClient *client);

/* Paired AND reachable devices. Valid until the next "changed" callback. */
const GPtrArray *kdc_client_get_devices       (KdcClient *client);
const KdcDevice *kdc_client_find_device       (KdcClient *client, const gchar *id);
gboolean         kdc_client_daemon_running    (KdcClient *client);

void kdc_client_send_clipboard (KdcClient *client, const gchar *id, const gchar *text);
void kdc_client_send_ping      (KdcClient *client, const gchar *id);
void kdc_client_find_phone     (KdcClient *client, const gchar *id);
void kdc_client_send_text      (KdcClient *client, const gchar *id, const gchar *text); /* URL auto-detected */
void kdc_client_share_url      (KdcClient *client, const gchar *id, const gchar *url);
/* NULL-terminated URI list; folders are zipped first (needs bsdtar). */
void kdc_client_share_uris     (KdcClient *client, const gchar *id, gchar **uris);

G_END_DECLS

#endif