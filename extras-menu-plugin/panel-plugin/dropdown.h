#ifndef EXTRAS_MENU_DROPDOWN_H
#define EXTRAS_MENU_DROPDOWN_H

#include "extras-menu.h"

G_BEGIN_DECLS

/* Creates the dropdown window (plugin->popover): a borderless always-on-top
 * GtkWindow with a transparent RGBA frame, and connects the signals that
 * open it from the panel button and close it again (click-away, focus
 * loss, being hidden). plugin->toggle_button must already exist. The
 * caller adds the window's content afterwards. */
void extras_menu_dropdown_create(ExtrasMenuPlugin *plugin);

/* Hides the dropdown and resets the panel button to match. Takes a
 * gpointer (the ExtrasMenuPlugin) so it can be handed to
 * extras_menu_quick_actions_attach() as its close callback. */
void extras_menu_dropdown_close(gpointer user_data);

/* Every dialog the dropdown opens must be registered here right after it
 * is created, so that focus moving to the dialog doesn't close the
 * dropdown it is parented to. When the last dialog is gone, focus goes
 * back to the dropdown. */
void extras_menu_dropdown_track_dialog(ExtrasMenuPlugin *plugin, GtkWidget *dialog);

/* "response" handler for dialogs that only need to go away when answered. */
void extras_menu_dropdown_dialog_response_destroy(GtkDialog *dialog, gint response_id,
                                                   gpointer user_data);

/* Shows a simple non-blocking message dialog with an OK button, parented
 * to the dropdown and tracked. secondary may be NULL. */
void extras_menu_dropdown_show_message(ExtrasMenuPlugin *plugin, GtkMessageType type,
                                        const gchar *primary, const gchar *secondary);

/* Connect this to a context menu's "selection-done" signal (with the
 * plugin as user_data). A context menu takes over the dropdown's pointer
 * grab; this gives it back once the menu is gone, so click-away keeps
 * working. */
void extras_menu_dropdown_context_menu_done(GtkMenuShell *menu, gpointer user_data);

G_END_DECLS

#endif /* EXTRAS_MENU_DROPDOWN_H */