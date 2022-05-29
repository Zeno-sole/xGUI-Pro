/*
** HVMLURISchema.c -- implementation of hvml URI schema.
**
** Copyright (C) 2022 FMSoft (http://www.fmsoft.cn)
**
** Author: Vincent Wei (https://github.com/VincentWei)
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
#include "HVMLURISchema.h"
#include "BuildRevision.h"

#include "utils/hvml-uri.h"
#include "utils/load-asset.h"

#include <webkit2/webkit2.h>
#include <purc/purc-pcrdr.h>
#include <purc/purc-helpers.h>

#include <assert.h>

void initializeWebExtensionsCallback(WebKitWebContext *context,
        gpointer user_data)
{
    const char *webext_dir = g_getenv("WEBKIT_WEBEXT_DIR");
    if (webext_dir == NULL) {
        webext_dir = WEBKIT_WEBEXT_DIR;
    }

    webkit_web_context_set_web_extensions_directory(context, webext_dir);
    webkit_web_context_set_web_extensions_initialization_user_data(context,
            g_variant_new_string("HVML"));
}

void hvmlURISchemeRequestCallback(WebKitURISchemeRequest *request,
        WebKitWebContext *webContext)
{
    const char *uri = webkit_uri_scheme_request_get_uri(request);
    char host[PURC_LEN_HOST_NAME + 1];
    char app[PURC_LEN_APP_NAME + 1];
    char runner[PURC_LEN_RUNNER_NAME + 1];

    if (!purc_hvml_uri_split(uri,
            host, app, runner, NULL, NULL) ||
            !purc_is_valid_host_name(host) ||
            !purc_is_valid_app_name(app) ||
            !purc_is_valid_runner_name(runner)) {
        GError *error = g_error_new(XGUI_PRO_ERROR,
                XGUI_PRO_ERROR_INVALID_HVML_URI,
                "Invalid HVML URI (%s): bad host, app, or runner name", uri);
        webkit_uri_scheme_request_finish_error(request, error);
        g_error_free(error);
        return;
    }

    char *group = NULL;
    char *page = NULL;
    if (!purc_hvml_uri_split_alloc(uri, NULL, NULL, NULL, &group, &page)) {
        GError *error = g_error_new(XGUI_PRO_ERROR,
                XGUI_PRO_ERROR_INVALID_HVML_URI,
                "Invalid HVML URI (%s): bad group or page name", uri);
        webkit_uri_scheme_request_finish_error(request, error);
        g_error_free(error);
        return;
    }

    char *initial_request_id = NULL;
    if (!purc_hvml_uri_get_query_value_alloc(uri,
                "irId", &initial_request_id) ||
            !purc_is_valid_unique_id(initial_request_id)) {
        GError *error = g_error_new(XGUI_PRO_ERROR,
                XGUI_PRO_ERROR_INVALID_HVML_URI,
                "Invalid HVML URI (%s): bad initial request identifier", uri);
        webkit_uri_scheme_request_finish_error(request, error);
        g_error_free(error);
        return;
    }

    gchar *contents, *content_type = NULL;
    gsize content_length;

    /* check if it is an asset was built in the renderer */
    if (strcmp(host, PCRDR_LOCALHOST) == 0 &&
            strcmp(app, PCRDR_APP_RENDERER) == 0 &&
            strcmp(runner, PCRDR_RUNNER_BUILTIN) == 0 &&
            strcmp(group, PCRDR_GROUP_NULL) == 0) {

        contents = load_asset_content("WEBKIT_WEBEXT_DIR", WEBKIT_WEBEXT_DIR,
                page, &content_length);
        if (contents == NULL) {
            GError *error = g_error_new(XGUI_PRO_ERROR,
                    XGUI_PRO_ERROR_INVALID_HVML_URI,
                    "Can not load contents from asset file (%s)", page);
            webkit_uri_scheme_request_finish_error(request, error);
            g_error_free(error);
            return;
        }

        gboolean result_uncertain;
        content_type = g_content_type_guess(page,
                (const guchar *)contents, content_length, &result_uncertain);
        if (result_uncertain) {
            g_free(content_type);
            content_type = NULL;
        }
    }
    else {
        contents = g_strdup_printf(
                "<!DOCTYPE html>"
                "<html>"
                "<body>"
                "<h1>xGUI Pro - an advanced HVML renderer</h1>"
                "<p>Status: <strong hvml-handle=\"731128\">Checking...</strong>.</p>"
                "<p>This content will be replaced by the HVML runner <span hvml-handle=\"790715\"></span>.</p>"
                "<p><small>WebKit2GTK API Version %s, WebKit Version %d.%d.%d, Build %s</small></p>"
                "</body>"
                "</html>",
                WEBKITGTK_API_VERSION_STRING,
                WEBKIT_MAJOR_VERSION, WEBKIT_MINOR_VERSION, WEBKIT_MICRO_VERSION,
                BUILD_REVISION);
        content_length = strlen(contents);
        content_type = g_strdup("text/html");
    }

    GInputStream *stream;
    stream = g_memory_input_stream_new_from_data(contents, content_length, g_free);
    webkit_uri_scheme_request_finish(request, stream, content_length,
            content_type ? content_type : "application/octet-stream");
    if (content_type) g_free(content_type);
    g_object_unref(stream);
}

