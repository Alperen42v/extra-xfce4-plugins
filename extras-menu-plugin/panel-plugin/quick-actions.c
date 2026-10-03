#include "quick-actions.h"

#include <gio/gio.h>

#include "popover.h"
#include "preferences/quick-actions-config.h"

/* What one button's "clicked" handler needs. Owned by the button. */
typedef struct
{
    gchar *command;
    ExtrasMenuQuickActionsCloseFunc close_func;
    gpointer close_data;
} QuickAction;

static void
quick_action_free(gpointer data, GClosure *closure)
{
    QuickAction *action = data;
    (void) closure;

    g_free(action->command);
    g_free(action);
}

/* Fire-and-forget launcher: GSubprocess without any pipes, so we
 * don't track the exit status (the launched program runs as its own
 * process, independent of the panel). The command line is split
 * shell-style into program + arguments. If it can't be parsed or the
 * program isn't installed we just log a warning instead of crashing or
 * bothering the user. */
static void
launch_command(const gchar *command_line)
{
    GError *error = NULL;
    gchar **argv = NULL;

    if (!g_shell_parse_argv(command_line, NULL, &argv, &error))
    {
        g_warning("extras-menu: can't parse command \"%s\": %s",
                  command_line, error != NULL ? error->message : "unknown error");
        g_clear_error(&error);
        return;
    }

    GSubprocess *proc = g_subprocess_newv((const gchar * const *) argv,
                                           G_SUBPROCESS_FLAGS_NONE, &error);
    g_strfreev(argv);

    if (proc == NULL)
    {
        g_warning("extras-menu: failed to launch %s: %s",
                  command_line, error != NULL ? error->message : "unknown error");
        g_clear_error(&error);
        return;
    }

    g_object_unref(proc);
}

/* Shared "clicked" handler for all buttons: close the dropdown first
 * (so it isn't in the way -- or in a screenshot), then launch the
 * button's program. */
static void
on_quick_action_clicked(GtkButton *button, gpointer user_data)
{
    (void) button;
    const QuickAction *action = user_data;

    if (action->close_func != NULL)
        action->close_func(action->close_data);

    launch_command(action->command);
}

/* --- keeping a container's buttons in sync with the config ------------------- */

typedef struct
{
    GtkWidget *box;
    ExtrasMenuQuickActionsCloseFunc close_func;
    gpointer close_data;
    guint listener_id;
} QuickActionsView;

/* GtkCallback-shaped wrapper: gtk_widget_destroy() itself takes one
 * argument, and casting it to GtkCallback is undefined behaviour. */
static void
destroy_child(GtkWidget *child, gpointer user_data)
{
    (void) user_data;
    gtk_widget_destroy(child);
}

static void
rebuild_buttons(QuickActionsView *view)
{
    gtk_container_foreach(GTK_CONTAINER(view->box), destroy_child, NULL);

    GPtrArray *actions = extras_menu_quick_actions_config_load();

    for (guint i = 0; i < actions->len && i < EXTRAS_MENU_QUICK_ACTIONS_MAX; i++)
    {
        const ExtrasMenuQuickActionConfig *config = g_ptr_array_index(actions, i);

        /* Nothing to run: keep it out of the dropdown rather than show
         * a button that does nothing. It stays in the config, so the
         * user can still finish setting it up in the editor. */
        if (config->command[0] == '\0')
            continue;

        GtkWidget *button = extras_menu_top_bar_button_new();

        GtkWidget *image = gtk_image_new();
        extras_menu_quick_action_set_image(GTK_IMAGE(image), config->icon, GTK_ICON_SIZE_BUTTON);
        gtk_container_add(GTK_CONTAINER(button), image);

        if (config->name[0] != '\0')
            gtk_widget_set_tooltip_text(button, config->name);

        QuickAction *action = g_new0(QuickAction, 1);
        action->command = g_strdup(config->command);
        action->close_func = view->close_func;
        action->close_data = view->close_data;

        /* quick_action_free as the destroy notify: the struct dies with the button. */
        g_signal_connect_data(button, "clicked",
                               G_CALLBACK(on_quick_action_clicked), action,
                               quick_action_free, 0);

        gtk_box_pack_start(GTK_BOX(view->box), button, FALSE, FALSE, 0);
        gtk_widget_show_all(button);
    }

    g_ptr_array_unref(actions);
}

static void
on_config_changed(gpointer user_data)
{
    rebuild_buttons(user_data);
}

static void
on_box_destroy(GtkWidget *box, gpointer user_data)
{
    QuickActionsView *view = user_data;
    (void) box;

    extras_menu_quick_actions_config_remove_listener(view->listener_id);
    g_free(view);
}

void
extras_menu_quick_actions_attach(GtkWidget *box,
                                  ExtrasMenuQuickActionsCloseFunc close_func,
                                  gpointer close_data)
{
    if (box == NULL)
        return;

    QuickActionsView *view = g_new0(QuickActionsView, 1);
    view->box = box;
    view->close_func = close_func;
    view->close_data = close_data;

    rebuild_buttons(view);

    view->listener_id = extras_menu_quick_actions_config_add_listener(on_config_changed, view);
    g_signal_connect(box, "destroy", G_CALLBACK(on_box_destroy), view);
}