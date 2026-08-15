#ifndef __CAFFEINE_ICONS_H__
#define __CAFFEINE_ICONS_H__

#include <gdk-pixbuf/gdk-pixbuf.h>
#include <glib.h>

G_BEGIN_DECLS

/*
 * Custom icon support.
 *
 * This plugin is meant to be themeable: users can drop their own PNGs
 * into a fixed folder and the plugin picks them up automatically, no UI
 * needed for it (see README.md for the exact filenames/sizes). If the
 * folder or files are missing, the plugin falls back to the built-in
 * Cairo-drawn cup, so nothing breaks for users who never add icons.
 *
 * Layout expected under the icon folder (see caffeine_icons_get_folder()):
 *   off.png       - shown when caffeine is OFF (single, static image)
 *   on-01.png
 *   on-02.png
 *   ...           - shown in sequence, looping, when caffeine is ON
 *                   (on-01.png alone is fine too - it just won't animate)
 *
 * Frame numbers must be zero-padded and consecutive starting at 01 with
 * no gaps; loading stops at the first missing number.
 */

typedef struct
{
    GdkPixbuf  *off_frame;      /* NULL if off.png wasn't found/loadable */
    GdkPixbuf **on_frames;      /* NULL if no on-NN.png frames were found */
    guint       on_frame_count; /* 0 if on_frames is NULL */
} CaffeineIconSet;

/* Returns the folder the plugin looks for custom icons in, as a
 * newly-allocated string the caller must g_free(). Currently
 * "~/.config/xfce4-caffeine-plugin/icons". */
gchar *caffeine_icons_get_folder (void);

/* Scans the icon folder and loads whatever is found, scaled to
 * target_size x target_size (the panel icon area's pixel size). Missing
 * files just leave the corresponding CaffeineIconSet field NULL/0 - this
 * always succeeds, callers check the fields to know what's available.
 * The result must be freed with caffeine_icon_set_free(). */
CaffeineIconSet *caffeine_icons_load (gint target_size);

void caffeine_icon_set_free (CaffeineIconSet *icons);

G_END_DECLS

#endif /* !__CAFFEINE_ICONS_H__ */