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

#include <webkit2/webkit2.h>
#include <purc/purc-helpers.h>

#include <assert.h>

void initializeWebExtensionsCallback(WebKitWebContext *context,
        gpointer user_data)
{
    /* Web Extensions get a different ID for each Web Process */
    static guint32 unique_id = 0;
    const char *webext_dir = g_getenv("WEBKIT_WEBEXT_DIR");
    if (webext_dir == NULL) {
        webext_dir = WEBKIT_WEBEXT_DIR;
    }

    g_print("%s: webext in %s\n", __func__, webext_dir);

    webkit_web_context_set_web_extensions_directory(context, webext_dir);
    webkit_web_context_set_web_extensions_initialization_user_data(context,
            g_variant_new_uint32(unique_id++));
}

void hvmlURISchemeRequestCallback(WebKitURISchemeRequest *request,
        WebKitWebContext *webContext)
{
    const char *uri = webkit_uri_scheme_request_get_uri(request);
    char *host = NULL, *app = NULL, *runner = NULL;

#ifndef NDEBUG
    do {
        const char *bad_hvml_uri[] = {
            "http://",
            "hvml://",
            "hvml://host",
            "hvml://host/app",
            "hvml://host/app/runner",
            "hvml://host/app/runner/group/",
            "hvml://host/app/runner/group/page/",
            "hvml://host/app/runner/group/page/trail",
        };

        const char *good_hvml_uri[] = {
            "hvml://host/app/runner/page",
            "hvml://host/app/runner/group/page",
            "HVML://HOST/APP/RUNNER/GROUP/PAGE",
        };

        for (size_t i = 0; i < sizeof(bad_hvml_uri)/sizeof(const char*); i++) {
            bool ret = hvml_uri_split(bad_hvml_uri[i],
                    NULL, NULL, NULL, NULL, NULL);
            assert(!ret);
        }

        for (size_t i = 0; i < sizeof(good_hvml_uri)/sizeof(const char*); i++) {
            char *host, *app, *runner, *group, *page;
            bool ret = hvml_uri_split(good_hvml_uri[i],
                    &host, &app, &runner, &group, &page);
            assert(ret);

            char *my_uri;
            if (group == NULL) {
                my_uri = g_strdup_printf("hvml://%s/%s/%s/%s",
                        host, app, runner, page);
            }
            else {
                my_uri = g_strdup_printf("hvml://%s/%s/%s/%s/%s",
                        host, app, runner, group, page);
            }

            assert(strcasecmp(good_hvml_uri[i], my_uri) == 0);
            g_free(my_uri);

            free(host);
            free(app);
            free(runner);
            if (group)
                free(group);
            free(page);
        }

    } while(0);
#endif

    if (!hvml_uri_split(uri,
            &host, &app, &runner, NULL, NULL) ||
            !purc_is_valid_host_name(host) ||
            !purc_is_valid_app_name(app) ||
            !purc_is_valid_runner_name(runner)) {
        GError *error = g_error_new(XGUI_PRO_ERROR,
                XGUI_PRO_ERROR_INVALID_HVML_URI,
                "Invalid HVML uri: hvml://%s/%s/%s/xxx", host, app, runner);
        webkit_uri_scheme_request_finish_error(request, error);
        g_error_free(error);
        goto error;
    }

    const char *webext_dir = g_getenv("WEBKIT_WEBEXT_DIR");
    if (webext_dir == NULL) {
        webext_dir = WEBKIT_WEBEXT_DIR;
    }

    gchar *contents;
    contents = g_strdup_printf(
            "<!DOCTYPE html>"
            "<html>"
            "<body>"
            "<h1>xGUI Pro - an advanced HVML renderer</h1>"
            "<p>Status: <strong hvml:handle=\"731128\">Checking...</strong>.</p>"
            "<p>This content will be replaced by the HVML runner <span hvml:handle=\"790715\"></span>.</p>"
            "<p><small>WebKit2GTK API Version %s, WebKit Version %d.%d.%d, Build %s</small></p>"
            "</body>"
            "</html>",
            WEBKITGTK_API_VERSION_STRING,
            WEBKIT_MAJOR_VERSION, WEBKIT_MINOR_VERSION, WEBKIT_MICRO_VERSION,
            BUILD_REVISION);

    GInputStream *stream;
    gsize streamLength;
    streamLength = strlen(contents);
    stream = g_memory_input_stream_new_from_data(contents, streamLength, g_free);

    webkit_uri_scheme_request_finish(request, stream, streamLength, "text/html");
    g_object_unref(stream);

error:
    if (runner)
        free(runner);
    if (app)
        free(app);
    if (host)
        free(host);
}

