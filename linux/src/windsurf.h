#pragma once

#include "config.h"
#include "http.h"
#include "model.h"

typedef CodexBarHttpResponse *(*CodexBarWindsurfTransport)(const CodexBarHttpRequest *request,
                                                          GError **error);

CodexBarProvider *codexbar_windsurf_parse(const guint8 *data,
                                          size_t length,
                                          gint64 now_ms,
                                          GError **error);
CodexBarProvider *codexbar_windsurf_parse_cached_plan(const char *json,
                                                      size_t length,
                                                      gint64 now_ms,
                                                      GError **error);
CodexBarProvider *codexbar_windsurf_fetch_local(const char *database_path,
                                                gint64 now_ms,
                                                GError **error);
CodexBarProvider *codexbar_windsurf_fetch_for_source_with_transport_and_cancellable(
    const CodexBarProviderConfig *config,
    const char *source,
    CodexBarWindsurfTransport transport,
    GCancellable *cancellable,
    gint64 now_ms,
    GError **error);
CodexBarProvider *codexbar_windsurf_fetch_for_source_with_cancellable(
    const CodexBarProviderConfig *config,
    const char *source,
    GCancellable *cancellable,
    GError **error);
CodexBarProvider *codexbar_windsurf_fetch_with_transport_and_cancellable(
    const CodexBarProviderConfig *config,
    CodexBarWindsurfTransport transport,
    GCancellable *cancellable,
    gint64 now_ms,
    GError **error);
CodexBarProvider *codexbar_windsurf_fetch_with_cancellable(const CodexBarProviderConfig *config,
                                                           GCancellable *cancellable,
                                                           GError **error);
