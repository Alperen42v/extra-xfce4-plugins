#include "quick-actions-page.h"
#include "quick-actions-config.h"

/* How long after the last edit the list is written out. Typing in an
 * entry changes it on every keystroke; waiting a moment means one save
 * (and one rebuild of the dropdown's buttons) per burst of typing. */
#define SAVE_DEBOUNCE_MS 400

typedef struct
{
    GtkWidget *rows_box;     /* vertical box holding one frame per action */
    GtkWidget *empty_label;  /* shown instead of rows when there are none */
    GtkWidget *add_button;
    GtkWidget *count_label;

    GPtrArray *actions;      /* owned ExtrasMenuQuickActionConfig*; the edited copy */
    guint save_source_id;    /* pending debounced save, or 0 */
} PageState;

/* Per-row context. Lives and dies with its row's frame widget; rows are
 * rebuilt whenever the list's structure changes (add/remove/move), so
 * `index` is always current for the row's lifetime. */
typedef struct
{
    PageState *state;
    guint index;
    GtkWidget *preview;
} RowCtx;

static void rebuild_rows(PageState *st);

/* GtkCallback-shaped wrapper: gtk_widget_destroy() itself takes one
 * argument, and casting it to GtkCallback is undefined behaviour. */
static void
destroy_child(GtkWidget *child, gpointer user_data)
{
    (void) user_data;
    gtk_widget_destroy(child);
}

/* --- saving ----------------------------------------------------------------- */

static void
save_now(PageState *st)
{
    GError *error = NULL;

    if (!extras_menu_quick_actions_config_save(st->actions, &error))
    {
        g_warning("extras-menu: couldn't save quick actions: %s",
                  error != NULL ? error->message : "unknown error");
        g_clear_error(&error);
    }
}

static gboolean
on_save_timeout(gpointer user_data)
{
    PageState *st = user_data;

    st->save_source_id = 0; /* this source is removed by returning below */
    save_now(st);
    return G_SOURCE_REMOVE;
}

static void
schedule_save(PageState *st)
{
    if (st->save_source_id == 0)
        st->save_source_id = g_timeout_add(SAVE_DEBOUNCE_MS, on_save_timeout, st);
}

/* --- small helpers ---------------------------------------------------------- */

static ExtrasMenuQuickActionConfig *
row_action(RowCtx *ctx)
{
    return g_ptr_array_index(ctx->state->actions, ctx->index);
}

/* Stores an entry's text into a config field, without surrounding
 * whitespace (the entry itself keeps whatever the user typed). */
static void
set_field(gchar **field, const gchar *text)
{
    g_free(*field);
    *field = g_strstrip(g_strdup(text));
}

static void
update_command_warning(GtkWidget *entry, const gchar *command)
{
    const gchar *problem = extras_menu_quick_action_command_problem(command);

    if (problem != NULL)
    {
        gtk_entry_set_icon_from_icon_name(GTK_ENTRY(entry), GTK_ENTRY_ICON_SECONDARY,
                                           "dialog-warning-symbolic");
        gtk_entry_set_icon_tooltip_text(GTK_ENTRY(entry), GTK_ENTRY_ICON_SECONDARY, problem);
    }
    else
    {
        gtk_entry_set_icon_from_icon_name(GTK_ENTRY(entry), GTK_ENTRY_ICON_SECONDARY, NULL);
    }
}

static void
update_footer(PageState *st)
{
    guint count = st->actions->len;

    gtk_widget_set_sensitive(st->add_button, count < EXTRAS_MENU_QUICK_ACTIONS_MAX);
    gtk_widget_set_visible(st->empty_label, count == 0);

    gchar *text = g_strdup_printf("%u / %d buttons", count, EXTRAS_MENU_QUICK_ACTIONS_MAX);
    gtk_label_set_text(GTK_LABEL(st->count_label), text);
    g_free(text);
}

/* --- row field handlers ------------------------------------------------------ */

static void
on_icon_changed(GtkEditable *editable, gpointer user_data)
{
    RowCtx *ctx = user_data;
    ExtrasMenuQuickActionConfig *action = row_action(ctx);

    set_field(&action->icon, gtk_entry_get_text(GTK_ENTRY(editable)));
    extras_menu_quick_action_set_image(GTK_IMAGE(ctx->preview), action->icon, GTK_ICON_SIZE_BUTTON);
    schedule_save(ctx->state);
}

static void
on_name_changed(GtkEditable *editable, gpointer user_data)
{
    RowCtx *ctx = user_data;

    set_field(&row_action(ctx)->name, gtk_entry_get_text(GTK_ENTRY(editable)));
    schedule_save(ctx->state);
}

static void
on_command_changed(GtkEditable *editable, gpointer user_data)
{
    RowCtx *ctx = user_data;
    ExtrasMenuQuickActionConfig *action = row_action(ctx);

    set_field(&action->command, gtk_entry_get_text(GTK_ENTRY(editable)));
    update_command_warning(GTK_WIDGET(editable), action->command);
    schedule_save(ctx->state);
}

/* --- choosing an image file for the icon ------------------------------------ */

/* The icon entry the file chooser should fill in. Kept as a weak
 * pointer: the chooser is non-blocking, so the row (and its entry) could
 * in principle be rebuilt before the user answers. */
typedef struct
{
    GtkWidget *entry;
} BrowseTarget;

static void
browse_target_free(gpointer data)
{
    BrowseTarget *target = data;

    if (target->entry != NULL)
        g_object_remove_weak_pointer(G_OBJECT(target->entry), (gpointer *) &target->entry);

    g_free(target);
}

static void
on_browse_response(GtkDialog *dialog, gint response_id, gpointer user_data)
{
    BrowseTarget *target = user_data;

    if (response_id == GTK_RESPONSE_ACCEPT && target->entry != NULL)
    {
        gchar *filename = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog));
        if (filename != NULL)
        {
            /* Triggers on_icon_changed(), which updates preview + saves. */
            gtk_entry_set_text(GTK_ENTRY(target->entry), filename);
            g_free(filename);
        }
    }

    gtk_widget_destroy(GTK_WIDGET(dialog));
}

static void
on_browse_clicked(GtkButton *button, gpointer user_data)
{
    GtkWidget *entry = user_data;
    GtkWidget *toplevel = gtk_widget_get_toplevel(GTK_WIDGET(button));

    GtkWidget *dialog = gtk_file_chooser_dialog_new(
        "Choose an icon image",
        GTK_IS_WINDOW(toplevel) ? GTK_WINDOW(toplevel) : NULL,
        GTK_FILE_CHOOSER_ACTION_OPEN,
        "_Cancel", GTK_RESPONSE_CANCEL,
        "_Open", GTK_RESPONSE_ACCEPT,
        NULL);

    GtkFileFilter *filter = gtk_file_filter_new();
    gtk_file_filter_set_name(filter, "Images");
    gtk_file_filter_add_pixbuf_formats(filter);
    gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(dialog), filter);

    BrowseTarget *target = g_new0(BrowseTarget, 1);
    target->entry = entry;
    g_object_add_weak_pointer(G_OBJECT(entry), (gpointer *) &target->entry);
    g_object_set_data_full(G_OBJECT(dialog), "browse-target", target, browse_target_free);

    g_signal_connect(dialog, "response", G_CALLBACK(on_browse_response), target);
    gtk_window_set_modal(GTK_WINDOW(dialog), TRUE);
    gtk_widget_show(dialog);
}

/* --- structural handlers (these rebuild the rows) ----------------------------- */

static void
swap_actions(PageState *st, guint a, guint b)
{
    gpointer tmp = st->actions->pdata[a];
    st->actions->pdata[a] = st->actions->pdata[b];
    st->actions->pdata[b] = tmp;
}

/* Note for the three handlers below: rebuild_rows() destroys the row
 * that owns the clicked button, and with it `ctx`. So everything needed
 * is copied out of ctx first and ctx is not touched afterwards. */

static void
on_move_up_clicked(GtkButton *button, gpointer user_data)
{
    RowCtx *ctx = user_data;
    PageState *st = ctx->state;
    guint index = ctx->index;
    (void) button;

    if (index == 0)
        return;

    swap_actions(st, index, index - 1);
    rebuild_rows(st);
    schedule_save(st);
}

static void
on_move_down_clicked(GtkButton *button, gpointer user_data)
{
    RowCtx *ctx = user_data;
    PageState *st = ctx->state;
    guint index = ctx->index;
    (void) button;

    if (index + 1 >= st->actions->len)
        return;

    swap_actions(st, index, index + 1);
    rebuild_rows(st);
    schedule_save(st);
}

static void
on_remove_clicked(GtkButton *button, gpointer user_data)
{
    RowCtx *ctx = user_data;
    PageState *st = ctx->state;
    guint index = ctx->index;
    (void) button;

    g_ptr_array_remove_index(st->actions, index);
    rebuild_rows(st);
    schedule_save(st);
}

static void
on_add_clicked(GtkButton *button, gpointer user_data)
{
    PageState *st = user_data;
    (void) button;

    if (st->actions->len >= EXTRAS_MENU_QUICK_ACTIONS_MAX)
        return;

    g_ptr_array_add(st->actions, extras_menu_quick_action_config_new(
        "New button", "applications-other-symbolic", ""));
    rebuild_rows(st);
    schedule_save(st);
}

static void
on_reset_response(GtkDialog *dialog, gint response_id, gpointer user_data)
{
    PageState *st = user_data;

    if (response_id == GTK_RESPONSE_ACCEPT)
    {
        g_ptr_array_unref(st->actions);
        st->actions = extras_menu_quick_actions_config_defaults();
        rebuild_rows(st);
        schedule_save(st);
    }

    gtk_widget_destroy(GTK_WIDGET(dialog));
}

static void
on_reset_clicked(GtkButton *button, gpointer user_data)
{
    GtkWidget *toplevel = gtk_widget_get_toplevel(GTK_WIDGET(button));

    GtkWidget *dialog = gtk_message_dialog_new(
        GTK_IS_WINDOW(toplevel) ? GTK_WINDOW(toplevel) : NULL,
        GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
        GTK_MESSAGE_QUESTION, GTK_BUTTONS_NONE,
        "Reset to the default buttons?");
    gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(dialog), "%s",
        "Your current buttons will be replaced by screenshot, settings, lock and log out.");
    gtk_dialog_add_buttons(GTK_DIALOG(dialog),
                           "_Cancel", GTK_RESPONSE_CANCEL,
                           "_Reset", GTK_RESPONSE_ACCEPT,
                           NULL);

    g_signal_connect(dialog, "response", G_CALLBACK(on_reset_response), user_data);
    gtk_widget_show(dialog);
}

/* --- building the rows -------------------------------------------------------- */

static GtkWidget *
make_icon_button(const gchar *icon_name, const gchar *tooltip)
{
    GtkWidget *button = gtk_button_new_from_icon_name(icon_name, GTK_ICON_SIZE_BUTTON);
    gtk_button_set_relief(GTK_BUTTON(button), GTK_RELIEF_NONE);
    gtk_widget_set_tooltip_text(button, tooltip);
    return button;
}

/* One action = one framed block:
 *
 *   [preview] [icon entry........] [...]  [name entry......]  [^] [v] [x]
 *             [command entry.............................]
 */
static GtkWidget *
build_row(PageState *st, guint index)
{
    ExtrasMenuQuickActionConfig *action = g_ptr_array_index(st->actions, index);

    RowCtx *ctx = g_new0(RowCtx, 1);
    ctx->state = st;
    ctx->index = index;

    GtkWidget *frame = gtk_frame_new(NULL);
    g_object_set_data_full(G_OBJECT(frame), "row-ctx", ctx, g_free);

    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 6);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 6);
    gtk_container_set_border_width(GTK_CONTAINER(grid), 8);
    gtk_container_add(GTK_CONTAINER(frame), grid);

    /* preview of what the button will look like */
    ctx->preview = gtk_image_new();
    extras_menu_quick_action_set_image(GTK_IMAGE(ctx->preview), action->icon, GTK_ICON_SIZE_BUTTON);

    GtkWidget *icon_entry = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(icon_entry), action->icon);
    gtk_entry_set_placeholder_text(GTK_ENTRY(icon_entry), "Icon name or image file");
    gtk_entry_set_width_chars(GTK_ENTRY(icon_entry), 20);
    gtk_widget_set_tooltip_text(icon_entry,
        "A theme icon name (e.g. camera-photo-symbolic) or the path of an image file.");

    GtkWidget *browse_button = make_icon_button("document-open-symbolic", "Choose an image file…");

    GtkWidget *name_entry = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(name_entry), action->name);
    gtk_entry_set_placeholder_text(GTK_ENTRY(name_entry), "Name");
    gtk_entry_set_width_chars(GTK_ENTRY(name_entry), 14);
    gtk_widget_set_hexpand(name_entry, TRUE);
    gtk_widget_set_tooltip_text(name_entry, "Shown when you hover over the button.");

    GtkWidget *up_button = make_icon_button("go-up-symbolic", "Move left");
    GtkWidget *down_button = make_icon_button("go-down-symbolic", "Move right");
    GtkWidget *remove_button = make_icon_button("list-remove-symbolic", "Remove this button");
    gtk_widget_set_sensitive(up_button, index > 0);
    gtk_widget_set_sensitive(down_button, index + 1 < st->actions->len);

    GtkWidget *command_entry = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(command_entry), action->command);
    gtk_entry_set_placeholder_text(GTK_ENTRY(command_entry), "Command, e.g. xfce4-terminal");
    gtk_widget_set_hexpand(command_entry, TRUE);
    gtk_widget_set_tooltip_text(command_entry,
        "Run when the button is clicked. Arguments and quotes work "
        "(xfce4-terminal --hold); pipes, ~ and $VARIABLES do not -- "
        "wrap those in: sh -c '...'");
    update_command_warning(command_entry, action->command);

    gtk_grid_attach(GTK_GRID(grid), ctx->preview,   0, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), icon_entry,     1, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), browse_button,  2, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), name_entry,     3, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), up_button,      4, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), down_button,    5, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), remove_button,  6, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), command_entry,  1, 1, 3, 1);

    /* Handlers are connected only now, after the initial text was set,
     * so filling the entries doesn't count as an edit. */
    g_signal_connect(icon_entry, "changed", G_CALLBACK(on_icon_changed), ctx);
    g_signal_connect(name_entry, "changed", G_CALLBACK(on_name_changed), ctx);
    g_signal_connect(command_entry, "changed", G_CALLBACK(on_command_changed), ctx);
    g_signal_connect(browse_button, "clicked", G_CALLBACK(on_browse_clicked), icon_entry);
    g_signal_connect(up_button, "clicked", G_CALLBACK(on_move_up_clicked), ctx);
    g_signal_connect(down_button, "clicked", G_CALLBACK(on_move_down_clicked), ctx);
    g_signal_connect(remove_button, "clicked", G_CALLBACK(on_remove_clicked), ctx);

    return frame;
}

static void
rebuild_rows(PageState *st)
{
    gtk_container_foreach(GTK_CONTAINER(st->rows_box), destroy_child, NULL);

    for (guint i = 0; i < st->actions->len; i++)
    {
        GtkWidget *row = build_row(st, i);
        gtk_box_pack_start(GTK_BOX(st->rows_box), row, FALSE, FALSE, 0);
        gtk_widget_show_all(row);
    }

    update_footer(st);
}

/* --- the page itself ----------------------------------------------------------- */

static void
on_page_destroy(GtkWidget *page, gpointer user_data)
{
    PageState *st = user_data;
    (void) page;

    /* Closing the dialog right after an edit must not lose it. */
    if (st->save_source_id != 0)
    {
        g_source_remove(st->save_source_id);
        st->save_source_id = 0;
        save_now(st);
    }

    g_ptr_array_unref(st->actions);
    g_free(st);
}

GtkWidget *
extras_menu_quick_actions_page_new(void)
{
    PageState *st = g_new0(PageState, 1);
    st->actions = extras_menu_quick_actions_config_load();

    GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);

    GtkWidget *heading = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(heading), "<b>Quick action buttons</b>");
    gtk_label_set_xalign(GTK_LABEL(heading), 0.0);
    gtk_box_pack_start(GTK_BOX(page), heading, FALSE, FALSE, 0);

    gchar *description_text = g_strdup_printf(
        "The small buttons in the top bar of the menu. Each one has an icon, "
        "a name (its tooltip) and a command to run. Up to %d buttons; a "
        "button without a command isn't shown.",
        EXTRAS_MENU_QUICK_ACTIONS_MAX);
    GtkWidget *description = gtk_label_new(description_text);
    g_free(description_text);
    gtk_label_set_line_wrap(GTK_LABEL(description), TRUE);
    gtk_label_set_xalign(GTK_LABEL(description), 0.0);
    gtk_label_set_max_width_chars(GTK_LABEL(description), 60);
    gtk_style_context_add_class(gtk_widget_get_style_context(description), "dim-label");
    gtk_box_pack_start(GTK_BOX(page), description, FALSE, FALSE, 0);

    /* Rows live in a scrolled window so five of them can't make the
     * dialog taller than a small screen. */
    st->rows_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    GtkWidget *scrolled = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled),
                                    GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(scrolled), GTK_SHADOW_NONE);
    gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(scrolled), TRUE);
    gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(scrolled), 400);
    gtk_container_add(GTK_CONTAINER(scrolled), st->rows_box);
    gtk_box_pack_start(GTK_BOX(page), scrolled, TRUE, TRUE, 0);

    st->empty_label = gtk_label_new("No buttons -- the top bar will only show the battery.");
    gtk_label_set_xalign(GTK_LABEL(st->empty_label), 0.0);
    gtk_widget_set_no_show_all(st->empty_label, TRUE); /* update_footer() decides */
    gtk_box_pack_start(GTK_BOX(page), st->empty_label, FALSE, FALSE, 0);

    GtkWidget *footer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);

    st->add_button = gtk_button_new_with_label("Add button");
    gtk_button_set_image(GTK_BUTTON(st->add_button),
                         gtk_image_new_from_icon_name("list-add-symbolic", GTK_ICON_SIZE_BUTTON));
    gtk_button_set_always_show_image(GTK_BUTTON(st->add_button), TRUE);
    g_signal_connect(st->add_button, "clicked", G_CALLBACK(on_add_clicked), st);

    GtkWidget *reset_button = gtk_button_new_with_label("Reset to defaults");
    g_signal_connect(reset_button, "clicked", G_CALLBACK(on_reset_clicked), st);

    st->count_label = gtk_label_new("");
    gtk_style_context_add_class(gtk_widget_get_style_context(st->count_label), "dim-label");

    gtk_box_pack_start(GTK_BOX(footer), st->add_button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(footer), reset_button, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(footer), st->count_label, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(page), footer, FALSE, FALSE, 0);

    g_signal_connect(page, "destroy", G_CALLBACK(on_page_destroy), st);

    rebuild_rows(st);

    return page;
}