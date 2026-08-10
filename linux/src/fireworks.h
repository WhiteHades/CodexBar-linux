#pragma once

#include "config.h"
#include "http.h"
#include "model.h"

typedef CodexBarHttpResponse *(*CodexBarFireworksTransport)(const CodexBarHttpRequest *request, GError **error);

gboolean codexbar_fireworks_account_slug_is_valid(const char *slug);
CodexBarProvider *codexbar_fireworks_parse_summary(const char *json, gint64 now_ms, GError **error);
CodexBarProvider *codexbar_fireworks_parse_summary_bytes(
    const char *json, size_t length, gint64 now_ms, GError **error);
CodexBarProvider *codexbar_fireworks_fetch_with_transport_and_cancellable(const CodexBarProviderConfig *config,
                                                                          CodexBarFireworksTransport transport,
                                                                          GCancellable *cancellable,
                                                                          gint64 now_ms,
                                                                          GError **error);
CodexBarProvider *codexbar_fireworks_fetch_with_cancellable(const CodexBarProviderConfig *config,
                                                            GCancellable *cancellable,
                                                            GError **error);
CodexBarProvider *codexbar_fireworks_fetch(const CodexBarProviderConfig *config, GError **error);
