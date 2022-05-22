/*
** PurcmcCallbacks.c -- The implementation of callbacks for PurCMC.
**
** Copyright (C) 2022 FMSoft <http://www.fmsoft.cn>
**
** Author: Vincent Wei <https://github.com/VincentWei>
**
** This file is part of xGUI Pro, an advanced HVML renderer.
**
** xGUI Pro is free software: you can redistribute it and/or modify
** it under the terms of the GNU General Public License as published by
** the Free Software Foundation, either version 3 of the License, or
** (at your option) any later version.
**
** xGUI Pro is distributed in the hope that it will be useful,
** but WITHOUT ANY WARRANTY; without even the implied warranty of
** MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
** GNU General Public License for more details.
** You should have received a copy of the GNU General Public License
** along with this program.  If not, see http://www.gnu.org/licenses/.
*/

#include "config.h"
#include "main.h"
#include "BrowserWindow.h"
#include "BuildRevision.h"
#include "PurcmcCallbacks.h"
#include "HVMLURISchema.h"

#include "purcmc/purcmc.h"

#include "utils/kvlist.h"
#include "utils/sorted-array.h"

#include <errno.h>
#include <assert.h>
#include <gtk/gtk.h>
#include <string.h>
#include <webkit2/webkit2.h>

/* handle types */
enum {
    HT_WORKSPACE = 0,
    HT_PLAINWIN,
    HT_WEBVIEW,
};

struct purcmc_plainwin {
    char *name;
    char *title;

    BrowserWindow *mainWin;
    WebKitWebView *webView;
};

struct purcmc_workspace {
    /* ungrouped plain windows */
    struct kvlist       ug_wins;
};

struct purcmc_session {
    WebKitSettings *webkitSettings;
    WebKitWebContext *webContext;

    /* the sorted array of all valid handles */
    struct sorted_array *all_handles;

    /* the only workspace */
    purcmc_workspace workspace;

    /* the URI prefix: hvml://<hostName>/<appName>/<runnerName>/ */
    char *uriPrefix;
};

purcmc_session *gtk_create_session(void *context, purcmc_endpoint *endpt)
{
    purcmc_session* sess = calloc(1, sizeof(purcmc_session));
    sess->uriPrefix = purc_assemble_hvml_uri_alloc(
            purcmc_endpoint_host_name(endpt),
            purcmc_endpoint_app_name(endpt),
            purcmc_endpoint_runner_name(endpt),
            NULL);
    if (sess->uriPrefix == NULL) {
        free(sess);
        return NULL;
    }

    LOG_DEBUG("URI prefix: %s\n", sess->uriPrefix);

    sess->all_handles = sorted_array_create(SAFLAG_DEFAULT, 8, NULL, NULL);
    if (sess->all_handles == NULL) {
        free(sess);
        return NULL;
    }

    WebKitSettings *webkitSettings = context;
    WebKitWebsiteDataManager *manager;
    manager = g_object_get_data(G_OBJECT(webkitSettings),
            "default-website-data-manager");

    WebKitWebContext *webContext = g_object_new(WEBKIT_TYPE_WEB_CONTEXT,
            "website-data-manager", manager,
            "process-swap-on-cross-site-navigation-enabled", TRUE,
#if !GTK_CHECK_VERSION(3, 98, 0)
            "use-system-appearance-for-scrollbars", FALSE,
#endif
            NULL);
    g_object_unref(manager);

    g_signal_connect(webContext, "initialize-web-extensions",
            G_CALLBACK(initializeWebExtensionsCallback), NULL);

#if 0
    if (enableSandbox)
        webkit_web_context_set_sandbox_enabled(webContext, TRUE);

    if (cookiesPolicy) {
        WebKitCookieManager *cookieManager = webkit_web_context_get_cookie_manager(webContext);
        GEnumClass *enumClass = g_type_class_ref(WEBKIT_TYPE_COOKIE_ACCEPT_POLICY);
        GEnumValue *enumValue = g_enum_get_value_by_nick(enumClass, cookiesPolicy);
        if (enumValue)
            webkit_cookie_manager_set_accept_policy(cookieManager, enumValue->value);
        g_type_class_unref(enumClass);
    }

    if (cookiesFile && !webkit_web_context_is_ephemeral(webContext)) {
        WebKitCookieManager *cookieManager = webkit_web_context_get_cookie_manager(webContext);
        WebKitCookiePersistentStorage storageType = g_str_has_suffix(cookiesFile, ".txt") ? WEBKIT_COOKIE_PERSISTENT_STORAGE_TEXT : WEBKIT_COOKIE_PERSISTENT_STORAGE_SQLITE;
        webkit_cookie_manager_set_persistent_storage(cookieManager, cookiesFile, storageType);
    }

    // Enable the favicon database, by specifying the default directory.
    webkit_web_context_set_favicon_database_directory(webContext, NULL);

    webkit_web_context_register_uri_scheme(webContext, BROWSER_ABOUT_SCHEME, (WebKitURISchemeRequestCallback)aboutURISchemeRequestCallback, webContext, NULL);

    WebKitUserContentManager *userContentManager = webkit_user_content_manager_new();
    webkit_user_content_manager_register_script_message_handler(userContentManager, "aboutData");
    g_signal_connect(userContentManager, "script-message-received::aboutData", G_CALLBACK(aboutDataScriptMessageReceivedCallback), webContext);

    WebKitWebsitePolicies *defaultWebsitePolicies = webkit_website_policies_new_with_policies(
        "autoplay", autoplayPolicy,
        NULL);
#endif

    // hvml schema
    webkit_web_context_register_uri_scheme(webContext,
            BROWSER_HVML_SCHEME,
            (WebKitURISchemeRequestCallback)hvmlURISchemeRequestCallback,
            webContext, NULL);

    sess->webkitSettings = webkitSettings;
    sess->webContext = webContext;

    kvlist_init(&sess->workspace.ug_wins, NULL);
    return sess;
}

static void
do_destroy_plainwin(purcmc_session *sess, purcmc_plainwin *plainWin);

int gtk_remove_session(purcmc_session *sess)
{
    const char *name;
    void *next, *data;
    purcmc_plainwin *plainWin;

    LOG_DEBUG("removing session (%p)...\n", sess);

    LOG_DEBUG("destroy all plain windows...\n");
    kvlist_for_each_safe(&sess->workspace.ug_wins, name, next, data) {
        plainWin = *(purcmc_plainwin **)data;

        do_destroy_plainwin(sess, plainWin);
    }

    LOG_DEBUG("destroy all kvlist for ungrouped plain windows...\n");
    kvlist_free(&sess->workspace.ug_wins);

    LOG_DEBUG("destroy sorted array for all handles...\n");
    sorted_array_destroy(sess->all_handles);

    LOG_DEBUG("clear webContext...\n");
    g_clear_object(&sess->webContext);

    LOG_DEBUG("free session...\n");
    free(sess);

    LOG_DEBUG("done\n");
    return PCRDR_SC_OK;
}

purcmc_plainwin *gtk_create_plainwin(purcmc_session *sess,
        purcmc_workspace *workspace,
        const char *gid,
        const char *name, const char *title, purc_variant_t properties,
        int *retv)
{
    purcmc_plainwin * plainWin = NULL;

    assert(workspace == NULL);

    if ((plainWin = calloc(1, sizeof(*plainWin))) == NULL) {
        *retv = PCRDR_SC_INSUFFICIENT_STORAGE;
        goto failed;
    }

    if (gid == NULL) {
        /* create a ungrouped plain window */
        LOG_DEBUG("try creating a plain window with name (%s)\n", name);

        if (kvlist_get(&sess->workspace.ug_wins, name)) {
            LOG_WARN("Duplicated ungrouped plain window: %s\n", name);
            *retv = PCRDR_SC_CONFLICT;
            goto failed;
        }

        plainWin->name = strdup(name);
        if (title)
            plainWin->title = strdup(title);

        BrowserWindow *mainWin;
        mainWin = BROWSER_WINDOW(browser_window_new(NULL, sess->webContext));

        GtkApplication *application;
        application = g_object_get_data(G_OBJECT(sess->webkitSettings),
                "gtk-application");

        gtk_application_add_window(GTK_APPLICATION(application),
                GTK_WINDOW(mainWin));

        purc_variant_t tmp;
        if ((tmp = purc_variant_object_get_by_ckey(properties, "darkMode")) &&
                purc_variant_is_true(tmp)) {
            g_object_set(gtk_widget_get_settings(GTK_WIDGET(mainWin)),
                    "gtk-application-prefer-dark-theme", TRUE, NULL);
        }

        if ((tmp = purc_variant_object_get_by_ckey(properties, "fullScreen")) &&
                purc_variant_is_true(tmp)) {
            gtk_window_fullscreen(GTK_WINDOW(mainWin));
        }

        if ((tmp = purc_variant_object_get_by_ckey(properties, "backgroundColor"))) {
            const char *value = purc_variant_get_string_const(tmp);

            GdkRGBA rgba;
            if (gdk_rgba_parse(&rgba, value)) {
                browser_window_set_background_color(mainWin, &rgba);
            }
        }

        WebKitWebsitePolicies *websitePolicies;
        websitePolicies = g_object_get_data(G_OBJECT(sess->webkitSettings),
                "default-website-policies");

        WebKitUserContentManager *userContentManager;
        userContentManager = g_object_get_data(G_OBJECT(sess->webkitSettings),
                "default-user-content-manager");

        WebKitWebView *webView = WEBKIT_WEB_VIEW(g_object_new(WEBKIT_TYPE_WEB_VIEW,
            "web-context", sess->webContext,
            "settings", sess->webkitSettings,
            "user-content-manager", userContentManager,
            "is-controlled-by-automation", FALSE,
            "website-policies", websitePolicies,
            NULL));

#if 0
        if (editorMode)
            webkit_web_view_set_editable(webView, TRUE);
#endif

        browser_window_append_view(mainWin, webView);

        char uri[strlen(sess->uriPrefix) + strlen(name) + 2];
        strcpy(uri, sess->uriPrefix);
        strcat(uri, name);
        webkit_web_view_load_uri(webView, uri);

        g_object_unref(sess->webContext);
        g_object_unref(userContentManager);

        gtk_widget_grab_focus(GTK_WIDGET(webView));
        gtk_widget_show(GTK_WIDGET(mainWin));

        plainWin->mainWin = mainWin;
        plainWin->webView = webView;

        LOG_DEBUG("A new plain window created: %p\n", plainWin);
        LOG_DEBUG("A new webView created: %p\n", webView);

        sorted_array_add(sess->all_handles, (uint64_t)(uintptr_t)plainWin,
                (void *)(uintptr_t)HT_PLAINWIN);
        sorted_array_add(sess->all_handles, (uint64_t)(uintptr_t)webView,
                (void *)(uintptr_t)HT_WEBVIEW);
    }
    else {
        /* TODO: create a plain window in the specified group */
        *retv = PCRDR_SC_NOT_IMPLEMENTED;
    }

failed:
    return plainWin;
}

int gtk_update_plainwin(purcmc_session *sess, purcmc_workspace *workspace,
        purcmc_plainwin *plainWin, const char *property, const char *value)
{
    void *data;
    if (!sorted_array_find(sess->all_handles,
                (uint64_t)(uintptr_t)plainWin, &data)) {
        return PCRDR_SC_NOT_FOUND;
    }

    if ((uintptr_t)data != HT_PLAINWIN) {
        return PCRDR_SC_BAD_REQUEST;
    }

    if (strcmp(property, "name") == 0) {
        /* Forbid to change name of a plain window */
        return PCRDR_SC_FORBIDDEN;
    }
    else if (strcmp(property, "title") == 0) {
        if (plainWin->title)
            free(plainWin->title);
        plainWin->title = strdup(value);
    }

    return PCRDR_SC_OK;
}

static void
do_destroy_plainwin(purcmc_session *sess, purcmc_plainwin *plainWin)
{
    sorted_array_remove(sess->all_handles, (uint64_t)(uintptr_t)plainWin);
    kvlist_delete(&sess->workspace.ug_wins, plainWin->name);
    free(plainWin->name);
    webkit_web_view_try_close(plainWin->webView);
    free(plainWin);
}

int gtk_destroy_plainwin(purcmc_session *sess, purcmc_workspace *workspace,
        purcmc_plainwin *plainWin)
{
    assert(workspace == NULL);

    void *data;
    if (!sorted_array_find(sess->all_handles,
                (uint64_t)(uintptr_t)plainWin, &data)) {
        return PCRDR_SC_NOT_FOUND;
    }

    if ((uintptr_t)data != HT_PLAINWIN) {
        return PCRDR_SC_BAD_REQUEST;
    }

    do_destroy_plainwin(sess, plainWin);
    return PCRDR_SC_OK;
}

purcmc_page *gtk_get_plainwin_page(purcmc_session *sess,
        purcmc_plainwin *plainWin, int *retv)
{
    LOG_DEBUG("get page of plainwin (%p)...\n", plainWin);

    void *data;
    if (!sorted_array_find(sess->all_handles,
                (uint64_t)(uintptr_t)plainWin, &data)) {
        *retv = PCRDR_SC_NOT_FOUND;
        LOG_DEBUG("not found\n");
        return NULL;
    }

    if ((uintptr_t)data != HT_PLAINWIN) {
        LOG_DEBUG("not a plain window\n");
        *retv = PCRDR_SC_BAD_REQUEST;
        return NULL;
    }

    *retv = PCRDR_SC_OK;
    return (purcmc_page *)plainWin->webView;
}

static inline WebKitWebView *validate_page(purcmc_session *sess,
        purcmc_page *page, int *retv)
{
    void *data;
    if (!sorted_array_find(sess->all_handles,
                (uint64_t)(uintptr_t)page, &data)) {
        *retv = PCRDR_SC_NOT_FOUND;
        LOG_DEBUG("not found\n");
        return NULL;
    }

    if ((uintptr_t)data != HT_WEBVIEW) {
        *retv = PCRDR_SC_BAD_REQUEST;
        LOG_DEBUG("page is not a WebView\n");
        return NULL;
    }

    return (WebKitWebView *)page;
}

static void
request_ready_callback(GObject* obj, GAsyncResult* result, gpointer userData)
{
    WebKitWebView *webView = (WebKitWebView*)userData;

    WebKitUserMessage * message;
    message = webkit_web_view_send_message_to_page_finish(webView, result, NULL);

    if (message) {
        const char *name = webkit_user_message_get_name(message);
        LOG_DEBUG("the name of message: %s\n", name);

        GVariant *param = webkit_user_message_get_parameters(message);
        const char* type = g_variant_get_type_string(param);
        LOG_DEBUG("The parameter type of the message: %s\n", type);
        if (strcmp(type, "s")) {
            LOG_DEBUG("The parameter: %s\n",
                    g_variant_get_string(param, NULL));
        }
    }
}

#define PAGE_MESSAGE_FORMAT  "{"    \
        "\"operation\":\"%s\","     \
        "\"requestId\":\"%s\","     \
        "\"data\":\"%s\"}"

purcmc_dom *gtk_load_or_write(purcmc_session *sess, purcmc_page *page,
            int op, const char *op_name, const char* request_id,
            const char *content, size_t length, int *retv)
{
    LOG_DEBUG("page: (%p)\n", page);

    WebKitWebView *webView = validate_page(sess, page, retv);
    if (webView == NULL)
        return NULL;

    char *escaped = pcutils_escape_string_for_json(content);
    gchar *json = g_strdup_printf(PAGE_MESSAGE_FORMAT, op_name,
            request_id, escaped);

    WebKitUserMessage * message = webkit_user_message_new(op_name,
            g_variant_new_string(json));
    g_free(json);
    free(escaped);

    LOG_DEBUG("Sending message to page (%p)\n", page);
    webkit_web_view_send_message_to_page(webView, message, NULL,
            request_ready_callback, webView);

    *retv = PCRDR_SC_OK;
    return (purcmc_dom *)webView;
}

#define DOM_MESSAGE_FORMAT  "{"     \
        "\"operation\":\"%s\","     \
        "\"requestId\":\"%s\","     \
        "\"elementType\":\"%s\","   \
        "\"element\":\"%s\","  \
        "\"property\":\"%s\","  \
        "\"data\":\"%s\"}"

int gtk_update_dom(purcmc_session *sess, purcmc_dom *dom,
            int op, const char *op_name, const char* request_id,
            const char* element_type, const char* element_value,
            const char* property,
            const char *content, size_t length)
{
    int retv = PCRDR_SC_OK;

    LOG_DEBUG("DOM: (%p)\n", dom);

    WebKitWebView *webView = validate_page(sess, (purcmc_page *)dom, &retv);
    if (webView == NULL)
        return retv;

    char *escaped = pcutils_escape_string_for_json(content);
    gchar *json = g_strdup_printf(DOM_MESSAGE_FORMAT, op_name, request_id,
            element_type, element_value, property,
            escaped);

    WebKitUserMessage * message = webkit_user_message_new(op_name,
            g_variant_new_string(json));
    g_free(json);
    free(escaped);

    LOG_DEBUG("Sending message to dom (%p)\n", dom);
    webkit_web_view_send_message_to_page(webView, message, NULL,
            request_ready_callback, webView);

    return retv;
}

