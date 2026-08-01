#pragma once

#include "config.h"
#include "http.h"
#include "model.h"

typedef CodexBarHttpResponse *(*CodexBarOpenCodeGoTransport)(const CodexBarHttpRequest *request,
                                                            GError **error);

CodexBarProvider *codexbar_opencode_go_fetch_from_home(
    const char *home_directory, gint64 now_ms, GError **error);
CodexBarProvider *codexbar_opencode_go_parse_web_usage(const char *text,
                                                       size_t length,
                                                       gint64 now_ms,
                                                       GError **error);
CodexBarProvider *codexbar_opencode_go_fetch_for_source_with_transport_and_cancellable(
    const CodexBarProviderConfig *config,
    const char *source,
    CodexBarOpenCodeGoTransport transport,
    GCancellable *cancellable,
    gint64 now_ms,
    GError **error);
CodexBarProvider *codexbar_opencode_go_fetch_for_source_with_cancellable(
    const CodexBarProviderConfig *config,
    const char *source,
    GCancellable *cancellable,
    GError **error);
CodexBarProvider *codexbar_opencode_go_fetch(GError **error);
