#pragma once

#include "config.h"
#include "http.h"
#include "model.h"

typedef CodexBarHttpResponse *(*CodexBarWebProviders4Transport)(const CodexBarHttpRequest *request,
                                                               GError **error);

CodexBarProvider *codexbar_commandcode_parse(const char *credits,
                                             size_t credits_length,
                                             const char *subscription,
                                             size_t subscription_length,
                                             gint64 now_ms,
                                             GError **error);
CodexBarProvider *codexbar_commandcode_fetch_with_transport_and_cancellable(
    const CodexBarProviderConfig *config,
    CodexBarWebProviders4Transport transport,
    GCancellable *cancellable,
    gint64 now_ms,
    GError **error);
CodexBarProvider *codexbar_commandcode_fetch_with_cancellable(const CodexBarProviderConfig *config,
                                                              GCancellable *cancellable,
                                                              GError **error);

CodexBarProvider *codexbar_qoder_parse(const char *json, size_t length, gint64 now_ms, GError **error);
CodexBarProvider *codexbar_qoder_fetch_with_transport_and_cancellable(
    const CodexBarProviderConfig *config,
    CodexBarWebProviders4Transport transport,
    GCancellable *cancellable,
    gint64 now_ms,
    GError **error);
CodexBarProvider *codexbar_qoder_fetch_with_cancellable(const CodexBarProviderConfig *config,
                                                        GCancellable *cancellable,
                                                        GError **error);

CodexBarProvider *codexbar_perplexity_parse(const char *json, size_t length, gint64 now_ms, GError **error);
CodexBarProvider *codexbar_perplexity_fetch_with_transport_and_cancellable(
    const CodexBarProviderConfig *config,
    CodexBarWebProviders4Transport transport,
    GCancellable *cancellable,
    gint64 now_ms,
    GError **error);
CodexBarProvider *codexbar_perplexity_fetch_with_cancellable(const CodexBarProviderConfig *config,
                                                             GCancellable *cancellable,
                                                             GError **error);

CodexBarProvider *codexbar_longcat_parse(const char *user_json,
                                         size_t user_length,
                                         const char *usage_json,
                                         size_t usage_length,
                                         const char *fuel_json,
                                         size_t fuel_length,
                                         gint64 now_ms,
                                         GError **error);
CodexBarProvider *codexbar_longcat_fetch_with_transport_and_cancellable(
    const CodexBarProviderConfig *config,
    CodexBarWebProviders4Transport transport,
    GCancellable *cancellable,
    gint64 now_ms,
    GError **error);
CodexBarProvider *codexbar_longcat_fetch_with_cancellable(const CodexBarProviderConfig *config,
                                                          GCancellable *cancellable,
                                                          GError **error);
