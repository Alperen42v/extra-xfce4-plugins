#ifndef __CAFFEINE_ICONS_H__
#define __CAFFEINE_ICONS_H__

#include <gdk-pixbuf/gdk-pixbuf.h>
#include <glib.h>

G_BEGIN_DECLS

/*
 * Which custom icon variant to load - AUTO detects light vs dark from
 * the current GTK theme (see caffeine_icons_resolve_theme()); LIGHT/DARK
 * pin it regardless of the system theme.
 */
typedef enum
{
    CAFFEINE_ICON_THEME_AUTO = 0,
    CAFFEINE_ICON_THEME_LIGHT = 1,
    CAFFEINE_ICON_THEME_DARK = 2
} CaffeineIconTheme;

/*
 * Custom icon support: users can drop PNGs into a fixed folder and the
 * plugin picks them up automatically. Missing files fall back to the
 * built-in Cairo-drawn cup.
 *
 * Layout expected under the icon folder (see caffeine_icons_get_folder()):
 *   off-light.png     - OFF state, light-coloured icon (for dark panels/themes)
 *   off-dark.png       - OFF state, dark-coloured icon (for light panels/themes)
 *   on-light-01.png
 *   on-light-02.png
 *   ...                - ON state, light-coloured icon, played in sequence
 *                        and looped (on-light-01.png alone is fine too -
 *                        it just won't animate)
 *   on-dark-01.png
 *   on-dark-02.png
 *   ...                - ON state, dark-coloured icon, same rules as above
 *
 * Frame numbers must be zero-padded and consecutive starting at 01 with
 * no gaps; loading stops at the first missing number.
 *
 * Filenames describe the icon's own colour, not the system theme: a
 * dark system theme needs the light-coloured icon to stay visible, so
 * "dark system theme" maps to loading the "-light" files, and vice versa.
 */

typedef struct
{
    GdkPixbuf  *off_frame;      /* NULL if the off-*.png variant wasn't found/loadable */
    GdkPixbuf **on_frames;      /* NULL if no on-*-NN.png frames were found */
    guint       on_frame_count; /* 0 if on_frames is NULL */
} CaffeineIconSet;

/* Returns the icon folder path, newly-allocated (caller must g_free()).
 * Currently "~/.config/xfce4-caffeine-plugin/icons". */
gchar *caffeine_icons_get_folder (void);

/* Resolves AUTO to LIGHT or DARK based on the current GTK theme.
 * LIGHT/DARK are returned unchanged. */
CaffeineIconTheme caffeine_icons_resolve_theme (CaffeineIconTheme theme);

/* Scans the icon folder for the given theme variant and loads whatever
 * is found, scaled to target_size x target_size. Missing files leave
 * the corresponding CaffeineIconSet field NULL/0 - always succeeds.
 * Free the result with caffeine_icon_set_free(). */
CaffeineIconSet *caffeine_icons_load (gint target_size, CaffeineIconTheme theme);

void caffeine_icon_set_free (CaffeineIconSet *icons);

G_END_DECLS

#endif /* !__CAFFEINE_ICONS_H__ */