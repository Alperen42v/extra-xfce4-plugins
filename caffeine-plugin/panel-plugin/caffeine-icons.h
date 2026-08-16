#ifndef __CAFFEINE_ICONS_H__
#define __CAFFEINE_ICONS_H__

#include <gdk-pixbuf/gdk-pixbuf.h>
#include <glib.h>

G_BEGIN_DECLS

/*
 * Which custom icon variant to load - AUTO detects light vs dark from
 * the current GTK theme at load time (see
 * caffeine_icons_resolve_theme()), so it follows the system without the
 * user having to pick manually; LIGHT/DARK pin it regardless of the
 * system theme. Also used by CaffeineSettings in caffeine.h to persist
 * the user's choice - declared here rather than there since it's
 * fundamentally about which icon files get loaded, and this header has
 * no dependency on the rest of the plugin.
 */
typedef enum
{
    CAFFEINE_ICON_THEME_AUTO = 0,
    CAFFEINE_ICON_THEME_LIGHT = 1,
    CAFFEINE_ICON_THEME_DARK = 2
} CaffeineIconTheme;

/*
 * Custom icon support.
 *
 * This plugin is meant to be themeable: users can drop their own PNGs
 * into a fixed folder and the plugin picks them up automatically, no UI
 * needed beyond the theme choice in Preferences (see README.md for the
 * exact filenames/sizes). If the folder or files are missing, the plugin
 * falls back to the built-in Cairo-drawn cup, so nothing breaks for
 * users who never add icons.
 *
 * Layout expected under the icon folder (see caffeine_icons_get_folder()):
 *   off-light.png     - OFF state, light variant (for dark panels/themes)
 *   off-dark.png       - OFF state, dark variant (for light panels/themes)
 *   on-light-01.png
 *   on-light-02.png
 *   ...                - ON state, light variant, played in sequence and
 *                        looped (on-light-01.png alone is fine too - it
 *                        just won't animate)
 *   on-dark-01.png
 *   on-dark-02.png
 *   ...                - ON state, dark variant, same rules as above
 *
 * Frame numbers must be zero-padded and consecutive starting at 01 with
 * no gaps; loading stops at the first missing number.
 *
 * Which variant (light/dark) gets loaded is controlled by the icon_theme
 * setting (CaffeineIconTheme, above): AUTO detects it from the current
 * GTK theme, LIGHT/DARK pin it regardless of the system theme.
 */

typedef struct
{
    GdkPixbuf  *off_frame;      /* NULL if the off-*.png variant wasn't found/loadable */
    GdkPixbuf **on_frames;      /* NULL if no on-*-NN.png frames were found */
    guint       on_frame_count; /* 0 if on_frames is NULL */
} CaffeineIconSet;

/* Returns the folder the plugin looks for custom icons in, as a
 * newly-allocated string the caller must g_free(). Currently
 * "~/.config/xfce4-caffeine-plugin/icons". */
gchar *caffeine_icons_get_folder (void);

/* Resolves AUTO to LIGHT or DARK based on the current GTK theme
 * ("gtk-application-prefer-dark-theme", the same property the GTK theme
 * switcher itself sets). LIGHT/DARK are returned unchanged. Call this
 * wherever you need a concrete variant, e.g. right before
 * caffeine_icons_load(). */
CaffeineIconTheme caffeine_icons_resolve_theme (CaffeineIconTheme theme);

/* Scans the icon folder for the given theme variant (resolve AUTO first
 * with caffeine_icons_resolve_theme() if needed) and loads whatever is
 * found, scaled to target_size x target_size (the panel icon area's
 * pixel size). Missing files just leave the corresponding
 * CaffeineIconSet field NULL/0 - this always succeeds, callers check the
 * fields to know what's available. The result must be freed with
 * caffeine_icon_set_free(). */
CaffeineIconSet *caffeine_icons_load (gint target_size, CaffeineIconTheme theme);

void caffeine_icon_set_free (CaffeineIconSet *icons);

G_END_DECLS

#endif /* !__CAFFEINE_ICONS_H__ */