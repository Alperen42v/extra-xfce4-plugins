/*
 * xfce4-kdeconnect-plugin: XFCE panel plugin for KDE Connect.
 *
 * One panel button per connected (paired + reachable) device. Clicking a
 * button opens a menu: send clipboard, send files/folders, send text/URL,
 * ping, find my phone. The button shows the device icon and battery level.
 */
#include <gtk/gtk.h>
#include <libxfce4panel/libxfce4panel.h>
#include <libxfce4util/libxfce4util.h>

#include "kdc-client.h"
#include "preferences.h"
#include "share-window.h"

typedef struct _Plugin Plugin;

typedef struct
{
  Plugin    *plugin;
  gchar     *id;
  gchar     *type;
  GtkWidget *button;
  GtkWidget *inner;
  GtkWidget *image;
  GtkWidget *label;
} DevButton;

struct _Plugin
{
  XfcePanelPlugin *xfce;
  KdcClient       *client;
  GtkWidget       *box;
  GtkWidget       *placeholder;
  GtkWidget       *placeholder_image;
  GHashTable      *buttons;    /* device id -> DevButton */
  GHashTable      *windows;    /* "share:<id>" / "text:<id>" -> GtkWidget */
  gint             icon_size;
  KdcPrefs        *prefs;
};

typedef struct
{
  Plugin *plugin;
  gchar  *id;
  gchar  *name;
} ItemCtx;

typedef struct
{
  Plugin    *plugin;
  gchar     *id;
  GtkWidget *entry;
} TextCtx;
/* icons*/

static const gchar *const ICONS_PHONE[]   = { "smartphone", "phone", "phone-symbolic", "kdeconnect", NULL };
static const gchar *const ICONS_TABLET[]  = { "tablet", "tablet-symbolic", "smartphone", "kdeconnect", NULL };
static const gchar *const ICONS_LAPTOP[]  = { "computer-laptop", "laptop", "computer", "kdeconnect", NULL };
static const gchar *const ICONS_DESKTOP[] = { "computer", "computer-symbolic", "kdeconnect", NULL };
static const gchar *const ICONS_TV[]      = { "video-display", "tv", "computer", "kdeconnect", NULL };

static GIcon *
icon_for_type (const gchar *type)
{
  const gchar *const *names = ICONS_PHONE;

  if (g_strcmp0 (type, "tablet") == 0)       names = ICONS_TABLET;
  else if (g_strcmp0 (type, "laptop") == 0)  names = ICONS_LAPTOP;
  else if (g_strcmp0 (type, "desktop") == 0) names = ICONS_DESKTOP;
  else if (g_strcmp0 (type, "tv") == 0)      names = ICONS_TV;

  return g_themed_icon_new_from_names ((gchar **) names, -1);
}

static void
set_icon (GtkWidget *image, GIcon *icon, gint size)
{
  gtk_image_set_from_gicon (GTK_IMAGE (image), icon, GTK_ICON_SIZE_BUTTON);
  gtk_image_set_pixel_size (GTK_IMAGE (image), size);
}

/* small helpers */

static void
show_message (GtkMessageType type, const gchar *text)
{
  GtkWidget *d = gtk_message_dialog_new (NULL, 0, type, GTK_BUTTONS_CLOSE, "%s", text);
  gtk_window_set_title (GTK_WINDOW (d), "My KDE Connect");
  g_signal_connect (d, "response", G_CALLBACK (gtk_widget_destroy), NULL);
  gtk_widget_show (d);
}

static void
on_window_destroyed (GtkWidget *w, Plugin *p)
{
  g_hash_table_remove (p->windows, g_object_get_data (G_OBJECT (w), "win-key"));
}

/* Track windows/dialogs so they can be re-presented and closed on exit. */
static void
register_window (Plugin *p, const gchar *key, GtkWidget *win)
{
  g_object_set_data_full (G_OBJECT (win), "win-key", g_strdup (key), g_free);
  g_hash_table_insert (p->windows, g_strdup (key), win);
  g_signal_connect (win, "destroy", G_CALLBACK (on_window_destroyed), p);
}

/* actions (shared by the menu items and the left-click shortcut)*/

static void
do_send_clipboard (Plugin *p, const gchar *id)
{
  GtkClipboard *cb = gtk_clipboard_get (GDK_SELECTION_CLIPBOARD);
  gchar *text = gtk_clipboard_wait_for_text (cb);

  if (text == NULL || *text == '\0')
    show_message (GTK_MESSAGE_INFO, "The clipboard is empty.");
  else
    kdc_client_send_clipboard (p->client, id, text);
  g_free (text);
}

static void
do_open_share_window (Plugin *p, const gchar *id, const gchar *name)
{
  gchar *key = g_strconcat ("share:", id, NULL);
  GtkWidget *win = g_hash_table_lookup (p->windows, key);

  if (win == NULL)
    {
      win = kdc_share_window_new (p->client, id, name);
      register_window (p, key, win);
      gtk_widget_show_all (win);
    }
  gtk_window_present (GTK_WINDOW (win));
  g_free (key);
}

static void
free_text_ctx (TextCtx *tc)
{
  g_free (tc->id);
  g_free (tc);
}

static void
on_text_response (GtkDialog *dlg, gint response, TextCtx *tc)
{
  if (response == GTK_RESPONSE_OK)
    kdc_client_send_text (tc->plugin->client, tc->id,
                          gtk_entry_get_text (GTK_ENTRY (tc->entry)));
  gtk_widget_destroy (GTK_WIDGET (dlg));
}

static void
do_open_text_dialog (Plugin *p, const gchar *id, const gchar *name)
{
  gchar *key = g_strconcat ("text:", id, NULL);
  GtkWidget *dlg = g_hash_table_lookup (p->windows, key);

  if (dlg == NULL)
    {
      gchar *title = g_strdup_printf ("Send Text or URL to %s", name);
      GtkWidget *area, *label;
      TextCtx *tc = g_new0 (TextCtx, 1);

      dlg = gtk_dialog_new_with_buttons (title, NULL, 0,
                                         "_Cancel", GTK_RESPONSE_CANCEL,
                                         "_Send", GTK_RESPONSE_OK, NULL);
      g_free (title);

      tc->plugin = p;
      tc->id = g_strdup (id);
      tc->entry = gtk_entry_new ();
      gtk_entry_set_width_chars (GTK_ENTRY (tc->entry), 44);
      gtk_entry_set_activates_default (GTK_ENTRY (tc->entry), TRUE);
      gtk_dialog_set_default_response (GTK_DIALOG (dlg), GTK_RESPONSE_OK);

      label = gtk_label_new ("Text or link to send:");
      gtk_label_set_xalign (GTK_LABEL (label), 0.0f);

      area = gtk_dialog_get_content_area (GTK_DIALOG (dlg));
      gtk_container_set_border_width (GTK_CONTAINER (area), 12);
      gtk_box_set_spacing (GTK_BOX (area), 8);
      gtk_box_pack_start (GTK_BOX (area), label, FALSE, FALSE, 0);
      gtk_box_pack_start (GTK_BOX (area), tc->entry, FALSE, FALSE, 0);

      g_object_set_data_full (G_OBJECT (dlg), "text-ctx", tc, (GDestroyNotify) free_text_ctx);
      g_signal_connect (dlg, "response", G_CALLBACK (on_text_response), tc);
      register_window (p, key, dlg);
      gtk_widget_show_all (dlg);
    }
  gtk_window_present (GTK_WINDOW (dlg));
  g_free (key);
}

static void
run_action (Plugin *p, KdcAction action, const gchar *id_in, const gchar *name_in)
{
  /* Work on copies: the clipboard call spins a nested main loop, and during
   * that the device list and the popup menu may be replaced and freed. */
  gchar *id = g_strdup (id_in);
  gchar *name = g_strdup (name_in);

  switch (action)
    {
    case KDC_ACTION_CLIPBOARD: do_send_clipboard (p, id); break;
    case KDC_ACTION_FILES:     do_open_share_window (p, id, name); break;
    case KDC_ACTION_TEXT:      do_open_text_dialog (p, id, name); break;
    case KDC_ACTION_PING:      kdc_client_send_ping (p->client, id); break;
    case KDC_ACTION_FIND:      kdc_client_find_phone (p->client, id); break;
    default: break;
    }

  g_free (id);
  g_free (name);
}

/* popup menu */

static void
free_item_ctx (ItemCtx *ctx)
{
  g_free (ctx->id);
  g_free (ctx->name);
  g_free (ctx);
}

static gboolean
destroy_menu_idle (gpointer menu)
{
  gtk_widget_destroy (GTK_WIDGET (menu));
  g_object_unref (menu);
  return G_SOURCE_REMOVE;
}

/* Destroy later, otherwise the item's "activate" would never fire. */
static void
on_menu_deactivate (GtkMenuShell *menu, gpointer data)
{
  g_idle_add (destroy_menu_idle, menu);
}

static void
add_info_item (GtkWidget *menu, const gchar *label)
{
  GtkWidget *mi = gtk_menu_item_new_with_label (label);

  gtk_widget_set_sensitive (mi, FALSE);
  gtk_menu_shell_append (GTK_MENU_SHELL (menu), mi);
}

static void
on_menu_action (GtkMenuItem *item, ItemCtx *ctx)
{
  KdcAction action = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (item), "action"));

  run_action (ctx->plugin, action, ctx->id, ctx->name);
}

static void
add_action_item (GtkWidget *menu, KdcAction action, ItemCtx *ctx)
{
  GtkWidget *mi = gtk_menu_item_new_with_label (kdc_action_label (action));

  g_object_set_data (G_OBJECT (mi), "action", GINT_TO_POINTER (action));
  g_signal_connect (mi, "activate", G_CALLBACK (on_menu_action), ctx);
  gtk_menu_shell_append (GTK_MENU_SHELL (menu), mi);
}

static void
show_menu (Plugin *p, GtkWidget *anchor, const gchar *id)
{
  GtkWidget *menu = gtk_menu_new ();
  const KdcDevice *dev = id != NULL ? kdc_client_find_device (p->client, id) : NULL;
  GdkGravity widget_anchor, menu_anchor;

  g_object_ref_sink (menu);

  if (dev == NULL)
    {
      add_info_item (menu, kdc_client_daemon_running (p->client)
                             ? "No connected device"
                             : "KDE Connect is not running");
    }
  else
    {
      ItemCtx *ctx = g_new0 (ItemCtx, 1);
      gboolean have_items = FALSE;
      gchar *battery;

      ctx->plugin = p;
      ctx->id = g_strdup (dev->id);
      ctx->name = g_strdup (dev->name);
      g_object_set_data_full (G_OBJECT (menu), "ctx", ctx, (GDestroyNotify) free_item_ctx);

      if (dev->battery >= 0)
        battery = g_strdup_printf ("Battery: %d%%%s", dev->battery, dev->charging ? " (charging)" : "");
      else
        battery = g_strdup ("Battery: unknown");

      add_info_item (menu, dev->name);
      add_info_item (menu, battery);
      gtk_menu_shell_append (GTK_MENU_SHELL (menu), gtk_separator_menu_item_new ());
      g_free (battery);

      /* only the items enabled in the preferences */
      for (gint a = 0; a < KDC_N_ACTIONS; a++)
        {
          if (!kdc_prefs_get_action_enabled (p->prefs, a))
            continue;
          if (a == KDC_ACTION_FIND && have_items)
            gtk_menu_shell_append (GTK_MENU_SHELL (menu), gtk_separator_menu_item_new ());
          add_action_item (menu, a, ctx);
          have_items = TRUE;
        }
      if (!have_items)
        add_info_item (menu, "No items enabled (see Properties)");
    }

  /* GTK flips the menu automatically when there is no room (bottom/right panels). */
  if (xfce_panel_plugin_get_orientation (p->xfce) == GTK_ORIENTATION_HORIZONTAL)
    {
      widget_anchor = GDK_GRAVITY_SOUTH_WEST;
      menu_anchor = GDK_GRAVITY_NORTH_WEST;
    }
  else
    {
      widget_anchor = GDK_GRAVITY_NORTH_EAST;
      menu_anchor = GDK_GRAVITY_NORTH_WEST;
    }

  gtk_menu_attach_to_widget (GTK_MENU (menu), anchor, NULL);
  xfce_panel_plugin_register_menu (p->xfce, GTK_MENU (menu));
  g_signal_connect (menu, "deactivate", G_CALLBACK (on_menu_deactivate), NULL);
  gtk_widget_show_all (menu);
  gtk_menu_popup_at_widget (GTK_MENU (menu), anchor, widget_anchor, menu_anchor, NULL);
}

/* device buttons*/


/* Left click: the chosen shortcut action, or the menu. */
static void
on_device_clicked (GtkButton *button, DevButton *db)
{
  Plugin *p = db->plugin;
  gint action = kdc_prefs_get_click_action (p->prefs);
  const KdcDevice *dev = kdc_client_find_device (p->client, db->id);

  if (action == KDC_CLICK_MENU || dev == NULL)
    show_menu (p, db->button, db->id);
  else
    run_action (p, action, dev->id, dev->name);
}

/* Middle click always opens the menu (right click belongs to the panel). */
static gboolean
on_device_button_press (GtkWidget *widget, GdkEventButton *event, DevButton *db)
{
  if (event->button != 2)
    return FALSE;

  show_menu (db->plugin, db->button, db->id);
  return TRUE;
}

static void
on_placeholder_clicked (GtkButton *button, Plugin *p)
{
  show_menu (p, p->placeholder, NULL);
}

static DevButton *
dev_button_new (Plugin *p, const KdcDevice *dev)
{
  DevButton *db = g_new0 (DevButton, 1);
  GtkOrientation o = xfce_panel_plugin_get_orientation (p->xfce);

  db->plugin = p;
  db->id = g_strdup (dev->id);

  db->button = gtk_button_new ();
  gtk_button_set_relief (GTK_BUTTON (db->button), GTK_RELIEF_NONE);
  gtk_widget_set_focus_on_click (db->button, FALSE);

  db->inner = gtk_box_new (o, 4);
  db->image = gtk_image_new ();
  db->label = gtk_label_new (NULL);
  gtk_widget_set_no_show_all (db->label, TRUE);   /* shown only when battery is known */
  gtk_box_pack_start (GTK_BOX (db->inner), db->image, FALSE, FALSE, 0);
  gtk_box_pack_start (GTK_BOX (db->inner), db->label, FALSE, FALSE, 0);
  gtk_container_add (GTK_CONTAINER (db->button), db->inner);

  gtk_box_pack_start (GTK_BOX (p->box), db->button, FALSE, FALSE, 0);
  xfce_panel_plugin_add_action_widget (p->xfce, db->button);
  g_signal_connect (db->button, "clicked", G_CALLBACK (on_device_clicked), db);
  g_signal_connect (db->button, "button-press-event", G_CALLBACK (on_device_button_press), db);
  gtk_widget_show_all (db->button);
  return db;
}

static void
dev_button_free (DevButton *db)
{
  gtk_widget_destroy (db->button);
  g_free (db->id);
  g_free (db->type);
  g_free (db);
}

static void
dev_button_update (DevButton *db, const KdcDevice *dev)
{
  Plugin *p = db->plugin;
  gint threshold = kdc_prefs_get_low_threshold (p->prefs);
  gboolean known = dev->battery >= 0;
  gboolean low = known && !dev->charging && threshold > 0 && dev->battery < threshold;
  GString *markup = g_string_new (NULL);
  gchar *tip;

  if (g_strcmp0 (db->type, dev->type) != 0)
    {
      GIcon *icon = icon_for_type (dev->type);
      g_free (db->type);
      db->type = g_strdup (dev->type);
      set_icon (db->image, icon, p->icon_size);
      g_object_unref (icon);
    }

  /* label: [device name] [battery%] -- either part can be turned off */
  if (kdc_prefs_get_show_name (p->prefs))
    {
      gchar *escaped = g_markup_escape_text (dev->name, -1);
      g_string_append (markup, escaped);
      g_free (escaped);
    }

  if (known && kdc_prefs_get_show_battery (p->prefs))
    {
      gchar *pct = g_strdup_printf ("%d%%%s", dev->battery,
                                    dev->charging && kdc_prefs_get_show_charging_icon (p->prefs) ? "⚡" : "");

      if (markup->len > 0)
        g_string_append_c (markup, ' ');
      if (low)
        g_string_append_printf (markup, "<span foreground=\"#e01b24\" weight=\"bold\">%s</span>", pct);
      else
        g_string_append (markup, pct);
      g_free (pct);
    }

  if (known)
    tip = g_strdup_printf ("%s\nBattery: %d%%%s", dev->name, dev->battery,
                           dev->charging ? " (charging)" : (low ? " (low)" : ""));
  else
    tip = g_strdup (dev->name);

  gtk_label_set_markup (GTK_LABEL (db->label), markup->str);
  gtk_widget_set_visible (db->label, markup->len > 0);
  gtk_widget_set_tooltip_text (db->button, tip);
  g_string_free (markup, TRUE);
  g_free (tip);
}

static void
sync_buttons (Plugin *p)
{
  const GPtrArray *devs = kdc_client_get_devices (p->client);
  GHashTableIter iter;
  gpointer key, value;

  /* drop buttons of devices that went away */
  g_hash_table_iter_init (&iter, p->buttons);
  while (g_hash_table_iter_next (&iter, &key, &value))
    if (kdc_client_find_device (p->client, key) == NULL)
      g_hash_table_iter_remove (&iter);

  /* add/update the rest */
  for (guint i = 0; i < devs->len; i++)
    {
      const KdcDevice *dev = g_ptr_array_index (devs, i);
      DevButton *db = g_hash_table_lookup (p->buttons, dev->id);

      if (db == NULL)
        {
          db = dev_button_new (p, dev);
          g_hash_table_insert (p->buttons, db->id, db);
        }
      dev_button_update (db, dev);
    }

  /* placeholder keeps the plugin visible/clickable when nothing is connected */
  gtk_widget_set_tooltip_text (p->placeholder,
                               kdc_client_daemon_running (p->client)
                                 ? "No KDE Connect device connected"
                                 : "KDE Connect is not running");
  gtk_widget_set_visible (p->placeholder, g_hash_table_size (p->buttons) == 0);
}

static void
on_client_changed (KdcClient *client, gpointer data)
{
  Plugin *p = data;
  if (p->client != NULL)
    sync_buttons (p);
}

static void
on_client_error (KdcClient *client, const gchar *message, gpointer data)
{
  show_message (GTK_MESSAGE_ERROR, message);
}

static void
on_prefs_changed (gpointer data)
{
  Plugin *p = data;
  if (p->client != NULL)
    sync_buttons (p);
}

/* panel integration */

static void
apply_sizes (Plugin *p)
{
  GHashTableIter iter;
  gpointer value;

  gtk_image_set_pixel_size (GTK_IMAGE (p->placeholder_image), p->icon_size);
  g_hash_table_iter_init (&iter, p->buttons);
  while (g_hash_table_iter_next (&iter, NULL, &value))
    gtk_image_set_pixel_size (GTK_IMAGE (((DevButton *) value)->image), p->icon_size);
}

static gboolean
on_size_changed (XfcePanelPlugin *xpp, gint size, Plugin *p)
{
  if (xfce_panel_plugin_get_orientation (xpp) == GTK_ORIENTATION_HORIZONTAL)
    gtk_widget_set_size_request (GTK_WIDGET (xpp), -1, size);
  else
    gtk_widget_set_size_request (GTK_WIDGET (xpp), size, -1);

  p->icon_size = xfce_panel_plugin_get_icon_size (xpp);
  apply_sizes (p);
  return TRUE;
}

static void
on_orientation_changed (XfcePanelPlugin *xpp, GtkOrientation o, Plugin *p)
{
  GHashTableIter iter;
  gpointer value;

  gtk_orientable_set_orientation (GTK_ORIENTABLE (p->box), o);
  g_hash_table_iter_init (&iter, p->buttons);
  while (g_hash_table_iter_next (&iter, NULL, &value))
    gtk_orientable_set_orientation (GTK_ORIENTABLE (((DevButton *) value)->inner), o);

  on_size_changed (xpp, xfce_panel_plugin_get_size (xpp), p);
}

static void
plugin_free (XfcePanelPlugin *xpp, Plugin *p)
{
  GList *wins = g_hash_table_get_values (p->windows);

  for (GList *l = wins; l != NULL; l = l->next)
    gtk_widget_destroy (GTK_WIDGET (l->data));   /* removes itself from the table */
  g_list_free (wins);
  g_hash_table_destroy (p->windows);

  kdc_client_free (p->client);
  p->client = NULL;
  kdc_prefs_free (p->prefs);
  p->prefs = NULL;
  g_hash_table_destroy (p->buttons);
  g_free (p);
}

static void
plugin_construct (XfcePanelPlugin *xpp)
{
  Plugin *p = g_new0 (Plugin, 1);
  GIcon *icon;

  p->xfce = xpp;
  p->buttons = g_hash_table_new_full (g_str_hash, g_str_equal, NULL, (GDestroyNotify) dev_button_free);
  p->windows = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  p->icon_size = xfce_panel_plugin_get_icon_size (xpp);

  p->prefs = kdc_prefs_new (xpp, on_prefs_changed, p);

  p->box = gtk_box_new (xfce_panel_plugin_get_orientation (xpp), 2);
  gtk_container_add (GTK_CONTAINER (xpp), p->box);

  p->placeholder = gtk_button_new ();
  gtk_button_set_relief (GTK_BUTTON (p->placeholder), GTK_RELIEF_NONE);
  gtk_widget_set_focus_on_click (p->placeholder, FALSE);
  gtk_widget_set_opacity (p->placeholder, 0.5);
  p->placeholder_image = gtk_image_new ();
  icon = icon_for_type ("smartphone");
  set_icon (p->placeholder_image, icon, p->icon_size);
  g_object_unref (icon);
  gtk_container_add (GTK_CONTAINER (p->placeholder), p->placeholder_image);
  gtk_box_pack_start (GTK_BOX (p->box), p->placeholder, FALSE, FALSE, 0);
  xfce_panel_plugin_add_action_widget (xpp, p->placeholder);
  g_signal_connect (p->placeholder, "clicked", G_CALLBACK (on_placeholder_clicked), p);

  gtk_widget_show_all (p->box);

  g_signal_connect (xpp, "free-data", G_CALLBACK (plugin_free), p);
  g_signal_connect (xpp, "size-changed", G_CALLBACK (on_size_changed), p);
  g_signal_connect (xpp, "orientation-changed", G_CALLBACK (on_orientation_changed), p);

  p->client = kdc_client_new (on_client_changed, on_client_error, p);
  on_size_changed (xpp, xfce_panel_plugin_get_size (xpp), p);
  sync_buttons (p);
}

XFCE_PANEL_PLUGIN_REGISTER (plugin_construct);