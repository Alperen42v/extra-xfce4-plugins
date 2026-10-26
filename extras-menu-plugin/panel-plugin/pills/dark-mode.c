/*
 * The Dark Mode pill: shows whether the GTK theme is a dark one and
 * switches between a theme's dark and light version. See dark-mode.h.
 *
 * The theme name lives in Xfce's settings, channel "xsettings", property
 * /Net/ThemeName -- what the Appearance dialog edits. xfsettingsd turns a
 * change of it into a theme change in every running application, so all
 * this module has to do is read and write that one string.
 */

#include "pills/dark-mode.h"

#include <string.h>
#include <xfconf/xfconf.h>

#define THEME_CHANNEL  "xsettings"
#define THEME_PROPERTY "/Net/ThemeName"

/* Where GTK looks for built-in themes (compiled into the library). */
#define BUILTIN_THEMES_RESOURCE "/org/gtk/libgtk/theme"

struct _ExtrasMenuDarkMode
{
    GtkWidget *pill; /* holds a reference */
    gulong clicked_id;

    XfconfChannel *channel; /* owned by xfconf; NULL if it couldn't start */
    gboolean xfconf_ready;  /* xfconf_init() succeeded: xfconf_shutdown() owed */

    ExtrasMenuDarkModeMessageFunc message_func;
    gpointer user_data;
};

/* --- reading theme names -------------------------------------------------------- */

static gboolean
is_separator(gchar c)
{
    return c == '-' || c == '_' || c == ':' || c == ' ';
}

/* If `name` ends in `word` (ignoring case) as a word of its own -- after
 * a separator, or starting a new capital ("NordDark") -- returns what is
 * in front of it, without the separators in between ("Adwaita-dark" and
 * "dark" give "Adwaita"). Otherwise NULL. This keeps "Twilight" from
 * being read as "Twi" + "light". */
static gchar *
strip_suffix_word(const gchar *name, const gchar *word)
{
    gsize length = strlen(name);
    gsize word_length = strlen(word);

    if (length < word_length || g_ascii_strcasecmp(name + length - word_length, word) != 0)
        return NULL;

    gsize base_length = length - word_length;
    if (base_length > 0 && !is_separator(name[base_length - 1]) && !g_ascii_isupper(name[base_length]))
        return NULL;

    while (base_length > 0 && is_separator(name[base_length - 1]))
        base_length--;

    return g_strndup(name, base_length);
}

/* For a dark theme's name: the name without its "dark"/"darker" ending.
 * NULL if the name doesn't end that way, i.e. isn't a dark theme. */
static gchar *
dark_base(const gchar *name)
{
    gchar *base = strip_suffix_word(name, "darker");

    return base != NULL ? base : strip_suffix_word(name, "dark");
}

static gboolean
is_dark_theme(const gchar *name)
{
    if (name == NULL)
        return FALSE;

    gchar *base = dark_base(name);
    gboolean dark = (base != NULL);

    g_free(base);
    return dark;
}

/* --- finding installed themes ---------------------------------------------------- */

/* Directories GTK (and Xfce's Appearance dialog) look for themes in. */
static GPtrArray *
theme_dirs(void)
{
    GPtrArray *dirs = g_ptr_array_new_with_free_func(g_free);

    g_ptr_array_add(dirs, g_build_filename(g_get_home_dir(), ".themes", NULL));
    g_ptr_array_add(dirs, g_build_filename(g_get_user_data_dir(), "themes", NULL));
    for (const gchar *const *d = g_get_system_data_dirs(); *d != NULL; d++)
        g_ptr_array_add(dirs, g_build_filename(*d, "themes", NULL));

    return dirs;
}

/* Whether the theme directory has a GTK 3 theme in it: a gtk-3.* folder
 * holding gtk.css or gtk.gresource. */
static gboolean
dir_has_gtk3_theme(const gchar *theme_path)
{
    GDir *dir = g_dir_open(theme_path, 0, NULL);
    gboolean found = FALSE;
    const gchar *entry;

    if (dir == NULL)
        return FALSE;

    while (!found && (entry = g_dir_read_name(dir)) != NULL)
    {
        if (!g_str_has_prefix(entry, "gtk-3."))
            continue;

        gchar *css = g_build_filename(theme_path, entry, "gtk.css", NULL);
        gchar *gresource = g_build_filename(theme_path, entry, "gtk.gresource", NULL);

        found = g_file_test(css, G_FILE_TEST_IS_REGULAR) ||
                g_file_test(gresource, G_FILE_TEST_IS_REGULAR);

        g_free(css);
        g_free(gresource);
    }

    g_dir_close(dir);
    return found;
}

/* The installed theme called `wanted` (ignoring case), spelled the way it
 * really is, or NULL. Themes GTK carries inside itself ("Adwaita") have
 * no folder anywhere, so those are looked up first the way GTK finds
 * them -- without that, the most common pair, Adwaita-dark and Adwaita,
 * would wrongly report the light half as missing. */
static gchar *
find_installed_theme(const gchar *wanted)
{
    if (wanted == NULL || *wanted == '\0')
        return NULL;

    gchar **builtin = g_resources_enumerate_children(BUILTIN_THEMES_RESOURCE,
                                                     G_RESOURCE_LOOKUP_FLAGS_NONE, NULL);
    if (builtin != NULL)
    {
        for (guint i = 0; builtin[i] != NULL; i++)
        {
            gchar *name = g_strdup(builtin[i]);
            gsize length = strlen(name);

            if (length > 0 && name[length - 1] == '/')
                name[length - 1] = '\0';

            if (g_ascii_strcasecmp(name, wanted) == 0)
            {
                g_strfreev(builtin);
                return name;
            }
            g_free(name);
        }
        g_strfreev(builtin);
    }

    gchar *result = NULL;
    GPtrArray *dirs = theme_dirs();

    for (guint i = 0; result == NULL && i < dirs->len; i++)
    {
        const gchar *base = g_ptr_array_index(dirs, i);
        GDir *dir = g_dir_open(base, 0, NULL);
        const gchar *entry;

        if (dir == NULL)
            continue;

        while (result == NULL && (entry = g_dir_read_name(dir)) != NULL)
        {
            if (g_ascii_strcasecmp(entry, wanted) != 0)
                continue;

            gchar *path = g_build_filename(base, entry, NULL);
            if (dir_has_gtk3_theme(path))
                result = g_strdup(entry);
            g_free(path);
        }

        g_dir_close(dir);
    }

    g_ptr_array_unref(dirs);
    return result;
}

static const gchar *const SEPARATORS[] = { "-", "_", " ", NULL };

/* The first installed theme named `base` + a separator + one of `words`
 * (words in order of preference), or, with `try_plain`, failing that
 * `base` on its own. NULL if none exists. */
static gchar *
find_variant(const gchar *base, const gchar *const *words, gboolean try_plain)
{
    for (guint w = 0; words[w] != NULL; w++)
    {
        for (guint s = 0; SEPARATORS[s] != NULL; s++)
        {
            gchar *candidate = g_strconcat(base, SEPARATORS[s], words[w], NULL);
            gchar *found = find_installed_theme(candidate);

            g_free(candidate);
            if (found != NULL)
                return found;
        }
    }

    return try_plain ? find_installed_theme(base) : NULL;
}

/* The light version of a dark theme: "<base>-light", or just "<base>"
 * ("Adwaita-dark" -> "Adwaita"). NULL if there is none installed. */
static gchar *
find_light_counterpart(const gchar *dark_name)
{
    static const gchar *const words[] = { "light", NULL };
    gchar *base = dark_base(dark_name);

    if (base == NULL)
        return NULL;

    gchar *found = find_variant(base, words, TRUE);
    g_free(base);
    return found;
}

/* The dark version of a light theme: "<base>-dark" (or "-darker"), where
 * base is the name without a "-light" ending, if it has one. NULL if
 * there is none installed. */
static gchar *
find_dark_counterpart(const gchar *light_name)
{
    static const gchar *const words[] = { "dark", "darker", NULL };
    gchar *base = strip_suffix_word(light_name, "light");

    if (base == NULL)
        base = g_strdup(light_name);

    gchar *found = find_variant(base, words, FALSE);
    g_free(base);
    return found;
}

/* --- the pill ---------------------------------------------------------------------- */

static void
show_message(ExtrasMenuDarkMode *self, const gchar *primary, const gchar *secondary)
{
    if (self->message_func != NULL)
        self->message_func(primary, secondary, self->user_data);
    else
        g_warning("extras-menu: %s %s", primary, secondary != NULL ? secondary : "");
}

/* The theme in use (free with g_free()), or NULL if it can't be told.
 * Xfce's setting is the truth; the toolkit's own idea of it is the
 * fallback if the setting was never written. */
static gchar *
current_theme_name(ExtrasMenuDarkMode *self)
{
    gchar *name = NULL;

    if (self->channel != NULL)
        name = xfconf_channel_get_string(self->channel, THEME_PROPERTY, NULL);

    if (name == NULL || *name == '\0')
    {
        g_free(name);
        name = NULL;
        g_object_get(gtk_settings_get_default(), "gtk-theme-name", &name, NULL);
    }

    return name;
}

/* Makes the pill show the current theme: lit for a dark one. Also what
 * undoes the flip a click gives a toggle button. */
static void
refresh(ExtrasMenuDarkMode *self)
{
    gchar *name = current_theme_name(self);
    gboolean dark = is_dark_theme(name);

    /* GTK3's gtk_toggle_button_set_active() emits "clicked" too, exactly
     * as a real click does, so our own handler must not hear this one
     * (it would switch the theme again). */
    g_signal_handler_block(self->pill, self->clicked_id);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(self->pill), dark);
    g_signal_handler_unblock(self->pill, self->clicked_id);

    gtk_widget_set_sensitive(self->pill, self->channel != NULL);

    if (self->channel == NULL)
    {
        gtk_widget_set_tooltip_text(self->pill,
                                    "Dark Mode unavailable\n"
                                    "(Xfce's settings service is not running)");
    }
    else if (name != NULL)
    {
        gchar *tooltip = g_strdup_printf("Theme: %s\nClick to switch to the %s version",
                                         name, dark ? "light" : "dark");
        gtk_widget_set_tooltip_text(self->pill, tooltip);
        g_free(tooltip);
    }
    else
    {
        gtk_widget_set_tooltip_text(self->pill, "Dark Mode: the current theme is unknown");
    }

    g_free(name);
}

static void
on_pill_clicked(GtkButton *button, gpointer user_data)
{
    (void) button;
    ExtrasMenuDarkMode *self = user_data;

    /* The click flipped the toggle button; the pill only ever changes
     * when the theme really does (see on_property_changed()). */
    refresh(self);

    if (self->channel == NULL)
        return;

    gchar *name = current_theme_name(self);
    if (name == NULL)
    {
        show_message(self, "Can't tell which theme is in use",
                     "Choose a theme in Settings > Appearance first.");
        return;
    }

    gboolean dark = is_dark_theme(name);
    gchar *target = dark ? find_light_counterpart(name) : find_dark_counterpart(name);

    if (target == NULL)
    {
        gchar *secondary = g_strdup_printf(
            "The theme \"%s\" has no %s version installed, so there is nothing "
            "to switch to. Install one, or choose a theme in Settings > Appearance.",
            name, dark ? "light" : "dark");

        show_message(self, dark ? "No light version of this theme"
                                : "No dark version of this theme",
                     secondary);
        g_free(secondary);
    }
    else if (g_strcmp0(target, name) != 0)
    {
        if (!xfconf_channel_set_string(self->channel, THEME_PROPERTY, target))
            show_message(self, "Could not change the theme", NULL);
    }

    g_free(target);
    g_free(name);
}

/* The theme setting changed -- by this pill or by the Appearance dialog. */
static void
on_property_changed(XfconfChannel *channel, const gchar *property, const GValue *value,
                    gpointer user_data)
{
    (void) channel;
    (void) value;

    if (g_strcmp0(property, THEME_PROPERTY) == 0)
        refresh(user_data);
}

/* --- setup and teardown --------------------------------------------------------------- */

ExtrasMenuDarkMode *
extras_menu_dark_mode_new(GtkWidget *pill, ExtrasMenuDarkModeMessageFunc message_func,
                          gpointer user_data)
{
    if (pill == NULL || !GTK_IS_TOGGLE_BUTTON(pill))
        return NULL;

    ExtrasMenuDarkMode *self = g_new0(ExtrasMenuDarkMode, 1);
    self->pill = g_object_ref(pill);
    self->message_func = message_func;
    self->user_data = user_data;

    GError *error = NULL;
    if (xfconf_init(&error))
    {
        self->xfconf_ready = TRUE;
        self->channel = xfconf_channel_get(THEME_CHANNEL);
        g_signal_connect(self->channel, "property-changed",
                         G_CALLBACK(on_property_changed), self);
    }
    else
    {
        g_warning("extras-menu: Dark Mode needs Xfce's settings service: %s",
                  error != NULL ? error->message : "unknown error");
        g_clear_error(&error);
    }

    /* Clicking switches the theme, so what the toggle does by itself is
     * not wanted: only "clicked" is used, and refresh() sets the look. */
    self->clicked_id = g_signal_connect(pill, "clicked", G_CALLBACK(on_pill_clicked), self);

    refresh(self);
    return self;
}

void
extras_menu_dark_mode_free(ExtrasMenuDarkMode *self)
{
    if (self == NULL)
        return;

    g_signal_handler_disconnect(self->pill, self->clicked_id);
    g_object_unref(self->pill);

    if (self->channel != NULL)
        g_signal_handlers_disconnect_by_data(self->channel, self);
    if (self->xfconf_ready)
        xfconf_shutdown();

    g_free(self);
}