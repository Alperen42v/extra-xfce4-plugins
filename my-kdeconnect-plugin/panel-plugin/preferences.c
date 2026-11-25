#include "preferences.h"

#include <stdlib.h>
#include <xfconf/xfconf.h>

#define PROJECT_URL  "https://github.com/Alperen42v/extra-xfce4-plugins"
#define ALL_ACTIONS  ((1u << KDC_N_ACTIONS) - 1u)

#define DEFAULT_SHOW_NAME      FALSE
#define DEFAULT_SHOW_BATTERY   TRUE
#define DEFAULT_SHOW_CHARGING  TRUE
#define DEFAULT_LOW_THRESHOLD  20

/* same order as KdcAction */
static const gchar *const ACTION_LABELS[KDC_N_ACTIONS] = {
  "Send Clipboard",
  "Send Files or Folders…",
  "Send Text or URL…",
  "Send Ping",
  "Find My Phone"
};

struct _KdcPrefs
{
  XfcePanelPlugin     *plugin;
  KdcPrefsChangedFunc  changed;
  gpointer             user_data;
  gboolean             xfconf_ok;
  GtkWidget           *dialog;          /* NULL while closed */

  gboolean             show_name;
  gboolean             show_battery;
  gboolean             show_charging;
  gint                 low_threshold;
  gint                 click_action;    /* KDC_CLICK_MENU or a KdcAction */
  guint                menu_actions;    /* bit mask, 1 << KdcAction */
};

const gchar *
kdc_action_label (KdcAction action)
{
  return (guint) action < KDC_N_ACTIONS ? ACTION_LABELS[action] : "";
}

/* storage (xfconf channel "xfce4-panel", per plugin instance)*/

static gchar *
setting_key (KdcPrefs *prefs, const gchar *name)
{
  return g_strdup_printf ("%s/%s", xfce_panel_plugin_get_property_base (prefs->plugin), name);
}

static gboolean
get_bool (KdcPrefs *prefs, const gchar *name, gboolean def)
{
  gboolean value = def;

  if (prefs->xfconf_ok)
    {
      gchar *key = setting_key (prefs, name);
      value = xfconf_channel_get_bool (xfconf_channel_get ("xfce4-panel"), key, def);
      g_free (key);
    }
  return value;
}

static gint
get_int (KdcPrefs *prefs, const gchar *name, gint def)
{
  gint value = def;

  if (prefs->xfconf_ok)
    {
      gchar *key = setting_key (prefs, name);
      value = xfconf_channel_get_int (xfconf_channel_get ("xfce4-panel"), key, def);
      g_free (key);
    }
  return value;
}

static void
set_bool (KdcPrefs *prefs, const gchar *name, gboolean value)
{
  gchar *key;

  if (!prefs->xfconf_ok)
    return;
  key = setting_key (prefs, name);
  xfconf_channel_set_bool (xfconf_channel_get ("xfce4-panel"), key, value);
  g_free (key);
}

static void
set_int (KdcPrefs *prefs, const gchar *name, gint value)
{
  gchar *key;

  if (!prefs->xfconf_ok)
    return;
  key = setting_key (prefs, name);
  xfconf_channel_set_int (xfconf_channel_get ("xfce4-panel"), key, value);
  g_free (key);
}

static void
load_settings (KdcPrefs *prefs)
{
  prefs->show_name = get_bool (prefs, "show-name", DEFAULT_SHOW_NAME);
  prefs->show_battery = get_bool (prefs, "show-battery", DEFAULT_SHOW_BATTERY);
  prefs->show_charging = get_bool (prefs, "show-charging-icon", DEFAULT_SHOW_CHARGING);
  prefs->low_threshold = CLAMP (get_int (prefs, "low-battery-threshold", DEFAULT_LOW_THRESHOLD), 0, 100);

  prefs->click_action = get_int (prefs, "click-action", KDC_CLICK_MENU);
  if (prefs->click_action < 0 || prefs->click_action >= KDC_N_ACTIONS)
    prefs->click_action = KDC_CLICK_MENU;

  prefs->menu_actions = ((guint) get_int (prefs, "menu-actions", (gint) ALL_ACTIONS)) & ALL_ACTIONS;
}

static void
settings_changed (KdcPrefs *prefs)
{
  set_bool (prefs, "show-name", prefs->show_name);
  set_bool (prefs, "show-battery", prefs->show_battery);
  set_bool (prefs, "show-charging-icon", prefs->show_charging);
  set_int (prefs, "low-battery-threshold", prefs->low_threshold);
  set_int (prefs, "click-action", prefs->click_action);
  set_int (prefs, "menu-actions", (gint) prefs->menu_actions);

  if (prefs->changed != NULL)
    prefs->changed (prefs->user_data);
}

/* dialog: signal handlers */

static void
on_show_name_toggled (GtkToggleButton *btn, KdcPrefs *prefs)
{
  prefs->show_name = gtk_toggle_button_get_active (btn);
  settings_changed (prefs);
}

static void
on_show_battery_toggled (GtkToggleButton *btn, KdcPrefs *prefs)
{
  GtkWidget *battery_opts = g_object_get_data (G_OBJECT (btn), "battery-opts");

  prefs->show_battery = gtk_toggle_button_get_active (btn);
  gtk_widget_set_sensitive (battery_opts, prefs->show_battery);
  settings_changed (prefs);
}

static void
on_show_charging_toggled (GtkToggleButton *btn, KdcPrefs *prefs)
{
  prefs->show_charging = gtk_toggle_button_get_active (btn);
  settings_changed (prefs);
}

static void
on_threshold_changed (GtkSpinButton *spin, KdcPrefs *prefs)
{
  prefs->low_threshold = gtk_spin_button_get_value_as_int (spin);
  settings_changed (prefs);
}

static void
on_click_action_changed (GtkComboBox *combo, KdcPrefs *prefs)
{
  const gchar *id = gtk_combo_box_get_active_id (combo);
  gint value = id != NULL ? atoi (id) : KDC_CLICK_MENU;

  prefs->click_action = (value >= 0 && value < KDC_N_ACTIONS) ? value : KDC_CLICK_MENU;
  settings_changed (prefs);
}

static void
on_action_toggled (GtkToggleButton *btn, KdcPrefs *prefs)
{
  guint bit = 1u << GPOINTER_TO_INT (g_object_get_data (G_OBJECT (btn), "action"));

  if (gtk_toggle_button_get_active (btn))
    prefs->menu_actions |= bit;
  else
    prefs->menu_actions &= ~bit;
  settings_changed (prefs);
}

static void
on_dialog_response (GtkWidget *dialog, gint response, KdcPrefs *prefs)
{
  gtk_widget_destroy (dialog);
}

static void
on_dialog_destroyed (GtkWidget *dialog, KdcPrefs *prefs)
{
  prefs->dialog = NULL;
  xfce_panel_plugin_unblock_menu (prefs->plugin);
}

/* dialog: building blocks*/

static GtkWidget *
make_page (void)
{
  GtkWidget *page = gtk_box_new (GTK_ORIENTATION_VERTICAL, 8);
  gtk_container_set_border_width (GTK_CONTAINER (page), 12);
  return page;
}

static GtkWidget *
make_heading (const gchar *markup)
{
  GtkWidget *label = gtk_label_new (NULL);
  gtk_label_set_markup (GTK_LABEL (label), markup);
  gtk_label_set_xalign (GTK_LABEL (label), 0.0f);
  return label;
}

static GtkWidget *
make_hint (const gchar *text)
{
  GtkWidget *label = gtk_label_new (text);
  gtk_label_set_xalign (GTK_LABEL (label), 0.0f);
  gtk_label_set_line_wrap (GTK_LABEL (label), TRUE);
  gtk_style_context_add_class (gtk_widget_get_style_context (label), "dim-label");
  return label;
}

/* The check button's signal is connected by the caller, after the initial state is set. */
static GtkWidget *
add_check (GtkWidget *box, const gchar *label, gboolean active)
{
  GtkWidget *check = gtk_check_button_new_with_label (label);
  gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (check), active);
  gtk_box_pack_start (GTK_BOX (box), check, FALSE, FALSE, 0);
  return check;
}

static GtkWidget *
build_button_page (KdcPrefs *prefs)
{
  GtkWidget *page = make_page ();
  GtkWidget *name_check, *battery_check, *charge_check, *battery_opts, *row, *spin;

  name_check = add_check (page, "Show device name", prefs->show_name);
  g_signal_connect (name_check, "toggled", G_CALLBACK (on_show_name_toggled), prefs);

  battery_check = add_check (page, "Show battery percentage", prefs->show_battery);

  /* options that only make sense while the percentage is shown */
  battery_opts = gtk_box_new (GTK_ORIENTATION_VERTICAL, 6);
  gtk_widget_set_margin_start (battery_opts, 24);

  charge_check = add_check (battery_opts, "Show ⚡ while charging", prefs->show_charging);
  g_signal_connect (charge_check, "toggled", G_CALLBACK (on_show_charging_toggled), prefs);

  row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
  spin = gtk_spin_button_new_with_range (0, 100, 5);
  gtk_spin_button_set_value (GTK_SPIN_BUTTON (spin), prefs->low_threshold);
  g_signal_connect (spin, "value-changed", G_CALLBACK (on_threshold_changed), prefs);
  gtk_box_pack_start (GTK_BOX (row), gtk_label_new ("Highlight low battery below"), FALSE, FALSE, 0);
  gtk_box_pack_start (GTK_BOX (row), spin, FALSE, FALSE, 0);
  gtk_box_pack_start (GTK_BOX (row), gtk_label_new ("%"), FALSE, FALSE, 0);
  gtk_box_pack_start (GTK_BOX (battery_opts), row, FALSE, FALSE, 0);
  gtk_box_pack_start (GTK_BOX (battery_opts),
                      make_hint ("Shown in red while the device is not charging. 0 turns it off."),
                      FALSE, FALSE, 0);

  gtk_widget_set_sensitive (battery_opts, prefs->show_battery);
  gtk_box_pack_start (GTK_BOX (page), battery_opts, FALSE, FALSE, 0);

  g_object_set_data (G_OBJECT (battery_check), "battery-opts", battery_opts);
  g_signal_connect (battery_check, "toggled", G_CALLBACK (on_show_battery_toggled), prefs);
  return page;
}

static GtkWidget *
build_actions_page (KdcPrefs *prefs)
{
  GtkWidget *page = make_page ();
  GtkWidget *combo;
  gchar id[16];

  gtk_box_pack_start (GTK_BOX (page), make_heading ("<b>Left click</b>"), FALSE, FALSE, 0);

  combo = gtk_combo_box_text_new ();
  gtk_combo_box_text_append (GTK_COMBO_BOX_TEXT (combo), "-1", "Open the menu");
  for (gint i = 0; i < KDC_N_ACTIONS; i++)
    {
      g_snprintf (id, sizeof id, "%d", i);
      gtk_combo_box_text_append (GTK_COMBO_BOX_TEXT (combo), id, kdc_action_label (i));
    }
  g_snprintf (id, sizeof id, "%d", prefs->click_action);
  gtk_combo_box_set_active_id (GTK_COMBO_BOX (combo), id);
  g_signal_connect (combo, "changed", G_CALLBACK (on_click_action_changed), prefs);
  gtk_box_pack_start (GTK_BOX (page), combo, FALSE, FALSE, 0);
  gtk_box_pack_start (GTK_BOX (page), make_hint ("Middle click always opens the menu."), FALSE, FALSE, 0);

  gtk_box_pack_start (GTK_BOX (page), make_heading ("<b>Items in the menu</b>"), FALSE, FALSE, 6);
  for (gint i = 0; i < KDC_N_ACTIONS; i++)
    {
      GtkWidget *check = add_check (page, kdc_action_label (i), (prefs->menu_actions >> i) & 1u);
      g_object_set_data (G_OBJECT (check), "action", GINT_TO_POINTER (i));
      g_signal_connect (check, "toggled", G_CALLBACK (on_action_toggled), prefs);
    }
  return page;
}

static void
show_dialog (KdcPrefs *prefs)
{
  GtkWidget *dlg, *area, *notebook, *footer, *link;

  if (prefs->dialog != NULL)
    {
      gtk_window_present (GTK_WINDOW (prefs->dialog));
      return;
    }

  xfce_panel_plugin_block_menu (prefs->plugin);   /* no "remove plugin" while open */

  dlg = gtk_dialog_new_with_buttons ("My KDE Connect Preferences", NULL, 0,
                                     "_Close", GTK_RESPONSE_CLOSE, NULL);
  gtk_window_set_icon_name (GTK_WINDOW (dlg), "preferences-system");
  gtk_window_set_screen (GTK_WINDOW (dlg), gtk_widget_get_screen (GTK_WIDGET (prefs->plugin)));
  gtk_window_set_position (GTK_WINDOW (dlg), GTK_WIN_POS_CENTER);
  gtk_window_set_default_size (GTK_WINDOW (dlg), 440, -1);

  area = gtk_dialog_get_content_area (GTK_DIALOG (dlg));
  gtk_container_set_border_width (GTK_CONTAINER (area), 8);
  gtk_box_set_spacing (GTK_BOX (area), 6);

  notebook = gtk_notebook_new ();
  gtk_notebook_append_page (GTK_NOTEBOOK (notebook), build_button_page (prefs), gtk_label_new ("Panel button"));
  gtk_notebook_append_page (GTK_NOTEBOOK (notebook), build_actions_page (prefs), gtk_label_new ("Actions"));
  gtk_box_pack_start (GTK_BOX (area), notebook, TRUE, TRUE, 0);

  /* footer: license + project link, always visible below the tabs */
  gtk_box_pack_start (GTK_BOX (area), gtk_separator_new (GTK_ORIENTATION_HORIZONTAL), FALSE, FALSE, 4);
  footer = make_hint ("Licensed under the GNU General Public License v3.0 (GPL-3.0).");
  gtk_box_pack_start (GTK_BOX (area), footer, FALSE, FALSE, 0);
  link = gtk_link_button_new_with_label (PROJECT_URL, PROJECT_URL);
  gtk_widget_set_halign (link, GTK_ALIGN_START);
  gtk_box_pack_start (GTK_BOX (area), link, FALSE, FALSE, 0);

  g_signal_connect (dlg, "response", G_CALLBACK (on_dialog_response), prefs);
  g_signal_connect (dlg, "destroy", G_CALLBACK (on_dialog_destroyed), prefs);

  prefs->dialog = dlg;
  gtk_widget_show_all (dlg);
}

static void
on_configure_plugin (XfcePanelPlugin *plugin, KdcPrefs *prefs)
{
  show_dialog (prefs);
}

/*this puplic */

KdcPrefs *
kdc_prefs_new (XfcePanelPlugin *plugin, KdcPrefsChangedFunc changed, gpointer user_data)
{
  KdcPrefs *prefs = g_new0 (KdcPrefs, 1);
  GError *err = NULL;

  prefs->plugin = plugin;
  prefs->changed = changed;
  prefs->user_data = user_data;

  prefs->xfconf_ok = xfconf_init (&err);
  if (!prefs->xfconf_ok)
    {
      g_warning ("xfce4-kdeconnect: xfconf unavailable, preferences will not be saved: %s", err->message);
      g_error_free (err);
    }
  load_settings (prefs);

  g_signal_connect (plugin, "configure-plugin", G_CALLBACK (on_configure_plugin), prefs);
  xfce_panel_plugin_menu_show_configure (plugin);
  return prefs;
}

void
kdc_prefs_free (KdcPrefs *prefs)
{
  if (prefs == NULL)
    return;

  g_signal_handlers_disconnect_by_data (prefs->plugin, prefs);
  if (prefs->dialog != NULL)
    gtk_widget_destroy (prefs->dialog);   /* handler resets prefs->dialog */
  if (prefs->xfconf_ok)
    xfconf_shutdown ();
  g_free (prefs);
}

gboolean
kdc_prefs_get_show_name (const KdcPrefs *prefs)
{
  return prefs->show_name;
}

gboolean
kdc_prefs_get_show_battery (const KdcPrefs *prefs)
{
  return prefs->show_battery;
}

gboolean
kdc_prefs_get_show_charging_icon (const KdcPrefs *prefs)
{
  return prefs->show_charging;
}

gint
kdc_prefs_get_low_threshold (const KdcPrefs *prefs)
{
  return prefs->low_threshold;
}

gint
kdc_prefs_get_click_action (const KdcPrefs *prefs)
{
  return prefs->click_action;
}

gboolean
kdc_prefs_get_action_enabled (const KdcPrefs *prefs, KdcAction action)
{
  return (guint) action < KDC_N_ACTIONS && ((prefs->menu_actions >> action) & 1u) != 0;
}