#include "share-window.h"

typedef struct
{
  KdcClient *client;
  gchar     *device_id;
  GPtrArray *uris;       /* owned strings */
  GtkWidget *list;
  GtkWidget *send_btn;
  GtkWidget *clear_btn;
} ShareWin;

static void
share_win_free (ShareWin *sw)
{
  g_free (sw->device_id);
  g_ptr_array_free (sw->uris, TRUE);
  g_free (sw);
}

static void
update_buttons (ShareWin *sw)
{
  gboolean has_items = sw->uris->len > 0;
  gtk_widget_set_sensitive (sw->send_btn, has_items);
  gtk_widget_set_sensitive (sw->clear_btn, has_items);
}

static gboolean
uri_known (ShareWin *sw, const gchar *uri)
{
  for (guint i = 0; i < sw->uris->len; i++)
    if (g_str_equal (g_ptr_array_index (sw->uris, i), uri))
      return TRUE;
  return FALSE;
}

static void
add_uri (ShareWin *sw, const gchar *uri)
{
  GtkWidget *row, *label;
  gchar *text;
  const gchar *icon = "text-html";   /* non-file links dragged from a browser */

  if (uri_known (sw, uri))
    return;
  g_ptr_array_add (sw->uris, g_strdup (uri));

  if (g_str_has_prefix (uri, "file://"))
    {
      GFile *f = g_file_new_for_uri (uri);
      text = g_file_get_parse_name (f);
      icon = g_file_query_file_type (f, G_FILE_QUERY_INFO_NONE, NULL) == G_FILE_TYPE_DIRECTORY
             ? "folder" : "text-x-generic";
      g_object_unref (f);
    }
  else
    text = g_strdup (uri);

  row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_container_set_border_width (GTK_CONTAINER (row), 6);
  gtk_box_pack_start (GTK_BOX (row),
                      gtk_image_new_from_icon_name (icon, GTK_ICON_SIZE_LARGE_TOOLBAR),
                      FALSE, FALSE, 0);

  label = gtk_label_new (text);
  gtk_label_set_xalign (GTK_LABEL (label), 0.0f);
  gtk_label_set_ellipsize (GTK_LABEL (label), PANGO_ELLIPSIZE_MIDDLE);
  gtk_widget_set_tooltip_text (label, text);
  gtk_box_pack_start (GTK_BOX (row), label, TRUE, TRUE, 0);

  gtk_list_box_insert (GTK_LIST_BOX (sw->list), row, -1);
  gtk_widget_show_all (row);
  g_free (text);
}

static void
on_drag_data_received (GtkWidget *widget, GdkDragContext *ctx, gint x, gint y,
                       GtkSelectionData *data, guint info, guint time, gpointer user_data)
{
  ShareWin *sw = user_data;
  gchar **uris = gtk_selection_data_get_uris (data);

  if (uris != NULL)
    {
      for (gchar **u = uris; *u != NULL; u++)
        add_uri (sw, *u);
      g_strfreev (uris);
    }
  update_buttons (sw);
  /* GTK_DEST_DEFAULT_ALL finishes the drag for us. */
}

static void
on_clear_clicked (GtkButton *button, ShareWin *sw)
{
  GList *children = gtk_container_get_children (GTK_CONTAINER (sw->list));

  for (GList *l = children; l != NULL; l = l->next)
    gtk_widget_destroy (GTK_WIDGET (l->data));
  g_list_free (children);

  g_ptr_array_set_size (sw->uris, 0);
  update_buttons (sw);
}

static void
on_send_clicked (GtkButton *button, ShareWin *sw)
{
  GtkWidget *window = gtk_widget_get_toplevel (GTK_WIDGET (button));

  g_ptr_array_add (sw->uris, NULL);   /* NULL-terminate for the client API */
  kdc_client_share_uris (sw->client, sw->device_id, (gchar **) sw->uris->pdata);
  gtk_widget_destroy (window);        /* sw is freed with the window */
}

GtkWidget *
kdc_share_window_new (KdcClient *client, const gchar *device_id, const gchar *device_name)
{
  ShareWin *sw = g_new0 (ShareWin, 1);
  GtkWidget *win, *vbox, *intro, *scrolled, *placeholder, *img, *hint, *buttons;
  gchar *title, *intro_text;

  sw->client = client;
  sw->device_id = g_strdup (device_id);
  sw->uris = g_ptr_array_new_with_free_func (g_free);

  win = gtk_window_new (GTK_WINDOW_TOPLEVEL);
  title = g_strdup_printf ("Send to %s", device_name);
  gtk_window_set_title (GTK_WINDOW (win), title);
  gtk_window_set_default_size (GTK_WINDOW (win), 440, 340);
  gtk_window_set_position (GTK_WINDOW (win), GTK_WIN_POS_CENTER);
  gtk_window_set_icon_name (GTK_WINDOW (win), "document-send");
  g_object_set_data_full (G_OBJECT (win), "share-win", sw, (GDestroyNotify) share_win_free);

  vbox = gtk_box_new (GTK_ORIENTATION_VERTICAL, 10);
  gtk_container_set_border_width (GTK_CONTAINER (vbox), 12);
  gtk_container_add (GTK_CONTAINER (win), vbox);

  intro_text = g_strdup_printf ("Drag the files or folders you want to send to %s into the box below, then press Send.",
                                device_name);
  intro = gtk_label_new (intro_text);
  gtk_label_set_line_wrap (GTK_LABEL (intro), TRUE);
  gtk_label_set_xalign (GTK_LABEL (intro), 0.0f);
  gtk_box_pack_start (GTK_BOX (vbox), intro, FALSE, FALSE, 0);

  /* Drop zone: a list with a friendly placeholder when empty. */
  sw->list = gtk_list_box_new ();
  gtk_list_box_set_selection_mode (GTK_LIST_BOX (sw->list), GTK_SELECTION_NONE);

  placeholder = gtk_box_new (GTK_ORIENTATION_VERTICAL, 8);
  gtk_widget_set_halign (placeholder, GTK_ALIGN_CENTER);
  gtk_widget_set_valign (placeholder, GTK_ALIGN_CENTER);
  img = gtk_image_new_from_icon_name ("document-send", GTK_ICON_SIZE_DIALOG);
  gtk_image_set_pixel_size (GTK_IMAGE (img), 64);
  hint = gtk_label_new ("Drag files or folders here");
  gtk_style_context_add_class (gtk_widget_get_style_context (hint), "dim-label");
  gtk_box_pack_start (GTK_BOX (placeholder), img, FALSE, FALSE, 0);
  gtk_box_pack_start (GTK_BOX (placeholder), hint, FALSE, FALSE, 0);
  gtk_widget_show_all (placeholder);
  gtk_list_box_set_placeholder (GTK_LIST_BOX (sw->list), placeholder);

  scrolled = gtk_scrolled_window_new (NULL, NULL);
  gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scrolled), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_shadow_type (GTK_SCROLLED_WINDOW (scrolled), GTK_SHADOW_IN);
  gtk_container_add (GTK_CONTAINER (scrolled), sw->list);
  gtk_widget_set_vexpand (scrolled, TRUE);
  gtk_box_pack_start (GTK_BOX (vbox), scrolled, TRUE, TRUE, 0);

  gtk_drag_dest_set (scrolled, GTK_DEST_DEFAULT_ALL, NULL, 0, GDK_ACTION_COPY);
  gtk_drag_dest_add_uri_targets (scrolled);
  g_signal_connect (scrolled, "drag-data-received", G_CALLBACK (on_drag_data_received), sw);

  /* Bottom row: Clear (left), Send (right). */
  buttons = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
  sw->clear_btn = gtk_button_new_with_mnemonic ("_Clear");
  sw->send_btn = gtk_button_new_with_mnemonic ("_Send");
  gtk_style_context_add_class (gtk_widget_get_style_context (sw->send_btn), "suggested-action");
  gtk_box_pack_start (GTK_BOX (buttons), sw->clear_btn, FALSE, FALSE, 0);
  gtk_box_pack_end (GTK_BOX (buttons), sw->send_btn, FALSE, FALSE, 0);
  gtk_box_pack_start (GTK_BOX (vbox), buttons, FALSE, FALSE, 0);

  g_signal_connect (sw->clear_btn, "clicked", G_CALLBACK (on_clear_clicked), sw);
  g_signal_connect (sw->send_btn, "clicked", G_CALLBACK (on_send_clicked), sw);
  update_buttons (sw);

  g_free (title);
  g_free (intro_text);
  return win;
}