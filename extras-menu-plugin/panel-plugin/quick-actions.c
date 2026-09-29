#include "quick-actions.h"

#include <gio/gio.h>

typedef struct
{
    const gchar *command;
    ExtrasMenuQuickActionsCloseFunc close_func;
    gpointer close_data;
} QuickAction;

/* Fire-and-forget launcher: GSubprocess without any pipes, so we
 * don't track the exit status (the launched program runs as its own
 * process, independent of the panel). If the program isn't installed
 * we just log a warning instead of crashing or bothering the user. */
static void
launch_command(const gchar *command)
{
    GError *error = NULL;
    GSubprocess *proc = g_subprocess_new(G_SUBPROCESS_FLAGS_NONE, &error,
                                          command, NULL);
    if (proc == NULL)
    {
        g_warning("extras-menu: failed to launch %s: %s",
                  command, error != NULL ? error->message : "unknown error");
        g_clear_error(&error);
        return;
    }

    g_object_unref(proc);
}

/* Shared "clicked" handler for all four buttons: close the dropdown
 * first (so it isn't in the way -- or in a screenshot), then launch
 * the button's program. */
static void
on_quick_action_clicked(GtkButton *button, gpointer user_data)
{
    (void) button;
    const QuickAction *action = user_data;

    if (action->close_func != NULL)
        action->close_func(action->close_data);

    launch_command(action->command);
}

static void
connect_action(GtkWidget *button, const gchar *command,
               ExtrasMenuQuickActionsCloseFunc close_func, gpointer close_data)
{
    if (button == NULL)
        return;

    QuickAction *action = g_new0(QuickAction, 1);
    action->command = command;
    action->close_func = close_func;
    action->close_data = close_data;

    /* g_free as the destroy notify: the struct dies with the button. */
    g_signal_connect_data(button, "clicked",
                           G_CALLBACK(on_quick_action_clicked), action,
                           (GClosureNotify) g_free, 0);
}

void
extras_menu_quick_actions_connect(GtkWidget *screenshot_button,
                                   GtkWidget *settings_button,
                                   GtkWidget *lock_button,
                                   GtkWidget *power_button,
                                   ExtrasMenuQuickActionsCloseFunc close_func,
                                   gpointer close_data)
{
    connect_action(screenshot_button, "xfce4-screenshooter", close_func, close_data);
    connect_action(settings_button, "xfce4-settings-manager", close_func, close_data);
    connect_action(lock_button, "xflock4", close_func, close_data);
    connect_action(power_button, "xfce4-session-logout", close_func, close_data);
}