#pragma once

#include "config.h"
#include "http.h"
#include "model.h"

typedef CodexBarHttpResponse *(*CodexBarApiProviders4Transport)(const CodexBarHttpRequest *request,
                                                               GError **error);

gboolean codexbar_factory_has_api_key(const CodexBarProviderConfig *config);
CodexBarProvider *codexbar_factory_parse_legacy_usage(const char *auth_json,
                                                      size_t auth_length,
                                                      const char *usage_json,
                                                      size_t usage_length,
                                                      gint64 now_ms,
                                                      GError **error);
CodexBarProvider *codexbar_factory_parse_billing_limits(const char *auth_json,
                                                        size_t auth_length,
                                                        const char *limits_json,
                                                        size_t limits_length,
                                                        gint64 now_ms,
                                                        GError **error);
CodexBarProvider *codexbar_factory_fetch_with_transport_and_cancellable(
    const CodexBarProviderConfig *config,
    CodexBarApiProviders4Transport transport,
    GCancellable *cancellable,
    gint64 now_ms,
    GError **error);
CodexBarProvider *codexbar_factory_fetch_for_source_with_cancellable(
    const CodexBarProviderConfig *config,
    const char *source,
    GCancellable *cancellable,
    GError **error);
CodexBarProvider *codexbar_factory_fetch_with_transport(const CodexBarProviderConfig *config,
                                                        CodexBarApiProviders4Transport transport,
                                                        gint64 now_ms,
                                                        GError **error);
CodexBarProvider *codexbar_factory_fetch_with_cancellable(const CodexBarProviderConfig *config,
                                                          GCancellable *cancellable,
                                                          GError **error);
CodexBarProvider *codexbar_factory_fetch(const CodexBarProviderConfig *config, GError **error);

gboolean codexbar_gemini_has_oauth_credentials(const CodexBarProviderConfig *config);
CodexBarProvider *codexbar_gemini_parse_quota(const char *quota_json,
                                             size_t quota_length,
                                             const char *id_token,
                                             const char *code_assist_json,
                                             size_t code_assist_length,
                                             gint64 now_ms,
                                             GError **error);
CodexBarProvider *codexbar_gemini_fetch_access_token_with_transport_and_cancellable(
    const char *access_token,
    const char *id_token,
    CodexBarApiProviders4Transport transport,
    GCancellable *cancellable,
    gint64 now_ms,
    GError **error);
CodexBarProvider *codexbar_gemini_fetch_with_transport_and_cancellable(
    const CodexBarProviderConfig *config,
    CodexBarApiProviders4Transport transport,
    GCancellable *cancellable,
    gint64 now_ms,
    GError **error);
CodexBarProvider *codexbar_gemini_fetch_with_transport(const CodexBarProviderConfig *config,
                                                       CodexBarApiProviders4Transport transport,
                                                       gint64 now_ms,
                                                       GError **error);
CodexBarProvider *codexbar_gemini_fetch_with_cancellable(const CodexBarProviderConfig *config,
                                                         GCancellable *cancellable,
                                                         GError **error);
CodexBarProvider *codexbar_gemini_fetch(const CodexBarProviderConfig *config, GError **error);

gboolean codexbar_antigravity_has_oauth_credentials(const CodexBarProviderConfig *config);
CodexBarProvider *codexbar_antigravity_parse_remote_usage(const char *models_json,
                                                           size_t models_length,
                                                           const char *quota_json,
                                                           size_t quota_length,
                                                           const char *id_token,
                                                           const char *email,
                                                           const char *code_assist_json,
                                                           size_t code_assist_length,
                                                           gint64 now_ms,
                                                           GError **error);
CodexBarProvider *codexbar_antigravity_oauth_fetch_with_transport_and_cancellable(
    const CodexBarProviderConfig *config,
    CodexBarApiProviders4Transport transport,
    GCancellable *cancellable,
    gint64 now_ms,
    GError **error);
CodexBarProvider *codexbar_antigravity_fetch_for_source_with_cancellable(
    const CodexBarProviderConfig *config,
    const char *source,
    GCancellable *cancellable,
    GError **error);

gboolean codexbar_ollama_has_api_key(const CodexBarProviderConfig *config);
CodexBarProvider *codexbar_ollama_parse_api_tags(const char *json,
                                                size_t length,
                                                gint64 now_ms,
                                                GError **error);
CodexBarProvider *codexbar_ollama_parse_settings_html(const char *html,
                                                      size_t length,
                                                      gint64 now_ms,
                                                      GError **error);
CodexBarProvider *codexbar_ollama_fetch_endpoints_with_transport_and_cancellable(
    const CodexBarProviderConfig *config,
    const char *tags_url,
    const char *validation_url,
    CodexBarApiProviders4Transport transport,
    GCancellable *cancellable,
    gint64 now_ms,
    GError **error);
CodexBarProvider *codexbar_ollama_fetch_with_transport_and_cancellable(
    const CodexBarProviderConfig *config,
    CodexBarApiProviders4Transport transport,
    GCancellable *cancellable,
    gint64 now_ms,
    GError **error);
CodexBarProvider *codexbar_ollama_fetch_for_source_with_transport_and_cancellable(
    const CodexBarProviderConfig *config,
    const char *source,
    CodexBarApiProviders4Transport transport,
    GCancellable *cancellable,
    gint64 now_ms,
    GError **error);
CodexBarProvider *codexbar_ollama_fetch_with_transport(const CodexBarProviderConfig *config,
                                                       CodexBarApiProviders4Transport transport,
                                                       gint64 now_ms,
                                                       GError **error);
CodexBarProvider *codexbar_ollama_fetch_with_cancellable(const CodexBarProviderConfig *config,
                                                         GCancellable *cancellable,
                                                         GError **error);
CodexBarProvider *codexbar_ollama_fetch_for_source_with_cancellable(
    const CodexBarProviderConfig *config,
    const char *source,
    GCancellable *cancellable,
    GError **error);
CodexBarProvider *codexbar_ollama_fetch(const CodexBarProviderConfig *config, GError **error);
