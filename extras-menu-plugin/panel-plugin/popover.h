#ifndef EXTRAS_MENU_POPOVER_H
#define EXTRAS_MENU_POPOVER_H

#include <gtk/gtk.h>

G_BEGIN_DECLS

/*
 * Builds the full dropdown content (volume + brightness sliders, then
 * the pill grid, then a hidden-by-default Wi-Fi network list). The
 * root widget is ready to be placed inside any GtkContainer via
 * gtk_container_add() -- currently a plain GtkWindow (see
 * extras-menu.h for why), previously a GtkPopover; this function
 * doesn't care which. The volume_scale and brightness_scale out-params
 * give the caller (extras-menu.c) handles to the two sliders so they
 * can be wired up to real backends (PulseAudio/PipeWire for volume,
 * brightnessctl for brightness) -- both to push user-driven changes
 * out, and to reflect external changes back in. volume_icon hands back
 * the volume row's icon GtkImage, so the caller can swap it to a
 * "muted" icon at 0%/when muted. bluetooth_toggle hands back the
 * Bluetooth pill's GtkToggleButton so the caller can wire it up to the
 * BlueZ backend the same way. network_pill_button/label/icon hand back
 * the network pill's button, label and icon widgets, so the caller can
 * switch its text/icon between "Wi-Fi" and "Ethernet" as the
 * connection changes, and wire up a click handler (e.g. to toggle
 * network_revealer below). network_revealer hands back the GtkRevealer
 * wrapping the Wi-Fi network list -- collapsed (reveal-child FALSE) by
 * default; the caller decides when to expand it (normally: when the
 * network pill is clicked, and only when in Wi-Fi mode -- Ethernet
 * mode has nothing to list). network_list_box hands back the
 * GtkListBox inside it, ready to be populated with one row per visible
 * access point. Any out-param may be NULL if the caller doesn't need
 * that handle.
 *
 * The rest of the pill grid (Dark Mode/Aeroplane Mode/Balanced/...)
 * remains purely visual for now; only the sliders, the Bluetooth
 * toggle, and the network pill are backend-aware at this stage.
 */
GtkWidget *extras_menu_popover_content_new(GtkWidget **volume_scale,
                                            GtkWidget **volume_icon,
                                            GtkWidget **brightness_scale,
                                            GtkWidget **bluetooth_toggle,
                                            GtkWidget **network_pill_button,
                                            GtkWidget **network_pill_label,
                                            GtkWidget **network_pill_icon,
                                            GtkWidget **network_revealer,
                                            GtkWidget **network_list_box);

G_END_DECLS

#endif /* EXTRAS_MENU_POPOVER_H */