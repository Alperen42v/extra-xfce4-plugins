/* Preferences: xfconf storage + the "Properties" dialog of the panel plugin. */
#ifndef PREFERENCES_H
#define PREFERENCES_H

#include <gtk/gtk.h>
#include <libxfce4panel/libxfce4panel.h>

G_BEGIN_DECLS

/* The things a device button can do (also the entries of its menu). */
typedef enum
{
  KDC_ACTION_CLIPBOARD,
  KDC_ACTION_FILES,
  KDC_ACTION_TEXT,
  KDC_ACTION_PING,
  KDC_ACTION_FIND,
  KDC_N_ACTIONS
} KdcAction;

/* Value of the "left click" preference meaning "open the menu". */
#define KDC_CLICK_MENU (-1)

const gchar *kdc_action_label (KdcAction action);

typedef struct _KdcPrefs KdcPrefs;

/* Called whenever the user changes a setting in the dialog. */
typedef void (*KdcPrefsChangedFunc) (gpointer user_data);

/* Loads the settings, adds "Properties..." to the plugin's right-click menu. */
KdcPrefs *kdc_prefs_new  (XfcePanelPlugin     *plugin,
                          KdcPrefsChangedFunc  changed,
                          gpointer             user_data);
void      kdc_prefs_free (KdcPrefs *prefs);

gboolean  kdc_prefs_get_show_name          (const KdcPrefs *prefs);
gboolean  kdc_prefs_get_show_battery       (const KdcPrefs *prefs);
gboolean  kdc_prefs_get_show_charging_icon (const KdcPrefs *prefs);
gint      kdc_prefs_get_low_threshold      (const KdcPrefs *prefs);   /* 0 = off */
gint      kdc_prefs_get_click_action       (const KdcPrefs *prefs);   /* KDC_CLICK_MENU or a KdcAction */
gboolean  kdc_prefs_get_action_enabled     (const KdcPrefs *prefs, KdcAction action);

G_END_DECLS

#endif