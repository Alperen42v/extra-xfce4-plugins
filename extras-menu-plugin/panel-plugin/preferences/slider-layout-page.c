#include "slider-layout-page.h"
#include "slider-layout-config.h"

/*
 * The page has EXTRAS_MENU_SLIDER_COUNT fixed rows. Row i shows
 * whichever slider sits at position i of the layout, and only the first
 * `layout->len` rows are visible: the rest stand for sliders that aren't
 * in the menu. Nothing is created or destroyed after the page is built:
 * moving, removing or adding a slider edits the layout and then
 * refresh() rewrites every row and shows/hides the rows. (Same scheme as
 * the pill editor; see pill-layout-page.c.)
 */

typedef struct _PageState PageState;

typedef struct
{
    PageState *state;
    guint index;                         /* this row's position in the layout */
    GtkWidget *root;                     /* the whole row, shown/hidden by refresh() */
    GtkWidget *icon;
    GtkWidget *label;
    GtkWidget *up_button;
    GtkWidget *down_button;
} Row;

struct _PageState
{
    GPtrArray *layout;                   /* const ExtrasMenuSliderInfo*, one per shown slider; not owned */
    Row rows[EXTRAS_MENU_SLIDER_COUNT];
    GtkWidget *add_button;
    GtkWidget *empty_label;              /* shown instead of the rows when no slider is left */
    GtkWidget *add_dialog;               /* the "add a slider" window, or NULL */
};

/* --- refreshing and saving ------------------------------------------------------ */

static void
refresh(PageState *st)
{
    for (guint i = 0; i < EXTRAS_MENU_SLIDER_COUNT; i++)
    {
        Row *row = &st->rows[i];
        gboolean shown = i < st->layout->len;

        gtk_widget_set_visible(row->root, shown);
        if (!shown)
            continue;

        const ExtrasMenuSliderInfo *slider = g_ptr_array_index(st->layout, i);

        gtk_image_set_from_icon_name(GTK_IMAGE(row->icon), slider->icon, GTK_ICON_SIZE_BUTTON);
        gtk_label_set_text(GTK_LABEL(row->label), slider->title);

        gtk_widget_set_sensitive(row->up_button, i > 0);
        gtk_widget_set_sensitive(row->down_button, i + 1 < st->layout->len);
    }

    gtk_widget_set_sensitive(st->add_button, st->layout->len < EXTRAS_MENU_SLIDER_COUNT);
    gtk_widget_set_visible(st->empty_label, st->layout->len == 0);
}

static void
save_layout(PageState *st)
{
    GError *error = NULL;

    if (!extras_menu_slider_layout_save(st->layout, &error))
    {
        g_warning("extras-menu: couldn't save the slider layout: %s",
                  error != NULL ? error->message : "unknown error");
        g_clear_error(&error);
    }
}

/* --- moving and removing --------------------------------------------------------- */

static void
swap_rows(PageState *st, guint a, guint b)
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
on_up_clicked(GtkButton *button, gpointer user_data)
{
    Row *row = user_data;
    (void) button;

    if (row->index > 0)
        swap_rows(row->state, row->index, row->index - 1);
}

static void
on_down_clicked(GtkButton *button, gpointer user_data)
{
    Row *row = user_data;
    (void) button;

    swap_rows(row->state, row->index, row->index + 1);
}

/* The sliders below the removed one each move up one place. The slider
 * isn't lost: it is offered again in the "add a slider" window. */
static void
on_remove_clicked(GtkButton *button, gpointer user_data)
{
    Row *row = user_data;
    PageState *st = row->state;
    (void) button;

    if (row->index >= st->layout->len)
        return;

    g_ptr_array_remove_index(st->layout, row->index);

    refresh(st);
    save_layout(st);
}

static void
on_reset_clicked(GtkButton *button, gpointer user_data)
{
    PageState *st = user_data;
    (void) button;

    g_ptr_array_unref(st->layout);
    st->layout = extras_menu_slider_layout_defaults();

    refresh(st);
    save_layout(st);
}

/* --- the "add a slider" window ----------------------------------------------------- */

typedef struct
{
    PageState *state;
    const ExtrasMenuSliderInfo *slider;
} AddRowCtx;

/* GClosureNotify-shaped wrapper around g_free(), so the context lives
 * exactly as long as its button's "clicked" handler. */
static void
add_row_ctx_free(gpointer data, GClosure *closure)
{
    (void) closure;
    g_free(data);
}

/* The entry stays in the window, switched to a disabled "Added", rather
 * than being destroyed: this runs inside the button's own click, and it
 * also lets several sliders be added without reopening the window. */
static void
on_add_slider_clicked(GtkButton *button, gpointer user_data)
{
    AddRowCtx *ctx = user_data;
    PageState *st = ctx->state;

    if (st->layout->len < EXTRAS_MENU_SLIDER_COUNT &&
        !extras_menu_slider_layout_contains(st->layout, ctx->slider->id))
    {
        g_ptr_array_add(st->layout, (gpointer) ctx->slider);

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

/* One available slider:  [icon]  Title            [Add]
 *                                what it does
 */
static GtkWidget *
build_add_entry(PageState *st, const ExtrasMenuSliderInfo *slider)
{
    GtkWidget *frame = gtk_frame_new(NULL);

    GtkWidget *entry = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_container_set_border_width(GTK_CONTAINER(entry), 10);
    gtk_container_add(GTK_CONTAINER(frame), entry);

    GtkWidget *icon = gtk_image_new_from_icon_name(slider->icon, GTK_ICON_SIZE_LARGE_TOOLBAR);
    gtk_box_pack_start(GTK_BOX(entry), icon, FALSE, FALSE, 0);

    GtkWidget *text = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_widget_set_valign(text, GTK_ALIGN_CENTER);

    GtkWidget *title = gtk_label_new(NULL);
    gchar *markup = g_markup_printf_escaped("<b>%s</b>", slider->title);
    gtk_label_set_markup(GTK_LABEL(title), markup);
    g_free(markup);
    gtk_label_set_xalign(GTK_LABEL(title), 0.0);
    gtk_box_pack_start(GTK_BOX(text), title, FALSE, FALSE, 0);

    GtkWidget *description = gtk_label_new(slider->description);
    gtk_label_set_xalign(GTK_LABEL(description), 0.0);
    gtk_label_set_line_wrap(GTK_LABEL(description), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(description), 40);
    gtk_style_context_add_class(gtk_widget_get_style_context(description), "dim-label");
    gtk_box_pack_start(GTK_BOX(text), description, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(entry), text, TRUE, TRUE, 0);

    GtkWidget *button = gtk_button_new_with_label("Add");
    gtk_widget_set_valign(button, GTK_ALIGN_CENTER);

    AddRowCtx *ctx = g_new0(AddRowCtx, 1);
    ctx->state = st;
    ctx->slider = slider;
    g_signal_connect_data(button, "clicked", G_CALLBACK(on_add_slider_clicked),
                          ctx, add_row_ctx_free, 0);

    gtk_box_pack_end(GTK_BOX(entry), button, FALSE, FALSE, 0);

    return frame;
}

/* Opens the window listing every slider that isn't in the menu right
 * now. It is non-blocking (no gtk_dialog_run) and modal for the
 * preferences dialog, so the layout can't change underneath it except
 * by its own Add buttons. */
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
        "Add a slider",
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
        "Sliders that exist but aren't in the menu right now. A slider you "
        "add goes at the bottom; move it afterwards if you like.");
    gtk_label_set_line_wrap(GTK_LABEL(intro), TRUE);
    gtk_label_set_xalign(GTK_LABEL(intro), 0.0);
    gtk_label_set_max_width_chars(GTK_LABEL(intro), 52);
    gtk_style_context_add_class(gtk_widget_get_style_context(intro), "dim-label");
    gtk_box_pack_start(GTK_BOX(box), intro, FALSE, FALSE, 0);

    guint available = 0;

    for (guint i = 0; i < EXTRAS_MENU_SLIDER_COUNT; i++)
    {
        const ExtrasMenuSliderInfo *slider = extras_menu_slider_catalog_get(i);

        if (extras_menu_slider_layout_contains(st->layout, slider->id))
            continue;

        gtk_box_pack_start(GTK_BOX(box), build_add_entry(st, slider), FALSE, FALSE, 0);
        available++;
    }

    if (available == 0)
    {
        GtkWidget *none = gtk_label_new("Every slider is already in the menu.");
        gtk_label_set_xalign(GTK_LABEL(none), 0.0);
        gtk_box_pack_start(GTK_BOX(box), none, FALSE, FALSE, 0);
    }

    gtk_widget_show_all(dialog);
}

/* --- building the page -------------------------------------------------------- */

static GtkWidget *
build_row(PageState *st, guint index)
{
    Row *row = &st->rows[index];
    row->state = st;
    row->index = index;

    GtkWidget *frame = gtk_frame_new(NULL);
    row->root = frame;

    GtkWidget *content = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(content), 10);
    gtk_container_add(GTK_CONTAINER(frame), content);

    row->icon = gtk_image_new();
    row->label = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(row->label), 0.0);
    gtk_box_pack_start(GTK_BOX(content), row->icon, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(content), row->label, TRUE, TRUE, 0);

    /* Up and down as one joined control, then remove on its own. */
    GtkWidget *arrows = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_style_context_add_class(gtk_widget_get_style_context(arrows), "linked");

    row->up_button = gtk_button_new_from_icon_name("go-up-symbolic", GTK_ICON_SIZE_BUTTON);
    gtk_widget_set_tooltip_text(row->up_button, "Move up");
    g_signal_connect(row->up_button, "clicked", G_CALLBACK(on_up_clicked), row);

    row->down_button = gtk_button_new_from_icon_name("go-down-symbolic", GTK_ICON_SIZE_BUTTON);
    gtk_widget_set_tooltip_text(row->down_button, "Move down");
    g_signal_connect(row->down_button, "clicked", G_CALLBACK(on_down_clicked), row);

    gtk_box_pack_start(GTK_BOX(arrows), row->up_button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(arrows), row->down_button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(content), arrows, FALSE, FALSE, 0);

    GtkWidget *remove_button = gtk_button_new_from_icon_name("list-remove-symbolic", GTK_ICON_SIZE_BUTTON);
    gtk_button_set_relief(GTK_BUTTON(remove_button), GTK_RELIEF_NONE);
    gtk_widget_set_tooltip_text(remove_button, "Remove from the menu");
    g_signal_connect(remove_button, "clicked", G_CALLBACK(on_remove_clicked), row);
    gtk_box_pack_start(GTK_BOX(content), remove_button, FALSE, FALSE, 0);

    /* Show the row's insides once, now; from here on refresh() only
     * shows or hides the row as a whole, and no_show_all keeps the
     * show_all() on the whole dialog from showing rows that stand for
     * sliders not in the menu. */
    gtk_widget_show_all(frame);
    gtk_widget_set_no_show_all(frame, TRUE);

    return frame;
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
extras_menu_slider_layout_page_new(void)
{
    PageState *st = g_new0(PageState, 1);
    st->layout = extras_menu_slider_layout_load();

    GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);

    GtkWidget *heading = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(heading), "<b>Sliders</b>");
    gtk_label_set_xalign(GTK_LABEL(heading), 0.0);
    gtk_box_pack_start(GTK_BOX(page), heading, FALSE, FALSE, 0);

    GtkWidget *description = gtk_label_new(
        "The long sliders at the top of the menu (volume, brightness, microphone), in "
        "the order they appear there. Use the arrows to move one up or "
        "down, remove the ones you don't want, and bring them back with "
        "Add slider. Changes apply immediately.");
    gtk_label_set_line_wrap(GTK_LABEL(description), TRUE);
    gtk_label_set_xalign(GTK_LABEL(description), 0.0);
    gtk_label_set_max_width_chars(GTK_LABEL(description), 60);
    gtk_style_context_add_class(gtk_widget_get_style_context(description), "dim-label");
    gtk_box_pack_start(GTK_BOX(page), description, FALSE, FALSE, 0);

    GtkWidget *rows = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    for (guint i = 0; i < EXTRAS_MENU_SLIDER_COUNT; i++)
        gtk_box_pack_start(GTK_BOX(rows), build_row(st, i), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(page), rows, FALSE, FALSE, 0);

    st->empty_label = gtk_label_new("No sliders -- the menu will show no volume, brightness or microphone bar.");
    gtk_label_set_xalign(GTK_LABEL(st->empty_label), 0.0);
    gtk_widget_set_no_show_all(st->empty_label, TRUE); /* refresh() decides */
    gtk_box_pack_start(GTK_BOX(page), st->empty_label, FALSE, FALSE, 0);

    GtkWidget *footer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);

    st->add_button = gtk_button_new_with_label("Add slider…");
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