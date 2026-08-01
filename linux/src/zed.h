#pragma once

#include "config.h"
#include "http.h"
#include "model.h"

typedef CodexBarHttpResponse *(*CodexBarZedTransport)(const CodexBarHttpRequest *request,
                                                      GError **error);

CodexBarProvider *codexbar_zed_parse(const char *json,
                                     size_t length,
                                     gint64 now_ms,
                                     GError **error);
CodexBarProvider *codexbar_zed_fetch_with_transport_and_cancellable(
    const CodexBarProviderConfig *config,
    CodexBarZedTransport transport,
    GCancellable *cancellable,
    gint64 now_ms,
    GError **error);
CodexBarProvider *codexbar_zed_fetch_with_cancellable(const CodexBarProviderConfig *config,
                                                      GCancellable *cancellable,
                                                      GError **error);
