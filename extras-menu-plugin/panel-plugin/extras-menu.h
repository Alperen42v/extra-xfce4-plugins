#ifndef EXTRAS_MENU_H
#define EXTRAS_MENU_H

#include <gtk/gtk.h>
#include <libxfce4panel/libxfce4panel.h>

#include "audio.h"
#include "battery.h"
#include "bluetooth/bluetooth.h"
#include "brightness.h"
#include "microphone.h"
#include "network/network.h"
#include "pills/airplane-mode.h"

G_BEGIN_DECLS

#define EXTRAS_MENU_TYPE_PLUGIN            (extras_menu_plugin_get_type())
#define EXTRAS_MENU_PLUGIN(obj)            (G_TYPE_CHECK_INSTANCE_CAST((obj), EXTRAS_MENU_TYPE_PLUGIN, ExtrasMenuPlugin))
#define EXTRAS_MENU_IS_PLUGIN(obj)         (G_TYPE_CHECK_INSTANCE_TYPE((obj), EXTRAS_MENU_TYPE_PLUGIN))
#define EXTRAS_MENU_PLUGIN_CLASS(klass)    (G_TYPE_CHECK_CLASS_CAST((klass), EXTRAS_MENU_TYPE_PLUGIN, ExtrasMenuPluginClass))

typedef struct _ExtrasMenuPlugin      ExtrasMenuPlugin;
typedef struct _ExtrasMenuPluginClass ExtrasMenuPluginClass;

struct _ExtrasMenuPlugin
{
    XfcePanelPlugin parent;

    /* panel button that toggles the popover */
    GtkWidget *toggle_button;

    /* What the panel button shows instead of a chevron: three status
     * icons side by side -- Wi-Fi/network state, volume level, and
     * battery level (or system-shutdown-symbolic on machines without a
     * battery). panel_icons_box holds them so its orientation can follow
     * the panel's (horizontal panel -> icons in a row, vertical panel ->
     * in a column). Each icon is kept up to date by the same backend
     * callbacks that drive the dropdown's own widgets. */
    GtkWidget *panel_icons_box;
    GtkWidget *panel_network_icon;
    GtkWidget *panel_volume_icon;
    GtkWidget *panel_battery_icon;

    /* the dropdown itself and its content, built once and reused.
     * Implemented as a plain always-on-top GtkWindow rather than a
     * GtkPopover: GtkPopover rendered behind the always-on-top panel
     * window on this setup (visible/mapped according to GTK, but
     * never actually painted above the panel), regardless of theme or
     * compositor settings. A manually positioned GtkWindow with
     * gtk_window_set_keep_above() sidesteps that entirely. */
    GtkWidget *popover;

    /* Top status bar inside the popover: battery badge + the
     * quick-action buttons. The buttons themselves are user settings
     * (see preferences/quick-actions-config.h), so there are no
     * per-button fields here: quick_actions_box is just the empty
     * container popover.c provides, which quick-actions.c fills in and
     * keeps up to date by itself. */
    GtkWidget *battery_label;
    GtkWidget *battery_icon;
    GtkWidget *quick_actions_box;

    /* Id of this instance's listener on the quick-actions config (0 =
     * none): it re-applies the saved button shape (square/circle)
     * whenever the preferences editor saves something. */
    guint quick_actions_config_listener;

    /* Same idea for the pill grid: this instance's listener on the pill
     * layout config (0 = none), which moves the pills when the
     * preferences editor saves a new order. */
    guint pill_layout_listener;

    /* And for the slider rows: this instance's listener on the slider
     * layout config (0 = none), which moves/shows/hides the volume and
     * brightness rows when the preferences editor saves a new layout. */
    guint slider_layout_listener;

    /* UPower battery backend driving the badge above. The badge is
     * hidden until the backend reports a battery is present, so
     * machines without one (desktops) never show it. */
    ExtrasMenuBattery *battery;

    /* volume slider inside the popover, its icon (swapped to a muted
     * variant at 0%/when muted), and the backend driving both */
    GtkWidget *volume_scale;
    GtkWidget *volume_icon;
    ExtrasMenuAudio *audio;

    /* brightness slider inside the popover, and the backend driving it */
    GtkWidget *brightness_scale;
    ExtrasMenuBrightness *brightness;

    /* Bluetooth pill inside the popover, split into two regions like
     * the network pill: bluetooth_toggle (turns the adapter on/off) and
     * bluetooth_expand_button (the small chevron, reveals
     * bluetooth_revealer's device list), plus the backend driving them. */
    GtkWidget *bluetooth_toggle;
    GtkWidget *bluetooth_expand_button;
    GtkWidget *bluetooth_expand_chevron;
    GtkWidget *bluetooth_revealer;
    GtkWidget *bluetooth_list_box;
    ExtrasMenuBluetooth *bluetooth;

    /* Maps device path (owned gchar*) -> the GtkListBoxRow currently
     * showing it (borrowed -- rows belong to bluetooth_list_box), so a
     * row's "Connecting.../Pairing..." label can be updated without a
     * linear search. Rebuilt every time the device list is repopulated. */
    GHashTable *bluetooth_row_by_path;

    /* The device a connect/disconnect/pair request is currently running
     * for (owned path, NULL if none), and the sentence to show if it
     * fails. Only one request runs at a time; clicks on rows while one
     * is running are ignored. */
    gchar *bluetooth_busy_path;
    const gchar *bluetooth_busy_text;           /* shown on that row meanwhile ("Connecting...") */
    const gchar *bluetooth_busy_failure_title;

    /* Network pill (labeled "Wi-Fi" or "Ethernet" depending on the
     * active connection) inside the popover, split into two regions:
     * network_toggle (icon + label, turns the Wi-Fi radio itself
     * on/off) and network_expand_button (the small chevron, reveals
     * network_revealer's Wi-Fi list). See extras_menu_make_split_pill()
     * in popover.c for why these are separate widgets rather than one
     * button doing both. */
    GtkWidget *network_toggle;
    GtkWidget *network_pill_label;
    GtkWidget *network_pill_icon;
    GtkWidget *network_expand_button;
    GtkWidget *network_expand_chevron;
    GtkWidget *network_icon_stack;
    GtkWidget *network_spinner;
    GtkWidget *network_revealer;
    GtkWidget *network_list_box;
    ExtrasMenuNetwork *network;

    /* Maps SSID (owned gchar*) -> the GtkListBoxRow currently showing
     * it (borrowed, not owned -- rows belong to network_list_box), so
     * on_network_row_activated() and extras_menu_network_ui_connect_result() can find and
     * update a specific row's "Connecting.../Connected" label without
     * a linear search. Rebuilt every time on_network_list_changed()
     * repopulates the list. */
    GHashTable *network_row_by_ssid;

    /* Last status reported by the network backend, cached here for the
     * same reason as bluetooth_last_available/powered below -- GDBus
     * has been observed to invoke callbacks before the relevant
     * widgets exist yet in some cases. Applied to the widgets as soon
     * as they're available. */
    ExtrasMenuNetworkKind network_last_kind;
    gchar *network_last_ip_address;
    gboolean network_has_status;

    /* Same caching idea, for the Wi-Fi radio's own enabled state
     * (separate from network_last_kind, which is about the active
     * *connection* rather than whether the radio is even on). */
    gboolean network_wifi_last_enabled;
    gboolean network_wifi_has_enabled_state;

    /* Tracks the network currently being connected to, so
     * extras_menu_network_ui_connect_result() knows whether a failure should fall back to
     * a password prompt (see on_network_row_activated() in
     * network/network-ui.c for the full flow: try without a password first,
     * only prompt if that fails and the network is secured). */
    gchar *pending_connect_ssid;
    gboolean pending_connect_secured;
    gboolean pending_connect_password_was_tried;

    /* The security scheme of the network being connected to. Needed
     * because enterprise (802.1X) networks can't be set up with just a
     * password, so a failed first attempt on one must show the
     * backend's explanation instead of falling back to a password
     * prompt that could never work. */
    ExtrasMenuApSecurity pending_connect_security;

    /* TRUE from the moment a network row is clicked until the attempt
     * has finally succeeded, failed or been cancelled -- including
     * while the password dialog is open. While it's set, further clicks
     * on network rows are ignored: a second attempt started on top of
     * the first would overwrite pending_connect_ssid (so the first
     * attempt's result would be applied to the wrong row), and a quick
     * double-click could create the same profile twice. */
    gboolean pending_connect_active;

    /* TRUE while the password dialog is open for the attempt. The
     * network list is rebuilt from scratch on every scan update (see
     * on_network_list_changed()), and the rebuilt row of the network
     * being connected to must show "Connecting..." again -- except
     * while the user is being asked for a password, when the row shows
     * nothing. */
    gboolean pending_connect_prompting;

    /* Last state reported by the Bluetooth backend, cached here in
     * case on_bluetooth_changed() fires before bluetooth_toggle exists
     * yet (observed in practice: GDBus can call back synchronously in
     * some cases, before extras_menu_bluetooth_new() has even
     * returned to construct()). Applied to the widget as soon as it's
     * available. */
    gboolean bluetooth_last_available;
    gboolean bluetooth_last_powered;
    gboolean bluetooth_has_state;

    /* guards against feedback loops: TRUE while we are programmatically
     * setting the slider's value from a backend callback, so our own
     * "value-changed" handler knows not to echo it straight back out
     * to PulseAudio. */
    gboolean updating_volume_from_backend;

    /* same idea as updating_volume_from_backend, but for the
     * brightness slider / brightnessctl. */
    gboolean updating_brightness_from_backend;

    /* same idea again, but for the Bluetooth toggle / BlueZ. */
    gboolean updating_bluetooth_from_backend;

    /* same idea again, but for network_toggle (Wi-Fi radio on/off) /
     * NetworkManager's WirelessEnabled. */
    gboolean updating_wifi_enabled_from_backend;

    /* The Aeroplane Mode pill's logic (pills/airplane-mode.c): switches
     * Wi-Fi and Bluetooth off together. It lives off to the side of the
     * other pills' fields on purpose -- the module owns its own state,
     * so all that is needed here is this one pointer. It is fed by
     * on_wifi_enabled_changed() and on_bluetooth_changed(). NULL if the
     * pill widget wasn't found. */
    ExtrasMenuAirplaneMode *airplane_mode;

    /* How many of the dropdown's own dialogs (password prompt, error
     * and info messages, network details) are currently open. While
     * this is non-zero, the dropdown's focus-out handler leaves the
     * dropdown open, since focus moving to one of those dialogs isn't
     * the user clicking away. See extras_menu_dropdown_track_dialog() in dropdown.c. */
    guint open_dialog_count;

    /* The microphone row's logic (microphone.c): level, mute and
     * following the default input device. Like the Aeroplane Mode
     * module it owns its own state and holds on to the row's widgets
     * itself, so all that is needed here is this one pointer. NULL if
     * the row's widgets weren't found.
     *
     * Kept as the LAST field on purpose: every .c file reads this
     * struct by field offset, so a field added in the middle moves all
     * the ones after it -- and any file that a build didn't recompile
     * would then read the wrong memory. Appended here, the older
     * offsets stay valid. */
    ExtrasMenuMicrophone *microphone;

    /* Bottom-panel behaviour of the dropdown (dropdown.c). With a panel
     * at the bottom of the screen the dropdown opens upward, so its
     * BOTTOM edge must stay next to the panel button even when a list
     * (Wi-Fi/Bluetooth) opens and the window grows. While
     * dropdown_anchored_bottom is TRUE, dropdown_anchor_bottom is the
     * screen Y of that bottom edge, and the window is moved up by
     * however much it grows. FALSE for every other panel position, where
     * the window opens downward and simply grows downward.
     *
     * Same rule as above: new fields go at the END of the struct. */
    gboolean dropdown_anchored_bottom;
    gint dropdown_anchor_bottom;
};

struct _ExtrasMenuPluginClass
{
    XfcePanelPluginClass parent_class;
};

GType extras_menu_plugin_get_type(void) G_GNUC_CONST;

G_END_DECLS

#endif /* EXTRAS_MENU_H */