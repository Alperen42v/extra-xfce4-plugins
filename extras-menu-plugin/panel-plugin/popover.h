#ifndef EXTRAS_MENU_POPOVER_H
#define EXTRAS_MENU_POPOVER_H

#include <gtk/gtk.h>

G_BEGIN_DECLS

/*
 * Builds the full dropdown content: a top status bar (battery badge +
 * quick-action buttons), then volume + brightness sliders, then the
 * pill grid, then a hidden-by-default Wi-Fi network list. The root
 * widget is ready to be placed inside any GtkContainer via
 * gtk_container_add() -- currently a plain GtkWindow (see
 * extras-menu.h for why), previously a GtkPopover; this function
 * doesn't care which.
 *
 * battery_label/battery_icon hand back the top bar's battery badge
 * pieces -- currently a fixed "100%" placeholder with a static icon,
 * not yet backed by a real UPower reading. screenshot_button/
 * settings_button/lock_button/power_button hand back the four
 * quick-action buttons in the top bar's top-right corner -- plain
 * icon buttons with no click behavior wired up yet.
 *
 * The volume_scale and brightness_scale out-params give the caller
 * (extras-menu.c) handles to the two sliders so they can be wired up
 * to real backends (PulseAudio/PipeWire for volume, brightnessctl for
 * brightness) -- both to push user-driven changes out, and to reflect
 * external changes back in. volume_icon hands back the volume row's
 * icon GtkImage, so the caller can swap it to a "muted" icon at
 * 0%/when muted. bluetooth_toggle hands back the Bluetooth pill's
 * GtkToggleButton so the caller can wire it up to the BlueZ backend
 * the same way.
 *
 * The network pill is split into two independently clickable regions
 * (see extras_menu_make_split_pill() in popover.c): network_toggle is
 * the main GtkToggleButton (icon + "Wi-Fi"/"Ethernet" label), meant to
 * turn the Wi-Fi radio itself on/off; network_expand_button is the
 * small chevron button that reveals network_revealer's contents.
 * network_expand_chevron hands back the chevron's own GtkImage, so the
 * caller can flip its direction (e.g. pan-down-symbolic while the list
 * is expanded, pan-end-symbolic while collapsed) to mirror the list's
 * open/closed state.
 * network_pill_label/icon hand back the label and icon widgets inside
 * network_toggle. network_icon_stack/network_spinner hand back the
 * GtkStack holding network_pill_icon (as the "icon" child) plus a
 * GtkSpinner (as the "spinner" child) -- the caller shows the spinner
 * while a connection attempt is in progress by calling
 * gtk_spinner_start() and gtk_stack_set_visible_child_name(stack,
 * "spinner"), then reverses both once the attempt finishes.
 * network_revealer hands back the GtkRevealer wrapping the
 * Wi-Fi network list -- collapsed (reveal-child FALSE) by default; the
 * caller decides when to expand it (normally: when network_expand_button
 * is clicked, and only when in Wi-Fi mode -- Ethernet mode has nothing
 * to list). network_list_box hands back the GtkListBox inside it,
 * ready to be populated with one row per visible access point. Any
 * out-param may be NULL if the caller doesn't need that handle.
 *
 * The rest of the pill grid (Dark Mode/Aeroplane Mode/Balanced/...)
 * remains purely visual for now; only the sliders, the Bluetooth
 * toggle, and the network pill are backend-aware at this stage. The
 * top bar is purely visual in its entirety for now.
 */
GtkWidget *extras_menu_popover_content_new(GtkWidget **battery_label,
                                            GtkWidget **battery_icon,
                                            GtkWidget **screenshot_button,
                                            GtkWidget **settings_button,
                                            GtkWidget **lock_button,
                                            GtkWidget **power_button,
                                            GtkWidget **volume_scale,
                                            GtkWidget **volume_icon,
                                            GtkWidget **brightness_scale,
                                            GtkWidget **bluetooth_toggle,
                                            GtkWidget **network_toggle,
                                            GtkWidget **network_pill_label,
                                            GtkWidget **network_pill_icon,
                                            GtkWidget **network_expand_button,
                                            GtkWidget **network_expand_chevron,
                                            GtkWidget **network_icon_stack,
                                            GtkWidget **network_spinner,
                                            GtkWidget **network_revealer,
                                            GtkWidget **network_list_box);

G_END_DECLS

#endif /* EXTRAS_MENU_POPOVER_H */