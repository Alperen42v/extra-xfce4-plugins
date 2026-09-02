#ifndef EXTRAS_MENU_H
#define EXTRAS_MENU_H

#include <gtk/gtk.h>
#include <libxfce4panel/libxfce4panel.h>

#include "audio.h"
#include "bluetooth.h"
#include "brightness.h"

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
    GtkWidget *button_icon;

    /* the dropdown itself and its content, built once and reused.
     * Implemented as a plain always-on-top GtkWindow rather than a
     * GtkPopover: GtkPopover rendered behind the always-on-top panel
     * window on this setup (visible/mapped according to GTK, but
     * never actually painted above the panel), regardless of theme or
     * compositor settings. A manually positioned GtkWindow with
     * gtk_window_set_keep_above() sidesteps that entirely. */
    GtkWidget *popover;

    /* volume slider inside the popover, its icon (swapped to a muted
     * variant at 0%/when muted), and the backend driving both */
    GtkWidget *volume_scale;
    GtkWidget *volume_icon;
    ExtrasMenuAudio *audio;

    /* brightness slider inside the popover, and the backend driving it */
    GtkWidget *brightness_scale;
    ExtrasMenuBrightness *brightness;

    /* Bluetooth pill toggle inside the popover, and the backend
     * driving it (adapter power on/off only, for now) */
    GtkWidget *bluetooth_toggle;
    ExtrasMenuBluetooth *bluetooth;

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
};

struct _ExtrasMenuPluginClass
{
    XfcePanelPluginClass parent_class;
};

GType extras_menu_plugin_get_type(void) G_GNUC_CONST;

G_END_DECLS

#endif /* EXTRAS_MENU_H */