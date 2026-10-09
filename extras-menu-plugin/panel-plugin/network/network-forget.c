/*
 * Network backend: forgetting a network (deleting every saved profile
 * for an SSID).
 */

#include "network-private.h"

#include <string.h>

/* --- forgetting a network (deleting its saved profiles) ---------------- */

typedef struct
{
    ExtrasMenuNetwork *network;
    gchar *ssid;
    ExtrasMenuNetworkConnectResultFunc result_callback;
    gpointer result_user_data;

    gchar **profile_paths; /* NULL-terminated */
    guint index;

    guint deleted_count;
    gboolean had_error;
    gchar *first_error; /* owned; first failure's message, for reporting */
} ForgetCtx;

static void forget_check_next(ForgetCtx *ctx);

static void
free_forget_ctx(ForgetCtx *ctx)
{
    g_free(ctx->ssid);
    g_free(ctx->first_error);
    g_strfreev(ctx->profile_paths);
    g_free(ctx);
}

static void
finish_forget(ForgetCtx *ctx)
{
    if (ctx->result_callback != NULL)
    {
        if (ctx->deleted_count == 0 && !ctx->had_error)
        {
            /* Nothing matched -- treat as success rather than an error:
             * the end state the user asked for ("this network isn't
             * saved anymore") is already true. */
            ctx->result_callback(TRUE, NULL, ctx->result_user_data);
        }
        else if (ctx->had_error)
        {
            ctx->result_callback(FALSE,
                                  ctx->first_error != NULL ? ctx->first_error : "Unknown error",
                                  ctx->result_user_data);
        }
        else
        {
            ctx->result_callback(TRUE, NULL, ctx->result_user_data);
        }
    }

    free_forget_ctx(ctx);
}

static void
on_profile_deleted(GObject *source, GAsyncResult *result, gpointer user_data)
{
    ForgetCtx *ctx = user_data;
    GError *error = NULL;

    GVariant *reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), result, &error);
    if (reply == NULL && call_was_cancelled(error))
    {
        g_clear_error(&error);
        free_forget_ctx(ctx);
        return;
    }

    if (reply != NULL)
    {
        ctx->deleted_count++;
        g_variant_unref(reply);
    }
    else
    {
        ctx->had_error = TRUE;
        if (ctx->first_error == NULL && error != NULL)
            ctx->first_error = g_strdup(error->message);
    }
    g_clear_error(&error);

    ctx->index++;
    forget_check_next(ctx);
}

static void
on_forget_profile_settings_finished(GObject *source, GAsyncResult *result, gpointer user_data)
{
    ForgetCtx *ctx = user_data;
    GError *error = NULL;

    GVariant *reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), result, &error);
    if (reply == NULL && call_was_cancelled(error))
    {
        g_clear_error(&error);
        free_forget_ctx(ctx);
        return;
    }

    gboolean matches = FALSE;

    if (reply != NULL)
    {
        GVariant *settings = NULL;
        g_variant_get(reply, "(@a{sa{sv}})", &settings);

        GVariant *wifi_section = g_variant_lookup_value(settings, "802-11-wireless", G_VARIANT_TYPE("a{sv}"));
        if (wifi_section != NULL)
        {
            GVariant *ssid_v = g_variant_lookup_value(wifi_section, "ssid", G_VARIANT_TYPE("ay"));
            if (ssid_v != NULL)
            {
                gchar *found_ssid = network_decode_ssid(ssid_v);
                matches = (g_strcmp0(found_ssid, ctx->ssid) == 0);
                g_free(found_ssid);
                g_variant_unref(ssid_v);
            }
            g_variant_unref(wifi_section);
        }

        g_variant_unref(settings);
        g_variant_unref(reply);
    }
    g_clear_error(&error);

    if (matches)
    {
        /* Delete this one, then carry on through the rest -- see
         * extras_menu_network_forget()'s doc comment for why we don't
         * stop at the first match. */
        g_dbus_connection_call(
            ctx->network->system_bus,
            NM_BUS_NAME,
            ctx->profile_paths[ctx->index],
            NM_SETTINGS_CONNECTION_IFACE,
            "Delete",
            NULL,
            NULL,
            G_DBUS_CALL_FLAGS_NONE,
            -1, ctx->network->cancellable,
            on_profile_deleted, ctx);
        return;
    }

    ctx->index++;
    forget_check_next(ctx);
}

static void
forget_check_next(ForgetCtx *ctx)
{
    if (ctx->profile_paths == NULL || ctx->profile_paths[ctx->index] == NULL)
    {
        finish_forget(ctx);
        return;
    }

    g_dbus_connection_call(
        ctx->network->system_bus,
        NM_BUS_NAME,
        ctx->profile_paths[ctx->index],
        NM_SETTINGS_CONNECTION_IFACE,
        "GetSettings",
        NULL,
        G_VARIANT_TYPE("(a{sa{sv}})"),
        G_DBUS_CALL_FLAGS_NONE,
        -1, ctx->network->cancellable,
        on_forget_profile_settings_finished, ctx);
}

static void
on_forget_list_connections_finished(GObject *source, GAsyncResult *result, gpointer user_data)
{
    ForgetCtx *ctx = user_data;
    GError *error = NULL;

    GVariant *reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), result, &error);
    if (reply == NULL && call_was_cancelled(error))
    {
        g_clear_error(&error);
        free_forget_ctx(ctx);
        return;
    }

    if (reply == NULL)
    {
        ctx->had_error = TRUE;
        if (error != NULL)
            ctx->first_error = g_strdup(error->message);
        g_clear_error(&error);
        finish_forget(ctx);
        return;
    }

    GVariant *paths_v = NULL;
    g_variant_get(reply, "(@ao)", &paths_v);

    guint n = (guint) g_variant_n_children(paths_v);
    ctx->profile_paths = g_new0(gchar *, n + 1); /* NULL-terminated */
    for (guint i = 0; i < n; i++)
    {
        GVariant *child = g_variant_get_child_value(paths_v, i);
        ctx->profile_paths[i] = g_variant_dup_string(child, NULL);
        g_variant_unref(child);
    }

    g_variant_unref(paths_v);
    g_variant_unref(reply);

    ctx->index = 0;
    forget_check_next(ctx);
}

void
extras_menu_network_forget(ExtrasMenuNetwork *network,
                            const gchar *ssid,
                            ExtrasMenuNetworkConnectResultFunc result_callback,
                            gpointer result_user_data)
{
    if (network == NULL || network->system_bus == NULL || ssid == NULL)
    {
        if (result_callback != NULL)
            result_callback(FALSE, "Wi-Fi not available", result_user_data);
        return;
    }

    ForgetCtx *ctx = g_new0(ForgetCtx, 1);
    ctx->network = network;
    ctx->ssid = g_strdup(ssid);
    ctx->result_callback = result_callback;
    ctx->result_user_data = result_user_data;

    g_dbus_connection_call(
        network->system_bus,
        NM_BUS_NAME,
        NM_SETTINGS_OBJ_PATH,
        NM_SETTINGS_IFACE,
        "ListConnections",
        NULL,
        G_VARIANT_TYPE("(ao)"),
        G_DBUS_CALL_FLAGS_NONE,
        -1, network->cancellable,
        on_forget_list_connections_finished, ctx);
}