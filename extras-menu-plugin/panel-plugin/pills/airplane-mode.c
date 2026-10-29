#include "airplane-mode.h"

/* How long after a click the module waits for both radios to reach the
 * requested state before it stops waiting and shows what is really
 * going on (a radio that refuses to switch, a backend that never
 * answers). */
#define SETTLE_TIMEOUT_MS 3000

struct _ExtrasMenuAirplaneMode
{
    /* NULL once the toggle has been destroyed (weak pointer). */
    GtkWidget *toggle;

    ExtrasMenuAirplaneModeSetRadioFunc set_wifi;
    ExtrasMenuAirplaneModeSetRadioFunc set_bluetooth;
    ExtrasMenuAirplaneModeChangedFunc changed;
    gpointer user_data;

    /* The on/off value last reported through `changed`. */
    gboolean last_notified;

    /* Latest state heard from the backends. */
    gboolean wifi_known;
    gboolean wifi_enabled;
    gboolean bluetooth_known;
    gboolean bluetooth_available;
    gboolean bluetooth_powered;

    /* What was on just before aeroplane mode switched things off, so
     * turning it off again brings back the same radios. Both start TRUE
     * so that after a plugin restart while aeroplane mode was already
     * on (nothing remembered), turning it off brings back everything. */
    gboolean restore_wifi;
    gboolean restore_bluetooth;

    /* A click has been sent to the backends and their answers are still
     * on the way. The radios change one after the other, so while this
     * is set the toggle keeps showing what the user asked for instead of
     * flickering through the half-way state. */
    gboolean request_active;
    gboolean target_wifi;
    gboolean target_bluetooth;
    guint settle_source_id;

    /* TRUE from a click that switched aeroplane mode on until the user
     * switches it off again or a radio comes back on by other means.
     * Without it, merely turning Wi-Fi off while Bluetooth was already
     * off (or absent) would look like aeroplane mode. */
    gboolean engaged;

    /* TRUE while the module itself moves the toggle, so its own
     * "toggled" handler doesn't treat that as a click. */
    gboolean updating_from_backend;
};

/* --- reading the state ---------------------------------------------------------- */

/* Whether the radios are in the given state. Without a Bluetooth
 * adapter there is nothing to switch, so Bluetooth matches any target. */
static gboolean
radios_match(const ExtrasMenuAirplaneMode *am, gboolean wifi_target, gboolean bluetooth_target)
{
    if (am->wifi_enabled != wifi_target)
        return FALSE;

    if (!am->bluetooth_available)
        return TRUE;

    return am->bluetooth_powered == bluetooth_target;
}

/* --- keeping the toggle in step ------------------------------------------------- */

static gboolean
toggle_is_active(const ExtrasMenuAirplaneMode *am)
{
    return am->toggle != NULL && gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(am->toggle));
}

/* Tells the owner when what the pill shows has flipped. Cheap to call
 * after anything that might have moved the toggle: it only reports a
 * real change. */
static void
notify_changed(ExtrasMenuAirplaneMode *am)
{
    gboolean now = toggle_is_active(am);

    if (now == am->last_notified)
        return;

    am->last_notified = now;

    if (am->changed != NULL)
        am->changed(now, am->user_data);
}

static void
set_toggle_active(ExtrasMenuAirplaneMode *am, gboolean active)
{
    if (am->toggle == NULL)
        return;

    am->updating_from_backend = TRUE;
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(am->toggle), active);
    am->updating_from_backend = FALSE;

    notify_changed(am);
}

/* Shows the real state: "on" when the user switched aeroplane mode on
 * and both radios are still off. Does nothing until both backends have
 * reported, and while a click is still being carried out. */
static void
sync_toggle(ExtrasMenuAirplaneMode *am)
{
    if (!am->wifi_known || !am->bluetooth_known)
        return;

    if (am->toggle != NULL)
        gtk_widget_set_sensitive(am->toggle, TRUE);

    if (am->request_active)
        return;

    gboolean both_off = radios_match(am, FALSE, FALSE);

    /* A radio is on again (switched elsewhere): aeroplane mode is over. */
    if (!both_off)
        am->engaged = FALSE;

    set_toggle_active(am, am->engaged && both_off);
}

static void
end_request(ExtrasMenuAirplaneMode *am)
{
    if (am->settle_source_id != 0)
    {
        g_source_remove(am->settle_source_id);
        am->settle_source_id = 0;
    }

    am->request_active = FALSE;
    sync_toggle(am);
}

static gboolean
on_settle_timeout(gpointer user_data)
{
    ExtrasMenuAirplaneMode *am = user_data;

    am->settle_source_id = 0; /* returning G_SOURCE_REMOVE removes it */
    end_request(am);
    return G_SOURCE_REMOVE;
}

/* Called after every state change: finishes the running request once
 * both radios are where the click wanted them, otherwise just shows the
 * real state. */
static void
state_changed(ExtrasMenuAirplaneMode *am)
{
    if (am->request_active && radios_match(am, am->target_wifi, am->target_bluetooth))
        end_request(am);
    else
        sync_toggle(am);
}

/* --- the user's click ----------------------------------------------------------- */

static void
on_toggled(GtkToggleButton *button, gpointer user_data)
{
    ExtrasMenuAirplaneMode *am = user_data;

    if (am->updating_from_backend)
        return;

    if (gtk_toggle_button_get_active(button))
    {
        /* Remember what is on now -- unless a previous click is still
         * being carried out, in which case the radios are half-way and
         * what was remembered then is the better answer. */
        if (!am->request_active)
        {
            am->restore_wifi = am->wifi_enabled;
            am->restore_bluetooth = am->bluetooth_available && am->bluetooth_powered;
        }

        am->target_wifi = FALSE;
        am->target_bluetooth = FALSE;
    }
    else
    {
        /* If nothing was on before, bring everything back anyway:
         * otherwise the toggle would switch "off" and immediately
         * show "on" again, because both radios would still be off. */
        gboolean nothing_remembered = !am->restore_wifi && !am->restore_bluetooth;

        am->target_wifi = am->restore_wifi || nothing_remembered;
        am->target_bluetooth = am->restore_bluetooth || nothing_remembered;
    }

    am->engaged = gtk_toggle_button_get_active(button);
    am->request_active = TRUE;

    /* The click itself already moved the toggle; let the owner know now
     * rather than when the radios have caught up. */
    notify_changed(am);

    if (am->settle_source_id != 0)
        g_source_remove(am->settle_source_id);
    am->settle_source_id = g_timeout_add(SETTLE_TIMEOUT_MS, on_settle_timeout, am);

    if (am->set_wifi != NULL)
        am->set_wifi(am->target_wifi, am->user_data);
    if (am->set_bluetooth != NULL)
        am->set_bluetooth(am->target_bluetooth, am->user_data);

    /* A radio that already was where it should be sends no change
     * signal; don't wait for one that will never come. */
    state_changed(am);
}

/* --- public API ----------------------------------------------------------------- */

ExtrasMenuAirplaneMode *
extras_menu_airplane_mode_new(GtkWidget *toggle,
                               ExtrasMenuAirplaneModeSetRadioFunc set_wifi,
                               ExtrasMenuAirplaneModeSetRadioFunc set_bluetooth,
                               ExtrasMenuAirplaneModeChangedFunc changed,
                               gpointer user_data)
{
    g_return_val_if_fail(GTK_IS_TOGGLE_BUTTON(toggle), NULL);

    ExtrasMenuAirplaneMode *am = g_new0(ExtrasMenuAirplaneMode, 1);
    am->toggle = toggle;
    am->set_wifi = set_wifi;
    am->set_bluetooth = set_bluetooth;
    am->changed = changed;
    am->user_data = user_data;
    am->restore_wifi = TRUE;
    am->restore_bluetooth = TRUE;

    /* The toggle can be destroyed before we are freed (the dropdown
     * window going away); the weak pointer then sets am->toggle to NULL
     * instead of leaving it dangling. */
    g_object_add_weak_pointer(G_OBJECT(toggle), (gpointer *) &am->toggle);

    gtk_widget_set_sensitive(toggle, FALSE);
    g_signal_connect(toggle, "toggled", G_CALLBACK(on_toggled), am);

    return am;
}

gboolean
extras_menu_airplane_mode_is_active(const ExtrasMenuAirplaneMode *am)
{
    return am != NULL && toggle_is_active(am);
}

void
extras_menu_airplane_mode_wifi_changed(ExtrasMenuAirplaneMode *am, gboolean enabled)
{
    if (am == NULL)
        return;

    am->wifi_known = TRUE;
    am->wifi_enabled = enabled;
    state_changed(am);
}

void
extras_menu_airplane_mode_bluetooth_changed(ExtrasMenuAirplaneMode *am,
                                             gboolean available, gboolean powered)
{
    if (am == NULL)
        return;

    am->bluetooth_known = TRUE;
    am->bluetooth_available = available;
    am->bluetooth_powered = available && powered;
    state_changed(am);
}

void
extras_menu_airplane_mode_free(ExtrasMenuAirplaneMode *am)
{
    if (am == NULL)
        return;

    if (am->settle_source_id != 0)
        g_source_remove(am->settle_source_id);

    if (am->toggle != NULL)
    {
        g_signal_handlers_disconnect_by_data(am->toggle, am);
        g_object_remove_weak_pointer(G_OBJECT(am->toggle), (gpointer *) &am->toggle);
    }

    g_free(am);
}