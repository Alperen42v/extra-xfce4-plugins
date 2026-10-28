#ifndef EXTRAS_MENU_DROPDOWN_STYLE_H
#define EXTRAS_MENU_DROPDOWN_STYLE_H

#include "extras-menu.h"

G_BEGIN_DECLS

/*
 * Gives the dropdown the panel's own background: its color and its
 * transparency, as a flat color (no gradient or image), so the menu looks
 * like a piece of the panel rather than a window in the theme's colors.
 * The Wi-Fi and Bluetooth lists get a darker shade of that color.
 *
 * Where the color comes from: the panel paints the same background
 * behind its plugins in the window the plugin lives in. That is the
 * color the panel's own settings choose (a solid color with its
 * transparency), or, when the panel is set to follow the theme, whatever
 * the theme gives a panel. Either way it is read from that window, and
 * kept up to date: when the panel's background is changed in its
 * settings, or the theme changes, the dropdown follows -- also while it
 * is open.
 *
 * If the panel has no usable color (a background image, or one that is
 * practically invisible), the theme's own background color is used
 * instead, so the menu always stays readable.
 *
 * Attach it once the dropdown's content exists (it needs the content and
 * the two lists); it adds a stylesheet to them and watches the panel. The
 * rest of the look (padding, rounded corners) stays in popover.c.
 */
void extras_menu_dropdown_style_attach(ExtrasMenuPlugin *plugin);

/* Stops watching and releases what this module keeps. NULL-safe. */
void extras_menu_dropdown_style_free(ExtrasMenuPlugin *plugin);

G_END_DECLS

#endif /* EXTRAS_MENU_DROPDOWN_STYLE_H */