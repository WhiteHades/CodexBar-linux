#pragma once

#include "config.h"
#include "http.h"
#include "model.h"

typedef CodexBarHttpResponse *(*CodexBarWebProviders5Transport)(const CodexBarHttpRequest *request,
                                                               GError **error);

CodexBarProvider *codexbar_alibaba_token_plan_parse(const char *json,
                                                    size_t length,
                                                    gint64 now_ms,
                                                    GError **error);
CodexBarProvider *codexbar_alibaba_token_plan_fetch_with_transport_and_cancellable(
    const CodexBarProviderConfig *config,
    CodexBarWebProviders5Transport transport,
    GCancellable *cancellable,
    gint64 now_ms,
    GError **error);
CodexBarProvider *codexbar_alibaba_token_plan_fetch_with_cancellable(
    const CodexBarProviderConfig *config,
    GCancellable *cancellable,
    GError **error);

CodexBarProvider *codexbar_mimo_parse(const char *balance_json,
                                      size_t balance_length,
                                      const char *detail_json,
                                      size_t detail_length,
                                      const char *usage_json,
                                      size_t usage_length,
                                      gint64 now_ms,
                                      GError **error);
CodexBarProvider *codexbar_mimo_local_parse(const char *json,
                                            size_t length,
                                            gint64 now_ms,
                                            GError **error);
CodexBarProvider *codexbar_mimo_fetch_with_transport_and_cancellable(
    const CodexBarProviderConfig *config,
    CodexBarWebProviders5Transport transport,
    GCancellable *cancellable,
    gint64 now_ms,
    GError **error);
CodexBarProvider *codexbar_mimo_fetch_with_cancellable(const CodexBarProviderConfig *config,
                                                       GCancellable *cancellable,
                                                       GError **error);
