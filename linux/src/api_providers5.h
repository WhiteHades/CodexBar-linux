#pragma once

#include "config.h"
#include "http.h"
#include "model.h"

typedef CodexBarHttpResponse *(*CodexBarApiProviders5Transport)(const CodexBarHttpRequest *request,
                                                               GError **error);

gboolean codexbar_litellm_has_credentials(const CodexBarProviderConfig *config);
CodexBarProvider *codexbar_litellm_parse_usage(const char *key_info_json,
                                               const char *account_info_json,
                                               gint64 now_ms,
                                               GError **error);
CodexBarProvider *codexbar_litellm_parse_usage_bytes(const char *key_info_json,
                                                     size_t key_info_length,
                                                     const char *account_info_json,
                                                     size_t account_info_length,
                                                     gint64 now_ms,
                                                     GError **error);
CodexBarProvider *codexbar_litellm_fetch_with_transport_and_cancellable(
    const CodexBarProviderConfig *config,
    CodexBarApiProviders5Transport transport,
    GCancellable *cancellable,
    gint64 now_ms,
    GError **error);
CodexBarProvider *codexbar_litellm_fetch_with_transport(const CodexBarProviderConfig *config,
                                                        CodexBarApiProviders5Transport transport,
                                                        gint64 now_ms,
                                                        GError **error);
CodexBarProvider *codexbar_litellm_fetch_with_cancellable(const CodexBarProviderConfig *config,
                                                          GCancellable *cancellable,
                                                          GError **error);
CodexBarProvider *codexbar_litellm_fetch(const CodexBarProviderConfig *config, GError **error);

gboolean codexbar_sub2api_has_credentials(const CodexBarProviderConfig *config);
CodexBarProvider *codexbar_sub2api_parse_usage(const char *json, gint64 now_ms, GError **error);
CodexBarProvider *codexbar_sub2api_parse_usage_bytes(const char *json,
                                                     size_t length,
                                                     gint64 now_ms,
                                                     GError **error);
CodexBarProvider *codexbar_sub2api_fetch_with_transport_and_cancellable(
    const CodexBarProviderConfig *config,
    CodexBarApiProviders5Transport transport,
    GCancellable *cancellable,
    gint64 now_ms,
    GError **error);
CodexBarProvider *codexbar_sub2api_fetch_with_transport(const CodexBarProviderConfig *config,
                                                        CodexBarApiProviders5Transport transport,
                                                        gint64 now_ms,
                                                        GError **error);
CodexBarProvider *codexbar_sub2api_fetch_with_cancellable(const CodexBarProviderConfig *config,
                                                          GCancellable *cancellable,
                                                          GError **error);
CodexBarProvider *codexbar_sub2api_fetch(const CodexBarProviderConfig *config, GError **error);

gboolean codexbar_bedrock_has_static_credentials(const CodexBarProviderConfig *config);
gboolean codexbar_bedrock_has_credentials(const CodexBarProviderConfig *config);
CodexBarProvider *codexbar_bedrock_parse_usage(const char *cost_explorer_json,
                                               const char *cloudwatch_json,
                                               const CodexBarProviderConfig *config,
                                               gint64 now_ms,
                                               GError **error);
CodexBarProvider *codexbar_bedrock_parse_usage_bytes(const char *cost_explorer_json,
                                                     size_t cost_explorer_length,
                                                     const char *cloudwatch_json,
                                                     size_t cloudwatch_length,
                                                     const CodexBarProviderConfig *config,
                                                     gint64 now_ms,
                                                     GError **error);
CodexBarProvider *codexbar_bedrock_fetch_with_transport_and_cancellable(
    const CodexBarProviderConfig *config,
    CodexBarApiProviders5Transport transport,
    GCancellable *cancellable,
    gint64 now_ms,
    GError **error);
CodexBarProvider *codexbar_bedrock_fetch_with_transport(const CodexBarProviderConfig *config,
                                                        CodexBarApiProviders5Transport transport,
                                                        gint64 now_ms,
                                                        GError **error);
CodexBarProvider *codexbar_bedrock_fetch_with_cancellable(const CodexBarProviderConfig *config,
                                                          GCancellable *cancellable,
                                                          GError **error);
CodexBarProvider *codexbar_bedrock_fetch(const CodexBarProviderConfig *config, GError **error);

char *codexbar_bedrock_sign_for_testing(const char *method,
                                        const char *url,
                                        const char *body,
                                        const char *access_key_id,
                                        const char *secret_access_key,
                                        const char *session_token,
                                        const char *region,
                                        const char *service,
                                        const char *amz_date,
                                        GError **error);
