#ifndef EXTRAS_MENU_QUICK_ACTIONS_CONFIG_H
#define EXTRAS_MENU_QUICK_ACTIONS_CONFIG_H

#include <gtk/gtk.h>

G_BEGIN_DECLS

/* The most quick-action buttons the top bar can hold. */
#define EXTRAS_MENU_QUICK_ACTIONS_MAX 5

/*
 * One user-configurable button in the dropdown's top bar. All three
 * strings are always non-NULL (possibly empty) and owned by the struct.
 *
 *   name     shown as the button's tooltip, and as its title in the
 *            preferences editor
 *   icon     either a theme icon name ("camera-photo-symbolic") or the
 *            path of an image file ("/home/me/pic.png", "~/pic.png")
 *   command  command line to run when the button is clicked, split
 *            shell-style into program + arguments (so quotes work, but
 *            pipes, "~" and $VARIABLES are not expanded). A button with
 *            an empty command is kept in the config but not shown.
 */
typedef struct
{
    gchar *name;
    gchar *icon;
    gchar *command;
} ExtrasMenuQuickActionConfig;

ExtrasMenuQuickActionConfig *extras_menu_quick_action_config_new(const gchar *name,
                                                                  const gchar *icon,
                                                                  const gchar *command);
ExtrasMenuQuickActionConfig *extras_menu_quick_action_config_copy(const ExtrasMenuQuickActionConfig *src);
void extras_menu_quick_action_config_free(gpointer action);

/* The four buttons the plugin shipped with (screenshot, settings, lock,
 * log out). Returns a new GPtrArray of owned ExtrasMenuQuickActionConfig*
 * that frees its elements itself; release it with g_ptr_array_unref(). */
GPtrArray *extras_menu_quick_actions_config_defaults(void);

/* Loads the saved list from
 * ~/.config/xfce4/extras-menu/quick-actions.ini (or wherever
 * XDG_CONFIG_HOME points). Falls back to the defaults if the file
 * doesn't exist yet or can't be read; a file that exists and lists zero
 * buttons is respected as "the user removed them all". Never returns
 * more than EXTRAS_MENU_QUICK_ACTIONS_MAX entries. Same ownership as
 * extras_menu_quick_actions_config_defaults(). */
GPtrArray *extras_menu_quick_actions_config_load(void);

/* Writes the list to the config file (keeping at most
 * EXTRAS_MENU_QUICK_ACTIONS_MAX entries) and, on success, calls every
 * registered change listener. */
gboolean extras_menu_quick_actions_config_save(const GPtrArray *actions, GError **error);

/* Change notification, so the dropdown can rebuild its buttons the
 * moment the preferences editor saves. Listeners run in the main thread
 * right after a successful save. Remove a listener before whatever
 * user_data points to goes away. */
typedef void (*ExtrasMenuQuickActionsChangedFunc)(gpointer user_data);

guint extras_menu_quick_actions_config_add_listener(ExtrasMenuQuickActionsChangedFunc func,
                                                     gpointer user_data);
void extras_menu_quick_actions_config_remove_listener(guint listener_id);

/* Sets a GtkImage from a config icon string: a theme icon name, or an
 * image file path (scaled to `size`). Empty falls back to a generic
 * icon; an unreadable file shows the theme's "missing image" icon. */
void extras_menu_quick_action_set_image(GtkImage *image, const gchar *icon, GtkIconSize size);

/* Returns a short, static, human-readable problem with a command line
 * ("empty", "unbalanced quotes"), or NULL if it looks runnable. Used by
 * the editor to flag bad entries; the dropdown simply doesn't show
 * buttons whose command is empty. */
const gchar *extras_menu_quick_action_command_problem(const gchar *command);

G_END_DECLS

#endif /* EXTRAS_MENU_QUICK_ACTIONS_CONFIG_H */