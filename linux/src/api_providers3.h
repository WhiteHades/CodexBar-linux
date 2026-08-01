#pragma once

#include "config.h"
#include "http.h"
#include "model.h"
#include "process.h"

typedef CodexBarHttpResponse *(*CodexBarApiProviders3Transport)(const CodexBarHttpRequest *request,
                                                               GError **error);
typedef CodexBarProcessResult *(*CodexBarDoubaoProcessRunner)(const CodexBarProcessRequest *request,
                                                             GCancellable *cancellable,
                                                             GError **error);

gboolean codexbar_minimax_has_api_key(const CodexBarProviderConfig *config);
CodexBarProvider *codexbar_minimax_parse_api_usage(const char *json,
                                                   size_t length,
                                                   gint64 now_ms,
                                                   GError **error);
CodexBarProvider *codexbar_minimax_fetch_with_transport_and_cancellable(
    const CodexBarProviderConfig *config,
    CodexBarApiProviders3Transport transport,
    GCancellable *cancellable,
    gint64 now_ms,
    GError **error);
CodexBarProvider *codexbar_minimax_fetch_for_source_with_transport_and_cancellable(
    const CodexBarProviderConfig *config,
    const char *source,
    CodexBarApiProviders3Transport transport,
    GCancellable *cancellable,
    gint64 now_ms,
    GError **error);
CodexBarProvider *codexbar_minimax_fetch_for_source_with_cancellable(
    const CodexBarProviderConfig *config,
    const char *source,
    GCancellable *cancellable,
    GError **error);
CodexBarProvider *codexbar_minimax_fetch_with_transport(const CodexBarProviderConfig *config,
                                                        CodexBarApiProviders3Transport transport,
                                                        gint64 now_ms,
                                                        GError **error);
CodexBarProvider *codexbar_minimax_fetch_with_cancellable(const CodexBarProviderConfig *config,
                                                          GCancellable *cancellable,
                                                          GError **error);
CodexBarProvider *codexbar_minimax_fetch(const CodexBarProviderConfig *config, GError **error);

gboolean codexbar_alibaba_has_api_key(const CodexBarProviderConfig *config);
CodexBarProvider *codexbar_alibaba_parse_api_usage(const char *json,
                                                   size_t length,
                                                   gint64 now_ms,
                                                   GError **error);
CodexBarProvider *codexbar_alibaba_fetch_with_transport_and_cancellable(
    const CodexBarProviderConfig *config,
    CodexBarApiProviders3Transport transport,
    GCancellable *cancellable,
    gint64 now_ms,
    GError **error);
CodexBarProvider *codexbar_alibaba_fetch_for_source_with_transport_and_cancellable(
    const CodexBarProviderConfig *config,
    const char *source,
    CodexBarApiProviders3Transport transport,
    GCancellable *cancellable,
    gint64 now_ms,
    GError **error);
CodexBarProvider *codexbar_alibaba_fetch_with_transport(const CodexBarProviderConfig *config,
                                                        CodexBarApiProviders3Transport transport,
                                                        gint64 now_ms,
                                                        GError **error);
CodexBarProvider *codexbar_alibaba_fetch_with_cancellable(const CodexBarProviderConfig *config,
                                                          GCancellable *cancellable,
                                                          GError **error);
CodexBarProvider *codexbar_alibaba_fetch_for_source_with_cancellable(
    const CodexBarProviderConfig *config,
    const char *source,
    GCancellable *cancellable,
    GError **error);
CodexBarProvider *codexbar_alibaba_fetch(const CodexBarProviderConfig *config, GError **error);

gboolean codexbar_doubao_has_credentials(const CodexBarProviderConfig *config);
CodexBarProvider *codexbar_doubao_parse_coding_plan(const char *json,
                                                    size_t length,
                                                    gint64 now_ms,
                                                    GError **error);
CodexBarProvider *codexbar_doubao_parse_agent_plan(const char *json,
                                                   size_t length,
                                                   gint64 now_ms,
                                                   GError **error);
CodexBarProvider *codexbar_doubao_parse_cli_usage(const char *json,
                                                 size_t length,
                                                 gint64 now_ms,
                                                 GError **error);
CodexBarProvider *codexbar_doubao_fetch_for_source_with_adapters(
    const CodexBarProviderConfig *config,
    const char *source,
    CodexBarApiProviders3Transport transport,
    CodexBarDoubaoProcessRunner runner,
    GCancellable *cancellable,
    gint64 now_ms,
    GError **error);
CodexBarProvider *codexbar_doubao_fetch_for_source_with_cancellable(
    const CodexBarProviderConfig *config,
    const char *source,
    GCancellable *cancellable,
    GError **error);
CodexBarProvider *codexbar_doubao_fetch_with_transport_and_cancellable(
    const CodexBarProviderConfig *config,
    CodexBarApiProviders3Transport transport,
    GCancellable *cancellable,
    gint64 now_ms,
    GError **error);
CodexBarProvider *codexbar_doubao_fetch_with_transport(const CodexBarProviderConfig *config,
                                                       CodexBarApiProviders3Transport transport,
                                                       gint64 now_ms,
                                                       GError **error);
CodexBarProvider *codexbar_doubao_fetch_with_cancellable(const CodexBarProviderConfig *config,
                                                         GCancellable *cancellable,
                                                         GError **error);
CodexBarProvider *codexbar_doubao_fetch(const CodexBarProviderConfig *config, GError **error);
