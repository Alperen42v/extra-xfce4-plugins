#ifndef EXTRAS_MENU_POPOVER_H
#define EXTRAS_MENU_POPOVER_H

#include <gtk/gtk.h>

#include "preferences/quick-actions-config.h"
#include "preferences/pill-layout-config.h"

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
 * pieces (initially a "100%" placeholder; extras-menu.c keeps them in
 * step with the UPower backend). quick_actions_box hands back the
 * empty horizontal GtkBox at the top bar's right edge that holds the
 * quick-action buttons -- popover.c doesn't create those buttons
 * itself, since which ones exist is a user setting; quick-actions.c
 * fills the box and keeps it up to date.
 *
 * The volume_scale and brightness_scale out-params give the caller
 * (extras-menu.c) handles to the two sliders so they can be wired up
 * to real backends (PulseAudio/PipeWire for volume, brightnessctl for
 * brightness) -- both to push user-driven changes out, and to reflect
 * external changes back in. volume_icon hands back the volume row's
 * icon GtkImage, so the caller can swap it to a "muted" icon at
 * 0%/when muted.
 *
 * The Bluetooth pill is a split pill like the network one: bluetooth_toggle
 * is its main GtkToggleButton (adapter on/off), bluetooth_expand_button
 * the small chevron button that reveals bluetooth_revealer, and
 * bluetooth_expand_chevron the chevron's GtkImage so the caller can flip
 * its direction with the list's open/closed state. bluetooth_list_box is
 * the GtkListBox inside the revealer, ready to be filled with one row per
 * device. The revealer is collapsed by default; the caller decides when
 * to open it (and keeps it and the Wi-Fi list from being open together).
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
 * toggle, and the network pill are backend-aware at this stage.
 */
GtkWidget *extras_menu_popover_content_new(GtkWidget **battery_label,
                                            GtkWidget **battery_icon,
                                            GtkWidget **quick_actions_box,
                                            GtkWidget **volume_scale,
                                            GtkWidget **volume_icon,
                                            GtkWidget **brightness_scale,
                                            GtkWidget **bluetooth_toggle,
                                            GtkWidget **bluetooth_expand_button,
                                            GtkWidget **bluetooth_expand_chevron,
                                            GtkWidget **bluetooth_revealer,
                                            GtkWidget **bluetooth_list_box,
                                            GtkWidget **network_toggle,
                                            GtkWidget **network_pill_label,
                                            GtkWidget **network_pill_icon,
                                            GtkWidget **network_expand_button,
                                            GtkWidget **network_expand_chevron,
                                            GtkWidget **network_icon_stack,
                                            GtkWidget **network_spinner,
                                            GtkWidget **network_revealer,
                                            GtkWidget **network_list_box);

/* Creates an empty button styled like the other top-bar buttons; the
 * caller puts its own icon inside. This is how quick-actions.c makes
 * the user-configured quick-action buttons match the rest of the bar. */
GtkWidget *extras_menu_top_bar_button_new(void);

/* Applies `layout` to the pill grid's big buttons: a GPtrArray of const
 * ExtrasMenuPillInfo* in reading order (see
 * preferences/pill-layout-config.h). Pills in the layout are shown in
 * their cells; pills not in it are hidden. `content` is the widget
 * returned by extras_menu_popover_content_new(). The pills themselves
 * are only moved and shown/hidden, never rebuilt, so the backends'
 * handles on them (the network and Bluetooth toggles, ...) stay valid
 * even for a hidden pill. The content already starts out in the saved
 * layout; this is for applying a later change. */
void extras_menu_popover_set_pill_order(GtkWidget *content, const GPtrArray *layout);

/* Applies `layout` to the slider rows (volume, brightness): a GPtrArray
 * of const ExtrasMenuSliderInfo* in top-to-bottom order (see
 * preferences/slider-layout-config.h). Sliders in the layout are shown
 * in that order; sliders not in it are hidden. `content` is the widget
 * returned by extras_menu_popover_content_new(). The rows are only
 * moved and shown/hidden, never rebuilt, so the volume_scale /
 * brightness_scale handles the caller got stay valid and keep following
 * their backends even while hidden. The content already starts out in
 * the saved layout; this is for applying a later change. */
void extras_menu_popover_set_slider_order(GtkWidget *content, const GPtrArray *layout);

/* Returns the pill widget with this id (one of the EXTRAS_MENU_PILL_*
 * ids from preferences/pill-layout-config.h), or NULL if there is none.
 * `content` is the widget returned by extras_menu_popover_content_new().
 * The widget is the pill's own GtkToggleButton, whether the pill is
 * currently shown or hidden, so a pill module (see pills/) can attach
 * itself to it without popover.c needing a new out-param for every
 * pill. The widget stays valid for the lifetime of `content`. */
GtkWidget *extras_menu_popover_get_pill(GtkWidget *content, const gchar *pill_id);

/* Restyles the pill grid's big buttons: the shipped soft rounded squares
 * or GNOME-style capsules with fully rounded ends. `content` is the
 * widget returned by extras_menu_popover_content_new(). Works on the
 * grid rather than on each pill, so it is safe to call as often as you
 * like. The content already starts out in the saved shape; this is for
 * applying a later change. */
void extras_menu_popover_set_pill_shape(GtkWidget *content, ExtrasMenuPillShape shape);

/* Restyles every quick-action button in quick_actions_box (the box
 * handed out by extras_menu_popover_content_new()) as squares or
 * circles. Works on the box rather than on the buttons themselves, so
 * it also covers buttons that are created later, and it is safe to
 * call as often as you like. */
void extras_menu_popover_set_quick_action_shape(GtkWidget *quick_actions_box,
                                                 ExtrasMenuQuickActionShape shape);

G_END_DECLS

#endif /* EXTRAS_MENU_POPOVER_H */