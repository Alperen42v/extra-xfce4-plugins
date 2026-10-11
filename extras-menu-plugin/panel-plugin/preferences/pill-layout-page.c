#include "pill-layout-page.h"
#include "pill-layout-config.h"

#include <string.h>

/*
 * The page shows EXTRAS_MENU_PILL_COUNT fixed cells in the same grid as
 * the dropdown. Cell i shows whichever pill sits at position i of the
 * layout, and only the first `layout->len` cells are visible: the rest
 * stand for pills that aren't in the menu. Nothing here ever creates or
 * destroys a cell after the page is built: moving, removing or adding a
 * pill edits the layout and then refresh() rewrites every cell's icon,
 * title and arrows and shows/hides the cells. That matters for drag and
 * drop -- the drop handler runs inside GTK's drag machinery, and
 * destroying the cell being dropped on from there would be asking for
 * a crash.
 */

#define ARROW_COUNT 4

typedef struct _PageState PageState;

typedef struct
{
    PageState *state;
    guint index;                         /* this cell's position in the layout */
    GtkWidget *root;                     /* the whole cell, shown/hidden by refresh() */
    GtkWidget *icon;
    GtkWidget *label;
    GtkWidget *arrow_buttons[ARROW_COUNT];
} Slot;

struct _PageState
{
    GPtrArray *layout;                   /* const ExtrasMenuPillInfo*, one per shown pill; not owned */
    Slot slots[EXTRAS_MENU_PILL_COUNT];
    GtkWidget *add_button;
    GtkWidget *empty_label;              /* shown instead of the grid when no pill is left */
    GtkWidget *add_dialog;               /* the "add a pill" window, or NULL */
};

/* Which way each arrow button moves a pill, as a difference of layout
 * positions: one step sideways is +-1, one row up/down is +-columns. */
static const struct
{
    const gchar *icon;
    const gchar *tooltip;
    gint delta;
} arrow_specs[ARROW_COUNT] = {
    { "go-previous-symbolic", "Move left",  -1 },
    { "go-next-symbolic",     "Move right",  1 },
    { "go-up-symbolic",       "Move up",    -EXTRAS_MENU_PILL_COLUMNS },
    { "go-down-symbolic",     "Move down",   EXTRAS_MENU_PILL_COLUMNS },
};

/* Drag and drop payload: just the number of the cell the drag started
 * from, as text. Same-app only; nothing outside this dialog is a valid
 * source or target. */
static const GtkTargetEntry drag_targets[] = {
    { "extras-menu/pill-slot", GTK_TARGET_SAME_APP, 0 },
};

/* --- refreshing and saving ------------------------------------------------------ */

/* Whether the pill in cell `index` can step by `delta` and land in a
 * cell that currently holds a pill. A sideways step must stay in the
 * same row, or the "right" arrow of the last cell in a row would
 * silently jump to the start of the next one. */
static gboolean
can_move(const PageState *st, guint index, gint delta)
{
    gint target = (gint) index + delta;

    if (target < 0 || target >= (gint) st->layout->len)
        return FALSE;

    if (ABS(delta) < EXTRAS_MENU_PILL_COLUMNS)
        return (index / EXTRAS_MENU_PILL_COLUMNS) == ((guint) target / EXTRAS_MENU_PILL_COLUMNS);

    return TRUE;
}

static void
refresh(PageState *st)
{
    for (guint i = 0; i < EXTRAS_MENU_PILL_COUNT; i++)
    {
        Slot *slot = &st->slots[i];
        gboolean shown = i < st->layout->len;

        gtk_widget_set_visible(slot->root, shown);
        if (!shown)
            continue;

        const ExtrasMenuPillInfo *pill = g_ptr_array_index(st->layout, i);

        gtk_image_set_from_icon_name(GTK_IMAGE(slot->icon), pill->icon, GTK_ICON_SIZE_BUTTON);
        gtk_label_set_text(GTK_LABEL(slot->label), pill->title);

        for (guint a = 0; a < ARROW_COUNT; a++)
            gtk_widget_set_sensitive(slot->arrow_buttons[a], can_move(st, i, arrow_specs[a].delta));
    }

    gtk_widget_set_sensitive(st->add_button, st->layout->len < EXTRAS_MENU_PILL_COUNT);
    gtk_widget_set_visible(st->empty_label, st->layout->len == 0);
}

static void
save_layout(PageState *st)
{
    GError *error = NULL;

    if (!extras_menu_pill_layout_save(st->layout, &error))
    {
        g_warning("extras-menu: couldn't save the pill layout: %s",
                  error != NULL ? error->message : "unknown error");
        g_clear_error(&error);
    }
}

/* --- moving and removing --------------------------------------------------------- */

static void
swap_cells(PageState *st, guint a, guint b)
{
    if (a == b || a >= st->layout->len || b >= st->layout->len)
        return;

    gpointer tmp = st->layout->pdata[a];
    st->layout->pdata[a] = st->layout->pdata[b];
    st->layout->pdata[b] = tmp;

    refresh(st);
    save_layout(st);
}

static void
on_arrow_clicked(GtkButton *button, gpointer user_data)
{
    Slot *slot = user_data;
    gint delta = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(button), "delta"));

    if (can_move(slot->state, slot->index, delta))
        swap_cells(slot->state, slot->index, (guint) ((gint) slot->index + delta));
}

/* The pills after the removed one each move up one place, like the
 * rows of the quick-actions editor. The pill isn't lost: it is offered
 * again in the "add a pill" window. */
static void
on_remove_clicked(GtkButton *button, gpointer user_data)
{
    Slot *slot = user_data;
    PageState *st = slot->state;
    (void) button;

    if (slot->index >= st->layout->len)
        return;

    g_ptr_array_remove_index(st->layout, slot->index);

    refresh(st);
    save_layout(st);
}

static void
on_reset_clicked(GtkButton *button, gpointer user_data)
{
    PageState *st = user_data;
    (void) button;

    g_ptr_array_unref(st->layout);
    st->layout = extras_menu_pill_layout_defaults();

    refresh(st);
    save_layout(st);
}

/* --- pill shape ------------------------------------------------------------------- */

/* Saved at once rather than debounced: it's a single click. Only the
 * round radio is watched -- picking one radio un-picks the other, so
 * each choice would otherwise fire twice. */
static void
on_shape_toggled(GtkToggleButton *round_radio, gpointer user_data)
{
    (void) user_data;
    GError *error = NULL;

    ExtrasMenuPillShape shape = gtk_toggle_button_get_active(round_radio)
        ? EXTRAS_MENU_PILL_SHAPE_ROUND
        : EXTRAS_MENU_PILL_SHAPE_DEFAULT;

    if (!extras_menu_pill_layout_set_shape(shape, &error))
    {
        g_warning("extras-menu: couldn't save the pill shape: %s",
                  error != NULL ? error->message : "unknown error");
        g_clear_error(&error);
    }
}

static GtkWidget *
make_shape_row(void)
{
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);

    GtkWidget *label = gtk_label_new("Pill shape:");

    GtkWidget *default_radio = gtk_radio_button_new_with_label(NULL, "Default (soft corners)");
    GtkWidget *round_radio = gtk_radio_button_new_with_label_from_widget(
        GTK_RADIO_BUTTON(default_radio), "Round ends (GNOME style)");

    /* Set the current choice before connecting, so showing the page
     * doesn't count as a change. */
    if (extras_menu_pill_layout_get_shape() == EXTRAS_MENU_PILL_SHAPE_ROUND)
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(round_radio), TRUE);

    g_signal_connect(round_radio, "toggled", G_CALLBACK(on_shape_toggled), NULL);

    gtk_box_pack_start(GTK_BOX(row), label, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(row), default_radio, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(row), round_radio, FALSE, FALSE, 0);

    return row;
}

/* --- the "add a pill" window ------------------------------------------------------ */

typedef struct
{
    PageState *state;
    const ExtrasMenuPillInfo *pill;
} AddRowCtx;

/* GClosureNotify-shaped wrapper around g_free(), so the context lives
 * exactly as long as its button's "clicked" handler. */
static void
add_row_ctx_free(gpointer data, GClosure *closure)
{
    (void) closure;
    g_free(data);
}

/* The row stays in the window, switched to a disabled "Added", rather
 * than being destroyed: this runs inside the button's own click, and
 * it also lets several pills be added without reopening the window. */
static void
on_add_pill_clicked(GtkButton *button, gpointer user_data)
{
    AddRowCtx *ctx = user_data;
    PageState *st = ctx->state;

    if (st->layout->len < EXTRAS_MENU_PILL_COUNT &&
        !extras_menu_pill_layout_contains(st->layout, ctx->pill->id))
    {
        g_ptr_array_add(st->layout, (gpointer) ctx->pill);

        refresh(st);
        save_layout(st);
    }

    gtk_button_set_label(button, "Added");
    gtk_widget_set_sensitive(GTK_WIDGET(button), FALSE);
}

static void
on_add_dialog_response(GtkDialog *dialog, gint response_id, gpointer user_data)
{
    (void) response_id;
    (void) user_data;
    gtk_widget_destroy(GTK_WIDGET(dialog));
}

/* One available pill:  [icon]  Title            [Add]
 *                              what it does
 */
static GtkWidget *
build_add_row(PageState *st, const ExtrasMenuPillInfo *pill)
{
    GtkWidget *frame = gtk_frame_new(NULL);

    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_container_set_border_width(GTK_CONTAINER(row), 10);
    gtk_container_add(GTK_CONTAINER(frame), row);

    GtkWidget *icon = gtk_image_new_from_icon_name(pill->icon, GTK_ICON_SIZE_LARGE_TOOLBAR);
    gtk_box_pack_start(GTK_BOX(row), icon, FALSE, FALSE, 0);

    GtkWidget *text = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_widget_set_valign(text, GTK_ALIGN_CENTER);

    GtkWidget *title = gtk_label_new(NULL);
    gchar *markup = g_markup_printf_escaped("<b>%s</b>", pill->title);
    gtk_label_set_markup(GTK_LABEL(title), markup);
    g_free(markup);
    gtk_label_set_xalign(GTK_LABEL(title), 0.0);
    gtk_box_pack_start(GTK_BOX(text), title, FALSE, FALSE, 0);

    GtkWidget *description = gtk_label_new(pill->description);
    gtk_label_set_xalign(GTK_LABEL(description), 0.0);
    gtk_label_set_line_wrap(GTK_LABEL(description), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(description), 40);
    gtk_style_context_add_class(gtk_widget_get_style_context(description), "dim-label");
    gtk_box_pack_start(GTK_BOX(text), description, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(row), text, TRUE, TRUE, 0);

    GtkWidget *button = gtk_button_new_with_label("Add");
    gtk_widget_set_valign(button, GTK_ALIGN_CENTER);

    AddRowCtx *ctx = g_new0(AddRowCtx, 1);
    ctx->state = st;
    ctx->pill = pill;
    g_signal_connect_data(button, "clicked", G_CALLBACK(on_add_pill_clicked),
                          ctx, add_row_ctx_free, 0);

    gtk_box_pack_end(GTK_BOX(row), button, FALSE, FALSE, 0);

    return frame;
}

/* Opens the window listing every pill that isn't in the menu right now.
 * It is non-blocking (no gtk_dialog_run) and modal for the preferences
 * dialog, so the layout can't change underneath it except by its own
 * Add buttons. */
static void
on_add_clicked(GtkButton *button, gpointer user_data)
{
    PageState *st = user_data;

    if (st->add_dialog != NULL)
    {
        gtk_window_present(GTK_WINDOW(st->add_dialog));
        return;
    }

    GtkWidget *toplevel = gtk_widget_get_toplevel(GTK_WIDGET(button));

    GtkWidget *dialog = gtk_dialog_new_with_buttons(
        "Add a pill",
        GTK_IS_WINDOW(toplevel) ? GTK_WINDOW(toplevel) : NULL,
        GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
        "_Close", GTK_RESPONSE_CLOSE,
        NULL);

    /* Cleared automatically however the window goes away (Close, the
     * window manager, or the preferences dialog being destroyed). */
    st->add_dialog = dialog;
    g_signal_connect(dialog, "destroy", G_CALLBACK(gtk_widget_destroyed), &st->add_dialog);
    g_signal_connect(dialog, "response", G_CALLBACK(on_add_dialog_response), NULL);
    gtk_window_set_default_size(GTK_WINDOW(dialog), 440, -1);

    GtkWidget *content_area = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(box), 14);
    gtk_box_pack_start(GTK_BOX(content_area), box, TRUE, TRUE, 0);

    GtkWidget *intro = gtk_label_new(
        "Pills that exist but aren't in the menu right now. A pill you add "
        "goes in the next free spot; move it afterwards if you like.");
    gtk_label_set_line_wrap(GTK_LABEL(intro), TRUE);
    gtk_label_set_xalign(GTK_LABEL(intro), 0.0);
    gtk_label_set_max_width_chars(GTK_LABEL(intro), 52);
    gtk_style_context_add_class(gtk_widget_get_style_context(intro), "dim-label");
    gtk_box_pack_start(GTK_BOX(box), intro, FALSE, FALSE, 0);

    guint available = 0;

    for (guint i = 0; i < EXTRAS_MENU_PILL_COUNT; i++)
    {
        const ExtrasMenuPillInfo *pill = extras_menu_pill_catalog_get(i);

        if (extras_menu_pill_layout_contains(st->layout, pill->id))
            continue;

        gtk_box_pack_start(GTK_BOX(box), build_add_row(st, pill), FALSE, FALSE, 0);
        available++;
    }

    if (available == 0)
    {
        GtkWidget *none = gtk_label_new("Every pill is already in the menu.");
        gtk_label_set_xalign(GTK_LABEL(none), 0.0);
        gtk_box_pack_start(GTK_BOX(box), none, FALSE, FALSE, 0);
    }

    gtk_widget_show_all(dialog);
}

/* --- drag and drop -------------------------------------------------------------- */

static void
on_drag_begin(GtkWidget *widget, GdkDragContext *context, gpointer user_data)
{
    Slot *slot = user_data;
    (void) widget;

    if (slot->index >= slot->state->layout->len)
        return;

    const ExtrasMenuPillInfo *pill = g_ptr_array_index(slot->state->layout, slot->index);
    gtk_drag_set_icon_name(context, pill->icon, 0, 0);
}

static void
on_drag_data_get(GtkWidget *widget, GdkDragContext *context, GtkSelectionData *data,
                 guint info, guint time, gpointer user_data)
{
    Slot *slot = user_data;
    (void) widget;
    (void) context;
    (void) info;
    (void) time;

    gchar *text = g_strdup_printf("%u", slot->index);
    gtk_selection_data_set(data, gtk_selection_data_get_target(data), 8,
                           (const guchar *) text, (gint) strlen(text));
    g_free(text);
}

/* The cells are drag destinations with GTK_DEST_DEFAULT_ALL, so GTK
 * highlights the cell under the pointer, fetches the data when the drop
 * happens and finishes the drag itself; this only has to do the swap.
 * Hidden cells can't be dropped on, and swap_cells() ignores any index
 * that doesn't hold a pill. */
static void
on_drag_data_received(GtkWidget *widget, GdkDragContext *context, gint x, gint y,
                      GtkSelectionData *data, guint info, guint time, gpointer user_data)
{
    Slot *slot = user_data;
    const guchar *raw = gtk_selection_data_get_data(data);
    gint length = gtk_selection_data_get_length(data);
    (void) widget;
    (void) context;
    (void) x;
    (void) y;
    (void) info;
    (void) time;

    if (raw == NULL || length <= 0)
        return;

    gchar *text = g_strndup((const gchar *) raw, (gsize) length);
    guint64 source = g_ascii_strtoull(text, NULL, 10);
    g_free(text);

    if (source < EXTRAS_MENU_PILL_COUNT)
        swap_cells(slot->state, (guint) source, slot->index);
}

/* --- building the page -------------------------------------------------------- */

static GtkWidget *
build_slot(PageState *st, guint index)
{
    Slot *slot = &st->slots[index];
    slot->state = st;
    slot->index = index;

    /* An event box, so the whole cell (not just a child that happens to
     * have its own window) takes part in drag and drop. */
    GtkWidget *event_box = gtk_event_box_new();
    gtk_widget_set_hexpand(event_box, TRUE);
    slot->root = event_box;

    GtkWidget *frame = gtk_frame_new(NULL);
    gtk_container_add(GTK_CONTAINER(event_box), frame);

    GtkWidget *content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(content), 10);
    gtk_container_add(GTK_CONTAINER(frame), content);

    GtkWidget *title_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    slot->icon = gtk_image_new();
    slot->label = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(slot->label), 0.0);
    gtk_box_pack_start(GTK_BOX(title_row), slot->icon, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(title_row), slot->label, TRUE, TRUE, 0);

    GtkWidget *remove_button = gtk_button_new_from_icon_name("list-remove-symbolic", GTK_ICON_SIZE_BUTTON);
    gtk_button_set_relief(GTK_BUTTON(remove_button), GTK_RELIEF_NONE);
    gtk_widget_set_tooltip_text(remove_button, "Remove from the menu");
    g_signal_connect(remove_button, "clicked", G_CALLBACK(on_remove_clicked), slot);
    gtk_box_pack_end(GTK_BOX(title_row), remove_button, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(content), title_row, FALSE, FALSE, 0);

    /* The four arrows as one joined control. */
    GtkWidget *arrow_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_style_context_add_class(gtk_widget_get_style_context(arrow_row), "linked");
    gtk_widget_set_halign(arrow_row, GTK_ALIGN_START);

    for (guint a = 0; a < ARROW_COUNT; a++)
    {
        GtkWidget *button = gtk_button_new_from_icon_name(arrow_specs[a].icon, GTK_ICON_SIZE_BUTTON);
        gtk_widget_set_tooltip_text(button, arrow_specs[a].tooltip);
        g_object_set_data(G_OBJECT(button), "delta", GINT_TO_POINTER(arrow_specs[a].delta));
        g_signal_connect(button, "clicked", G_CALLBACK(on_arrow_clicked), slot);

        slot->arrow_buttons[a] = button;
        gtk_box_pack_start(GTK_BOX(arrow_row), button, FALSE, FALSE, 0);
    }

    gtk_box_pack_start(GTK_BOX(content), arrow_row, FALSE, FALSE, 0);

    gtk_drag_source_set(event_box, GDK_BUTTON1_MASK, drag_targets,
                        G_N_ELEMENTS(drag_targets), GDK_ACTION_MOVE);
    gtk_drag_dest_set(event_box, GTK_DEST_DEFAULT_ALL, drag_targets,
                      G_N_ELEMENTS(drag_targets), GDK_ACTION_MOVE);
    g_signal_connect(event_box, "drag-begin", G_CALLBACK(on_drag_begin), slot);
    g_signal_connect(event_box, "drag-data-get", G_CALLBACK(on_drag_data_get), slot);
    g_signal_connect(event_box, "drag-data-received", G_CALLBACK(on_drag_data_received), slot);

    /* Show the cell's insides once, now; from here on refresh() only
     * shows or hides the cell as a whole, and no_show_all keeps the
     * show_all() on the whole dialog from showing cells that stand for
     * pills not in the menu. */
    gtk_widget_show_all(event_box);
    gtk_widget_set_no_show_all(event_box, TRUE);

    return event_box;
}

static void
on_page_destroy(GtkWidget *page, gpointer user_data)
{
    PageState *st = user_data;
    (void) page;

    /* The add window's buttons point at `st`, so it must go first. (If
     * it is already being destroyed, the pointer was cleared.) */
    if (st->add_dialog != NULL)
        gtk_widget_destroy(st->add_dialog);

    g_ptr_array_unref(st->layout);
    g_free(st);
}

GtkWidget *
extras_menu_pill_layout_page_new(void)
{
    PageState *st = g_new0(PageState, 1);
    st->layout = extras_menu_pill_layout_load();

    GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);

    GtkWidget *heading = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(heading), "<b>Pill buttons</b>");
    gtk_label_set_xalign(GTK_LABEL(heading), 0.0);
    gtk_box_pack_start(GTK_BOX(page), heading, FALSE, FALSE, 0);

    GtkWidget *description = gtk_label_new(
        "The big buttons in the menu, laid out here exactly as they are "
        "in the dropdown. Drag a button onto another one to swap them, or "
        "use the arrows to move one a step. Remove the ones you don't "
        "want, and bring them back with Add pill. Changes apply "
        "immediately.");
    gtk_label_set_line_wrap(GTK_LABEL(description), TRUE);
    gtk_label_set_xalign(GTK_LABEL(description), 0.0);
    gtk_label_set_max_width_chars(GTK_LABEL(description), 60);
    gtk_style_context_add_class(gtk_widget_get_style_context(description), "dim-label");
    gtk_box_pack_start(GTK_BOX(page), description, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(page), make_shape_row(), FALSE, FALSE, 0);

    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 8);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 8);
    gtk_grid_set_column_homogeneous(GTK_GRID(grid), TRUE);

    for (guint i = 0; i < EXTRAS_MENU_PILL_COUNT; i++)
    {
        gtk_grid_attach(GTK_GRID(grid), build_slot(st, i),
                        (gint) (i % EXTRAS_MENU_PILL_COLUMNS),
                        (gint) (i / EXTRAS_MENU_PILL_COLUMNS), 1, 1);
    }

    gtk_box_pack_start(GTK_BOX(page), grid, FALSE, FALSE, 0);

    st->empty_label = gtk_label_new("No pills -- the menu will only show the sliders.");
    gtk_label_set_xalign(GTK_LABEL(st->empty_label), 0.0);
    gtk_widget_set_no_show_all(st->empty_label, TRUE); /* refresh() decides */
    gtk_box_pack_start(GTK_BOX(page), st->empty_label, FALSE, FALSE, 0);

    GtkWidget *footer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);

    st->add_button = gtk_button_new_with_label("Add pill…");
    gtk_button_set_image(GTK_BUTTON(st->add_button),
                         gtk_image_new_from_icon_name("list-add-symbolic", GTK_ICON_SIZE_BUTTON));
    gtk_button_set_always_show_image(GTK_BUTTON(st->add_button), TRUE);
    g_signal_connect(st->add_button, "clicked", G_CALLBACK(on_add_clicked), st);

    GtkWidget *reset_button = gtk_button_new_with_label("Reset to defaults");
    g_signal_connect(reset_button, "clicked", G_CALLBACK(on_reset_clicked), st);

    gtk_box_pack_start(GTK_BOX(footer), st->add_button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(footer), reset_button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(page), footer, FALSE, FALSE, 0);

    g_signal_connect(page, "destroy", G_CALLBACK(on_page_destroy), st);

    refresh(st);

    return page;
}