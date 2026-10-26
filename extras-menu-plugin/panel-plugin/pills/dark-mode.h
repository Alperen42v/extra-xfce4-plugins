#ifndef EXTRAS_MENU_DARK_MODE_H
#define EXTRAS_MENU_DARK_MODE_H

#include <gtk/gtk.h>

G_BEGIN_DECLS

/*
 * The Dark Mode pill's logic: shows whether the desktop's GTK theme is a
 * dark one, and switches between a theme's dark and light version.
 *
 * "Dark" is decided by the theme's name: one that ends in "dark" or
 * "darker" ("Adwaita-dark", "Arc-Dark", "Materia_dark", ...) counts as
 * dark mode being on; anything else, "Adwaita" or "Arc" or a name
 * ending in "light", counts as off. The pill is lit while it is on.
 *
 * A click switches the theme to its counterpart at once:
 *
 *   dark  -> light   the same name with "-light" in place of "-dark", or
 *                    failing that the name without the "-dark" at all
 *   light -> dark    the same name with "-dark" added (or "-light"
 *                    replaced by it)
 *
 * Only themes that are really installed are considered (GTK's built-in
 * "Adwaita" included). If the counterpart doesn't exist -- a dark theme
 * with no light version, say -- nothing is changed and the user gets a
 * warning through the message function instead.
 *
 * The theme is read and written through Xfce's settings (the
 * "xsettings" channel, /Net/ThemeName), the same setting the Appearance
 * dialog uses, so the pill follows changes made there too, and
 * xfsettingsd applies the new theme to every open window. Only the GTK
 * theme changes; the window manager's own theme (title bars) and the
 * icon theme are separate settings and stay as they are.
 *
 * Like the other pill modules, it owns all of its state and keeps hold
 * of the pill itself, so the plugin needs nothing but the one pointer.
 */
typedef struct _ExtrasMenuDarkMode ExtrasMenuDarkMode;

/* How the module shows a warning (the plugin shows it in a dialog parented
 * to the dropdown). `secondary` may be NULL. */
typedef void (*ExtrasMenuDarkModeMessageFunc)(const gchar *primary,
                                               const gchar *secondary,
                                               gpointer user_data);

/* `pill` is the Dark Mode pill's GtkToggleButton, as handed out by
 * extras_menu_popover_get_pill(content, EXTRAS_MENU_PILL_DARK_MODE).
 * Returns NULL if `pill` isn't a toggle button. */
ExtrasMenuDarkMode *extras_menu_dark_mode_new(GtkWidget *pill,
                                               ExtrasMenuDarkModeMessageFunc message_func,
                                               gpointer user_data);

/* Disconnects from the pill and from Xfce's settings. NULL is allowed. */
void extras_menu_dark_mode_free(ExtrasMenuDarkMode *self);

G_END_DECLS

#endif /* EXTRAS_MENU_DARK_MODE_H */