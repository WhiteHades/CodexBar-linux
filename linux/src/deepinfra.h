#pragma once

#include "config.h"
#include "http.h"
#include "model.h"

typedef CodexBarHttpResponse *(*CodexBarDeepInfraTransport)(const CodexBarHttpRequest *request, GError **error);

CodexBarProvider *codexbar_deepinfra_parse_usage(
    const char *checklist_json, const char *usage_json, gint64 now_ms, GError **error);
CodexBarProvider *codexbar_deepinfra_parse_usage_bytes(const char *checklist_json,
                                                       size_t checklist_length,
                                                       const char *usage_json,
                                                       size_t usage_length,
                                                       gint64 now_ms,
                                                       GError **error);
CodexBarProvider *codexbar_deepinfra_fetch_with_transport(const CodexBarProviderConfig *config,
                                                           CodexBarDeepInfraTransport transport,
                                                           gint64 now_ms,
                                                           GError **error);
CodexBarProvider *codexbar_deepinfra_fetch_with_transport_and_cancellable(const CodexBarProviderConfig *config,
                                                                          CodexBarDeepInfraTransport transport,
                                                                          GCancellable *cancellable,
                                                                          gint64 now_ms,
                                                                          GError **error);
double codexbar_deepinfra_retry_delay_for_testing(const CodexBarHttpResponse *response);
CodexBarProvider *codexbar_deepinfra_fetch(const CodexBarProviderConfig *config, GError **error);
CodexBarProvider *codexbar_deepinfra_fetch_with_cancellable(const CodexBarProviderConfig *config,
                                                            GCancellable *cancellable,
                                                            GError **error);
