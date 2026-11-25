#ifndef SHARE_WINDOW_H
#define SHARE_WINDOW_H

#include <gtk/gtk.h>
#include "kdc-client.h"

G_BEGIN_DECLS

/* Drop window: "Drag files or folders here" + Send. Destroys itself after sending. */
GtkWidget *kdc_share_window_new (KdcClient *client,
                                 const gchar *device_id,
                                 const gchar *device_name);

G_END_DECLS

#endif
