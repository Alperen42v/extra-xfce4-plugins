/*
 * Custom icon loading for the Caffeine plugin.
 *
 * Kept separate from caffeine.c on purpose, same reasoning as
 * caffeine-prefs.c: this is a self-contained concern (find files on
 * disk, load/scale them, hand back pixbufs) that shouldn't clutter the
 * inhibit/drawing/lifecycle code.
 */

#include <gio/gio.h>
#include <gtk/gtk.h>

#include "caffeine-icons.h"

#define MAX_ON_FRAMES 999  /* sanity cap, matches the "on-999.png" width below */

gchar *
caffeine_icons_get_folder (void)
{
    return g_build_filename (g_get_home_dir (), ".config",
                              "xfce4-caffeine-plugin", "icons", NULL);
}

CaffeineIconTheme
caffeine_icons_resolve_theme (CaffeineIconTheme theme)
{
    GtkSettings *settings;
    gboolean     prefer_dark = FALSE;

    if (theme != CAFFEINE_ICON_THEME_AUTO)
        return theme;

    /* Same property GTK's own theme switcher sets, so this follows
     * whatever the user's system/GTK theme is currently doing rather
     * than trying to guess from colours or a desktop-specific API. */
    settings = gtk_settings_get_default ();
    if (settings != NULL)
        g_object_get (settings, "gtk-application-prefer-dark-theme", &prefer_dark, NULL);

    return prefer_dark ? CAFFEINE_ICON_THEME_DARK : CAFFEINE_ICON_THEME_LIGHT;
}

/* Loads one PNG, scaled to size x size, or NULL if it doesn't exist / fails
 * to load (logged at debug level only - a missing custom icon is a normal,
 * expected case, not a warning-worthy error). */
static GdkPixbuf *
load_scaled_png (const gchar *path, gint size)
{
    GError    *error = NULL;
    GdkPixbuf *pixbuf;

    if (!g_file_test (path, G_FILE_TEST_IS_REGULAR))
        return NULL;

    pixbuf = gdk_pixbuf_new_from_file_at_scale (path, size, size,
                                                 FALSE /* don't preserve aspect ratio -
                                                          force exact panel size */,
                                                 &error);
    if (pixbuf == NULL)
    {
        g_debug ("Caffeine: could not load icon '%s': %s", path,
                  error ? error->message : "unknown error");
        g_clear_error (&error);
        return NULL;
    }

    return pixbuf;
}

CaffeineIconSet *
caffeine_icons_load (gint target_size, CaffeineIconTheme theme)
{
    CaffeineIconSet   *icons;
    gchar             *folder;
    GPtrArray         *frames;
    gint               n;
    CaffeineIconTheme  resolved;
    const gchar       *variant;

    /* AUTO must already be resolved to LIGHT/DARK by the caller via
     * caffeine_icons_resolve_theme(), but resolve again here too so this
     * function is safe to call directly with AUTO as well. */
    resolved = caffeine_icons_resolve_theme (theme);
    variant = (resolved == CAFFEINE_ICON_THEME_DARK) ? "dark" : "light";

    icons = g_new0 (CaffeineIconSet, 1);
    folder = caffeine_icons_get_folder ();

    /* OFF: single static frame, e.g. off-light.png / off-dark.png */
    {
        gchar *off_name = g_strdup_printf ("off-%s.png", variant);
        gchar *off_path = g_build_filename (folder, off_name, NULL);
        icons->off_frame = load_scaled_png (off_path, target_size);
        g_free (off_name);
        g_free (off_path);
    }

    /* ON: on-light-01.png, on-light-02.png, ... (or on-dark-*) - stop at
     * the first gap */
    frames = g_ptr_array_new ();
    for (n = 1; n <= MAX_ON_FRAMES; n++)
    {
        gchar     *name = g_strdup_printf ("on-%s-%02d.png", variant, n);
        gchar     *path = g_build_filename (folder, name, NULL);
        GdkPixbuf *frame = load_scaled_png (path, target_size);

        g_free (name);
        g_free (path);

        if (frame == NULL)
            break;

        g_ptr_array_add (frames, frame);
    }

    if (frames->len > 0)
    {
        icons->on_frame_count = frames->len;
        icons->on_frames = (GdkPixbuf **) g_ptr_array_free (frames, FALSE);
    }
    else
    {
        icons->on_frame_count = 0;
        icons->on_frames = NULL;
        g_ptr_array_free (frames, TRUE);
    }

    g_free (folder);
    return icons;
}

void
caffeine_icon_set_free (CaffeineIconSet *icons)
{
    guint i;

    if (icons == NULL)
        return;

    if (icons->off_frame != NULL)
        g_object_unref (icons->off_frame);

    for (i = 0; i < icons->on_frame_count; i++)
        g_object_unref (icons->on_frames[i]);
    g_free (icons->on_frames);

    g_free (icons);
}