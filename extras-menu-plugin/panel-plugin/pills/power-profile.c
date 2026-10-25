/*
 * The Balanced pill: shows and switches the system power profile via
 * power-profiles-daemon. See power-profile.h.
 *
 * The daemon is on the system bus under one of two names: the current
 * org.freedesktop.UPower.PowerProfiles, or the older
 * net.hadess.PowerProfiles that older versions only had. Both offer the
 * same properties, so the first one that exists is used.
 *
 *   ActiveProfile  (s, read/write)  "power-saver" | "balanced" | "performance"
 *   Profiles       (aa{sv})         the supported ones, each with a "Profile"
 *                                   key, always in the order above
 *
 * A GDBusProxy caches both and tells us when they change, so the pill
 * follows outside changes without any polling.
 */

#include "pills/power-profile.h"

#include <gio/gio.h>

typedef struct
{
    const gchar *id;            /* the daemon's name for the profile */
    const gchar *title;         /* shown on the pill */
    const gchar *icon;          /* from the icon theme (Adwaita has these) ... */
    const gchar *fallback_icon; /* ... and this if the theme lacks it */
} ProfileInfo;

static const ProfileInfo PROFILE_INFOS[] = {
    { "power-saver", "Power Saver", "power-profile-power-saver-symbolic", "battery-good-symbolic" },
    { "balanced",    "Balanced",    "power-profile-balanced-symbolic",    "weather-clear-symbolic" },
    { "performance", "Performance", "power-profile-performance-symbolic", "starred-symbolic" },
};

typedef struct
{
    const gchar *name;
    const gchar *path;
    const gchar *interface;
} Service;

static const Service SERVICES[] = {
    { "org.freedesktop.UPower.PowerProfiles", "/org/freedesktop/UPower/PowerProfiles",
      "org.freedesktop.UPower.PowerProfiles" },
    { "net.hadess.PowerProfiles", "/net/hadess/PowerProfiles", "net.hadess.PowerProfiles" },
};

struct _ExtrasMenuPowerProfile
{
    /* The pill and the two widgets inside it; each holds a reference. */
    GtkWidget *pill;
    GtkWidget *icon;
    GtkWidget *label;
    gulong clicked_id;

    GCancellable *cancellable; /* cancelled in _free(): ends every pending call */
    GDBusProxy *proxy;         /* NULL until a service answered */
    guint service_index;       /* which entry of SERVICES is being tried / used */

    /* What the daemon reported last (see read_state()). */
    gboolean available;
    gchar *active;
    GPtrArray *profiles;       /* owned gchar*, supported ones in cycle order */

    gboolean busy;             /* a Set call is in flight: clicks are ignored */
};

static const ProfileInfo *
lookup_profile(const gchar *id)
{
    if (id == NULL)
        return NULL;

    for (guint i = 0; i < G_N_ELEMENTS(PROFILE_INFOS); i++)
    {
        if (g_strcmp0(PROFILE_INFOS[i].id, id) == 0)
            return &PROFILE_INFOS[i];
    }
    return NULL;
}

/* --- pill display ------------------------------------------------------------- */

/* Makes the pill show what the last read_state() found. */
static void
refresh(ExtrasMenuPowerProfile *self)
{
    const ProfileInfo *info = lookup_profile(self->available ? self->active : "balanced");

    /* A profile this code doesn't know (a future one): show its raw
     * name, with the Balanced icon. */
    const ProfileInfo *icon_info = info != NULL ? info : lookup_profile("balanced");
    GtkIconTheme *theme = gtk_icon_theme_get_for_screen(gtk_widget_get_screen(self->pill));
    const gchar *icon_name = gtk_icon_theme_has_icon(theme, icon_info->icon)
                                 ? icon_info->icon
                                 : icon_info->fallback_icon;

    gtk_image_set_from_icon_name(GTK_IMAGE(self->icon), icon_name, GTK_ICON_SIZE_BUTTON);
    gtk_label_set_text(GTK_LABEL(self->label),
                       info != NULL ? info->title : (self->active != NULL ? self->active : "Balanced"));
    gtk_widget_set_sensitive(self->pill, self->available);

    /* Highlighted while a non-default profile is active, so the pill
     * "lights up" when the machine is trading speed against battery.
     * A click flips a toggle button by itself; this is also what puts
     * it back (see on_pill_clicked()). */
    gboolean lit = self->available && g_strcmp0(self->active, "balanced") != 0;
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(self->pill), lit);

    if (!self->available)
    {
        gtk_widget_set_tooltip_text(self->pill,
                                    "Power profiles unavailable\n"
                                    "(power-profiles-daemon is not installed or not running)");
    }
    else if (self->profiles->len < 2)
    {
        gtk_widget_set_tooltip_text(self->pill, "Only one power profile is available");
    }
    else
    {
        const gchar *current = info != NULL ? info->title : self->active;
        gchar *tooltip = g_strdup_printf("Power profile: %s\nClick to switch", current);
        gtk_widget_set_tooltip_text(self->pill, tooltip);
        g_free(tooltip);
    }
}

/* Re-reads the proxy's cached ActiveProfile / Profiles and updates the pill. */
static void
read_state(ExtrasMenuPowerProfile *self)
{
    g_ptr_array_set_size(self->profiles, 0);
    g_clear_pointer(&self->active, g_free);
    self->available = FALSE;

    /* Without a name owner the daemon isn't running and the proxy's
     * cache is empty. */
    gchar *owner = self->proxy != NULL ? g_dbus_proxy_get_name_owner(self->proxy) : NULL;
    if (owner != NULL)
    {
        GVariant *active = g_dbus_proxy_get_cached_property(self->proxy, "ActiveProfile");
        if (active != NULL)
        {
            if (g_variant_is_of_type(active, G_VARIANT_TYPE_STRING))
            {
                self->active = g_variant_dup_string(active, NULL);
                self->available = TRUE;
            }
            g_variant_unref(active);
        }

        GVariant *profiles = g_dbus_proxy_get_cached_property(self->proxy, "Profiles");
        if (profiles != NULL)
        {
            if (g_variant_is_of_type(profiles, G_VARIANT_TYPE("aa{sv}")))
            {
                GVariantIter iter;
                GVariant *entry;

                g_variant_iter_init(&iter, profiles);
                while ((entry = g_variant_iter_next_value(&iter)) != NULL)
                {
                    const gchar *id = NULL;
                    if (g_variant_lookup(entry, "Profile", "&s", &id) && id != NULL)
                        g_ptr_array_add(self->profiles, g_strdup(id));
                    g_variant_unref(entry);
                }
            }
            g_variant_unref(profiles);
        }
    }
    g_free(owner);

    /* No usable list: at least cycle through the one we know. */
    if (self->available && self->profiles->len == 0)
        g_ptr_array_add(self->profiles, g_strdup(self->active));

    refresh(self);
}

/* --- switching ------------------------------------------------------------------ */

static void
on_set_done(GObject *source, GAsyncResult *result, gpointer user_data)
{
    GError *error = NULL;
    GVariant *reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), result, &error);

    /* Cancelled means the plugin is going away: `self` may be gone. */
    if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    {
        g_clear_error(&error);
        return;
    }

    ExtrasMenuPowerProfile *self = user_data;
    self->busy = FALSE;

    if (error != NULL)
    {
        g_warning("extras-menu: could not change the power profile: %s", error->message);
        g_clear_error(&error);
    }
    if (reply != NULL)
        g_variant_unref(reply);

    /* On success the daemon's change signal usually arrives as well;
     * reading again here also puts the pill back right after a failure. */
    read_state(self);
}

/* Index in self->profiles of the active profile, or -1. */
static gint
active_index(ExtrasMenuPowerProfile *self)
{
    for (guint i = 0; i < self->profiles->len; i++)
    {
        if (g_strcmp0(g_ptr_array_index(self->profiles, i), self->active) == 0)
            return (gint) i;
    }
    return -1;
}

static void
on_pill_clicked(GtkButton *button, gpointer user_data)
{
    (void) button;
    ExtrasMenuPowerProfile *self = user_data;

    /* The click already flipped the toggle button; undo that. The pill's
     * look only ever changes when the daemon reports a new profile. */
    refresh(self);

    if (!self->available || self->busy || self->proxy == NULL || self->profiles->len < 2)
        return;

    gint index = active_index(self);
    const gchar *next = g_ptr_array_index(self->profiles,
                                          (guint) (index + 1) % self->profiles->len);

    self->busy = TRUE;

    /* ActiveProfile is a read/write property: set it through the
     * standard Properties interface. */
    g_dbus_connection_call(g_dbus_proxy_get_connection(self->proxy),
                           g_dbus_proxy_get_name(self->proxy),
                           g_dbus_proxy_get_object_path(self->proxy),
                           "org.freedesktop.DBus.Properties", "Set",
                           g_variant_new("(ssv)",
                                         g_dbus_proxy_get_interface_name(self->proxy),
                                         "ActiveProfile",
                                         g_variant_new_string(next)),
                           NULL, G_DBUS_CALL_FLAGS_NONE, -1,
                           self->cancellable, on_set_done, self);
}

/* --- connecting to the daemon ----------------------------------------------------- */

static void
on_proxy_properties_changed(GDBusProxy *proxy, GVariant *changed, GStrv invalidated,
                            gpointer user_data)
{
    (void) proxy;
    (void) changed;
    (void) invalidated;
    read_state(user_data);
}

static void
on_proxy_owner_changed(GObject *object, GParamSpec *pspec, gpointer user_data)
{
    (void) object;
    (void) pspec;
    read_state(user_data); /* the daemon started, stopped or restarted */
}

static void connect_service(ExtrasMenuPowerProfile *self, guint index);

static void
on_proxy_ready(GObject *source, GAsyncResult *result, gpointer user_data)
{
    (void) source;

    GError *error = NULL;
    GDBusProxy *proxy = g_dbus_proxy_new_for_bus_finish(result, &error);

    /* Cancelled means the plugin is going away: `self` may be gone. */
    if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    {
        g_clear_error(&error);
        return;
    }
    g_clear_error(&error);

    ExtrasMenuPowerProfile *self = user_data;

    gchar *owner = proxy != NULL ? g_dbus_proxy_get_name_owner(proxy) : NULL;
    if (owner == NULL)
    {
        /* Nothing under this name: try the next one, and if there is
         * none the pill stays greyed out. */
        g_clear_object(&proxy);
        if (self->service_index + 1 < G_N_ELEMENTS(SERVICES))
        {
            connect_service(self, self->service_index + 1);
            return;
        }
        read_state(self);
        return;
    }
    g_free(owner);

    self->proxy = proxy;
    g_signal_connect(proxy, "g-properties-changed",
                     G_CALLBACK(on_proxy_properties_changed), self);
    g_signal_connect(proxy, "notify::g-name-owner",
                     G_CALLBACK(on_proxy_owner_changed), self);
    read_state(self);
}

static void
connect_service(ExtrasMenuPowerProfile *self, guint index)
{
    self->service_index = index;

    /* Asking for the proxy also starts the daemon if it is installed
     * and only waiting to be activated. */
    g_dbus_proxy_new_for_bus(G_BUS_TYPE_SYSTEM, G_DBUS_PROXY_FLAGS_NONE, NULL,
                             SERVICES[index].name, SERVICES[index].path,
                             SERVICES[index].interface,
                             self->cancellable, on_proxy_ready, self);
}

/* --- setup and teardown ------------------------------------------------------------ */

/* The pill is a toggle button holding a box with an icon and a label
 * (see extras_menu_make_pill_toggle() in popover.c). Finds those two. */
static gboolean
find_pill_parts(GtkWidget *pill, GtkWidget **icon, GtkWidget **label)
{
    *icon = NULL;
    *label = NULL;

    if (!GTK_IS_BIN(pill))
        return FALSE;

    GtkWidget *box = gtk_bin_get_child(GTK_BIN(pill));
    if (box == NULL || !GTK_IS_CONTAINER(box))
        return FALSE;

    GList *children = gtk_container_get_children(GTK_CONTAINER(box));
    for (GList *l = children; l != NULL; l = l->next)
    {
        if (*icon == NULL && GTK_IS_IMAGE(l->data))
            *icon = l->data;
        else if (*label == NULL && GTK_IS_LABEL(l->data))
            *label = l->data;
    }
    g_list_free(children);

    return *icon != NULL && *label != NULL;
}

ExtrasMenuPowerProfile *
extras_menu_power_profile_new(GtkWidget *pill)
{
    GtkWidget *icon = NULL;
    GtkWidget *label = NULL;

    if (pill == NULL || !GTK_IS_TOGGLE_BUTTON(pill) || !find_pill_parts(pill, &icon, &label))
        return NULL;

    ExtrasMenuPowerProfile *self = g_new0(ExtrasMenuPowerProfile, 1);
    self->pill = g_object_ref(pill);
    self->icon = g_object_ref(icon);
    self->label = g_object_ref(label);
    self->cancellable = g_cancellable_new();
    self->profiles = g_ptr_array_new_with_free_func(g_free);

    /* Clicking cycles the profile, so what the toggle does by itself is
     * not wanted: only "clicked" is used, and refresh() sets its look. */
    self->clicked_id = g_signal_connect(pill, "clicked", G_CALLBACK(on_pill_clicked), self);

    /* Greyed out until the daemon has answered. */
    refresh(self);
    connect_service(self, 0);

    return self;
}

void
extras_menu_power_profile_free(ExtrasMenuPowerProfile *self)
{
    if (self == NULL)
        return;

    /* Ends every pending call; their callbacks see "cancelled" and
     * return without touching `self`. */
    g_cancellable_cancel(self->cancellable);
    g_object_unref(self->cancellable);

    if (self->proxy != NULL)
    {
        g_signal_handlers_disconnect_by_data(self->proxy, self);
        g_object_unref(self->proxy);
    }

    g_signal_handler_disconnect(self->pill, self->clicked_id);
    g_object_unref(self->label);
    g_object_unref(self->icon);
    g_object_unref(self->pill);

    g_ptr_array_unref(self->profiles);
    g_free(self->active);
    g_free(self);
}