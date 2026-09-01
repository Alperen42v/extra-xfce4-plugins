#ifndef EXTRAS_MENU_POPOVER_H
#define EXTRAS_MENU_POPOVER_H

#include <gtk/gtk.h>

G_BEGIN_DECLS

/*
 * Builds the full dropdown content (volume + brightness sliders, then
 * the pill grid). The root widget is ready to be placed inside any
 * GtkContainer via gtk_container_add() -- currently a plain GtkWindow
 * (see extras-menu.h for why), previously a GtkPopover; this function
 * doesn't care which. The volume_scale and brightness_scale out-params
 * give the caller (extras-menu.c) handles to the two sliders so they
 * can be wired up to real backends (PulseAudio/PipeWire for volume,
 * brightnessctl for brightness) -- both to push user-driven changes
 * out, and to reflect external changes back in. Either out-param may
 * be NULL if the caller doesn't need that handle.
 *
 * The pill grid (Bluetooth/Dark Mode/Aeroplane Mode/...) remains
 * purely visual for now; only the two sliders are backend-aware at
 * this stage.
 */
GtkWidget *extras_menu_popover_content_new(GtkWidget **volume_scale,
                                            GtkWidget **brightness_scale);

G_END_DECLS

#endif /* EXTRAS_MENU_POPOVER_H */