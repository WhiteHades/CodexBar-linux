#pragma once

#include "config.h"
#include "http.h"
#include "model.h"

typedef CodexBarHttpResponse *(*CodexBarClinePassTransport)(const CodexBarHttpRequest *request, GError **error);

CodexBarProvider *codexbar_clinepass_parse_usage(const char *json, gint64 now_ms, GError **error);
CodexBarProvider *codexbar_clinepass_parse_usage_bytes(
    const char *json, size_t length, gint64 now_ms, GError **error);
CodexBarProvider *codexbar_clinepass_fetch_with_transport(const CodexBarProviderConfig *config,
                                                          CodexBarClinePassTransport transport,
                                                          gint64 now_ms,
                                                          GError **error);
CodexBarProvider *codexbar_clinepass_fetch(const CodexBarProviderConfig *config, GError **error);
