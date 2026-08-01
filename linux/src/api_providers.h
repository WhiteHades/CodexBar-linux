#pragma once

#include "config.h"
#include "http.h"
#include "model.h"

typedef CodexBarHttpResponse *(*CodexBarAPIProviderTransport)(const CodexBarHttpRequest *request,
                                                              GError **error);

gboolean codexbar_deepgram_has_api_key(const CodexBarProviderConfig *config);
CodexBarProvider *codexbar_deepgram_parse_usage_bytes(const char *json,
                                                       size_t length,
                                                       const char *project_id,
                                                       const char *project_name,
                                                       guint project_count,
                                                       gint64 now_ms,
                                                       GError **error);
CodexBarProvider *codexbar_deepgram_fetch_with_transport_and_cancellable(
    const CodexBarProviderConfig *config,
    CodexBarAPIProviderTransport transport,
    GCancellable *cancellable,
    gint64 now_ms,
    GError **error);
CodexBarProvider *codexbar_deepgram_fetch_with_transport(const CodexBarProviderConfig *config,
                                                          CodexBarAPIProviderTransport transport,
                                                          gint64 now_ms,
                                                          GError **error);
CodexBarProvider *codexbar_deepgram_fetch_with_cancellable(const CodexBarProviderConfig *config,
                                                            GCancellable *cancellable,
                                                            GError **error);
CodexBarProvider *codexbar_deepgram_fetch(const CodexBarProviderConfig *config, GError **error);

gboolean codexbar_poe_has_api_key(const CodexBarProviderConfig *config);
CodexBarProvider *codexbar_poe_parse_usage_bytes(const char *balance_json,
                                                  size_t balance_length,
                                                  const char *history_json,
                                                  size_t history_length,
                                                  gint64 now_ms,
                                                  GError **error);
CodexBarProvider *codexbar_poe_fetch_with_transport_and_cancellable(
    const CodexBarProviderConfig *config,
    CodexBarAPIProviderTransport transport,
    GCancellable *cancellable,
    gint64 now_ms,
    GError **error);
CodexBarProvider *codexbar_poe_fetch_with_transport(const CodexBarProviderConfig *config,
                                                     CodexBarAPIProviderTransport transport,
                                                     gint64 now_ms,
                                                     GError **error);
CodexBarProvider *codexbar_poe_fetch_with_cancellable(const CodexBarProviderConfig *config,
                                                       GCancellable *cancellable,
                                                       GError **error);
CodexBarProvider *codexbar_poe_fetch(const CodexBarProviderConfig *config, GError **error);

gboolean codexbar_chutes_has_api_key(const CodexBarProviderConfig *config);
CodexBarProvider *codexbar_chutes_parse_usage_bytes(const char *json,
                                                     size_t length,
                                                     gint64 now_ms,
                                                     GError **error);
CodexBarProvider *codexbar_chutes_fetch_with_transport_and_cancellable(
    const CodexBarProviderConfig *config,
    CodexBarAPIProviderTransport transport,
    GCancellable *cancellable,
    gint64 now_ms,
    GError **error);
CodexBarProvider *codexbar_chutes_fetch_with_transport(const CodexBarProviderConfig *config,
                                                        CodexBarAPIProviderTransport transport,
                                                        gint64 now_ms,
                                                        GError **error);
CodexBarProvider *codexbar_chutes_fetch_with_cancellable(const CodexBarProviderConfig *config,
                                                          GCancellable *cancellable,
                                                          GError **error);
CodexBarProvider *codexbar_chutes_fetch(const CodexBarProviderConfig *config, GError **error);
