#pragma once

#include "config.h"
#include "http.h"
#include "model.h"

typedef CodexBarHttpResponse *(*CodexBarKimiTransport)(const CodexBarHttpRequest *request,
                                                       GError **error);

char *codexbar_kimi_usage_url(const char *base_url, GError **error);
CodexBarProvider *codexbar_kimi_parse_usage(const char *json, gint64 now_ms, GError **error);
CodexBarProvider *codexbar_kimi_parse_web_usage(const char *json, gint64 now_ms, GError **error);
CodexBarProvider *codexbar_kimi_fetch_with_transport_and_cancellable(
    const CodexBarProviderConfig *config,
    const char *source,
    CodexBarKimiTransport transport,
    GCancellable *cancellable,
    gint64 now_ms,
    GError **error);
CodexBarProvider *codexbar_kimi_fetch_with_cancellable(const CodexBarProviderConfig *config,
                                                       const char *source,
                                                       GCancellable *cancellable,
                                                       GError **error);
CodexBarProvider *codexbar_kimi_fetch(const CodexBarProviderConfig *config, GError **error);
