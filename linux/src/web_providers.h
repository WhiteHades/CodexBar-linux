#pragma once

#include "config.h"
#include "http.h"
#include "model.h"

typedef CodexBarHttpResponse *(*CodexBarWebProvidersTransport)(const CodexBarHttpRequest *request,
                                                              GError **error);

gboolean codexbar_cursor_has_auth(const CodexBarProviderConfig *config);
char *codexbar_cursor_load_app_cookie(const char *database_path, gint64 now_ms, GError **error);
CodexBarProvider *codexbar_cursor_parse_usage(const char *summary_json,
                                              size_t summary_length,
                                              const char *user_json,
                                              size_t user_length,
                                              const char *request_json,
                                              size_t request_length,
                                              gint64 now_ms,
                                              GError **error);
CodexBarProvider *codexbar_cursor_fetch_with_transport_and_cancellable(
    const CodexBarProviderConfig *config,
    CodexBarWebProvidersTransport transport,
    GCancellable *cancellable,
    gint64 now_ms,
    GError **error);
CodexBarProvider *codexbar_cursor_fetch_with_transport(const CodexBarProviderConfig *config,
                                                       CodexBarWebProvidersTransport transport,
                                                       gint64 now_ms,
                                                       GError **error);
CodexBarProvider *codexbar_cursor_fetch_with_cancellable(const CodexBarProviderConfig *config,
                                                         GCancellable *cancellable,
                                                         GError **error);
CodexBarProvider *codexbar_cursor_fetch(const CodexBarProviderConfig *config, GError **error);

gboolean codexbar_opencode_has_auth(const CodexBarProviderConfig *config);
CodexBarProvider *codexbar_opencode_parse_usage(const char *json,
                                                size_t length,
                                                gint64 now_ms,
                                                GError **error);
CodexBarProvider *codexbar_opencode_fetch_with_transport_and_cancellable(
    const CodexBarProviderConfig *config,
    CodexBarWebProvidersTransport transport,
    GCancellable *cancellable,
    gint64 now_ms,
    GError **error);
CodexBarProvider *codexbar_opencode_fetch_with_transport(const CodexBarProviderConfig *config,
                                                         CodexBarWebProvidersTransport transport,
                                                         gint64 now_ms,
                                                         GError **error);
CodexBarProvider *codexbar_opencode_fetch_with_cancellable(const CodexBarProviderConfig *config,
                                                           GCancellable *cancellable,
                                                           GError **error);
CodexBarProvider *codexbar_opencode_fetch(const CodexBarProviderConfig *config, GError **error);

gboolean codexbar_devin_has_auth(const CodexBarProviderConfig *config);
char *codexbar_devin_normalize_organization(const char *raw);
CodexBarProvider *codexbar_devin_parse_usage(const char *json,
                                             size_t length,
                                             const char *organization,
                                             gint64 now_ms,
                                             GError **error);
CodexBarProvider *codexbar_devin_fetch_with_transport_and_cancellable(
    const CodexBarProviderConfig *config,
    CodexBarWebProvidersTransport transport,
    GCancellable *cancellable,
    gint64 now_ms,
    GError **error);
CodexBarProvider *codexbar_devin_fetch_with_transport(const CodexBarProviderConfig *config,
                                                      CodexBarWebProvidersTransport transport,
                                                      gint64 now_ms,
                                                      GError **error);
CodexBarProvider *codexbar_devin_fetch_with_cancellable(const CodexBarProviderConfig *config,
                                                        GCancellable *cancellable,
                                                        GError **error);
CodexBarProvider *codexbar_devin_fetch(const CodexBarProviderConfig *config, GError **error);
