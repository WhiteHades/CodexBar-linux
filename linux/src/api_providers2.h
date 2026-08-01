#pragma once

#include "config.h"
#include "http.h"
#include "model.h"

typedef CodexBarHttpResponse *(*CodexBarApiProviders2Transport)(const CodexBarHttpRequest *request,
                                                               GError **error);

gboolean codexbar_synthetic_has_api_key(const CodexBarProviderConfig *config);
CodexBarProvider *codexbar_synthetic_parse_usage(const char *json, gint64 now_ms, GError **error);
CodexBarProvider *codexbar_synthetic_parse_usage_bytes(const char *json,
                                                       size_t length,
                                                       gint64 now_ms,
                                                       GError **error);
CodexBarProvider *codexbar_synthetic_fetch_with_transport_and_cancellable(
    const CodexBarProviderConfig *config,
    CodexBarApiProviders2Transport transport,
    GCancellable *cancellable,
    gint64 now_ms,
    GError **error);
CodexBarProvider *codexbar_synthetic_fetch_with_transport(const CodexBarProviderConfig *config,
                                                          CodexBarApiProviders2Transport transport,
                                                          gint64 now_ms,
                                                          GError **error);
CodexBarProvider *codexbar_synthetic_fetch_with_cancellable(const CodexBarProviderConfig *config,
                                                            GCancellable *cancellable,
                                                            GError **error);
CodexBarProvider *codexbar_synthetic_fetch(const CodexBarProviderConfig *config, GError **error);

gboolean codexbar_warp_has_api_key(const CodexBarProviderConfig *config);
CodexBarProvider *codexbar_warp_parse_usage(const char *json, gint64 now_ms, GError **error);
CodexBarProvider *codexbar_warp_parse_usage_bytes(const char *json,
                                                  size_t length,
                                                  gint64 now_ms,
                                                  GError **error);
CodexBarProvider *codexbar_warp_fetch_with_transport_and_cancellable(
    const CodexBarProviderConfig *config,
    CodexBarApiProviders2Transport transport,
    GCancellable *cancellable,
    gint64 now_ms,
    GError **error);
CodexBarProvider *codexbar_warp_fetch_with_transport(const CodexBarProviderConfig *config,
                                                     CodexBarApiProviders2Transport transport,
                                                     gint64 now_ms,
                                                     GError **error);
CodexBarProvider *codexbar_warp_fetch_with_cancellable(const CodexBarProviderConfig *config,
                                                       GCancellable *cancellable,
                                                       GError **error);
CodexBarProvider *codexbar_warp_fetch(const CodexBarProviderConfig *config, GError **error);

gboolean codexbar_groq_has_api_key(const CodexBarProviderConfig *config);
gboolean codexbar_groq_parse_scalar(const char *json, double *value, GError **error);
gboolean codexbar_groq_parse_scalar_bytes(const char *json, size_t length, double *value, GError **error);
CodexBarProvider *codexbar_groq_parse_usage(const char *requests_json,
                                            const char *input_tokens_json,
                                            const char *output_tokens_json,
                                            const char *cache_hits_json,
                                            gint64 now_ms,
                                            GError **error);
CodexBarProvider *codexbar_groq_fetch_with_transport_and_cancellable(
    const CodexBarProviderConfig *config,
    CodexBarApiProviders2Transport transport,
    GCancellable *cancellable,
    gint64 now_ms,
    GError **error);
CodexBarProvider *codexbar_groq_fetch_with_transport(const CodexBarProviderConfig *config,
                                                     CodexBarApiProviders2Transport transport,
                                                     gint64 now_ms,
                                                     GError **error);
CodexBarProvider *codexbar_groq_fetch_with_cancellable(const CodexBarProviderConfig *config,
                                                       GCancellable *cancellable,
                                                       GError **error);
CodexBarProvider *codexbar_groq_fetch(const CodexBarProviderConfig *config, GError **error);
