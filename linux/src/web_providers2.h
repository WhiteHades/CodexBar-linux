#pragma once

#include "config.h"
#include "http.h"
#include "model.h"

typedef CodexBarHttpResponse *(*CodexBarWebProviders2Transport)(const CodexBarHttpRequest *request,
                                                               GError **error);

gboolean codexbar_manus_has_auth(const CodexBarProviderConfig *config);
char *codexbar_manus_extract_session_token(const char *raw);
CodexBarProvider *codexbar_manus_parse_credits(const char *json,
                                               size_t length,
                                               gint64 now_ms,
                                               GError **error);
CodexBarProvider *codexbar_manus_fetch_with_transport_and_cancellable(
    const CodexBarProviderConfig *config,
    CodexBarWebProviders2Transport transport,
    GCancellable *cancellable,
    gint64 now_ms,
    GError **error);
CodexBarProvider *codexbar_manus_fetch_with_transport(const CodexBarProviderConfig *config,
                                                      CodexBarWebProviders2Transport transport,
                                                      gint64 now_ms,
                                                      GError **error);
CodexBarProvider *codexbar_manus_fetch_with_cancellable(const CodexBarProviderConfig *config,
                                                        GCancellable *cancellable,
                                                        GError **error);
CodexBarProvider *codexbar_manus_fetch(const CodexBarProviderConfig *config, GError **error);

gboolean codexbar_amp_has_auth(const CodexBarProviderConfig *config);
CodexBarProvider *codexbar_amp_parse_display_text(const char *text,
                                                  size_t length,
                                                  gint64 now_ms,
                                                  GError **error);
CodexBarProvider *codexbar_amp_parse_usage_api(const char *json,
                                               size_t length,
                                               gint64 now_ms,
                                               GError **error);
CodexBarProvider *codexbar_amp_parse_settings_html(const char *html,
                                                   size_t length,
                                                   gint64 now_ms,
                                                   GError **error);
CodexBarProvider *codexbar_amp_fetch_with_transport_and_cancellable(
    const CodexBarProviderConfig *config,
    CodexBarWebProviders2Transport transport,
    GCancellable *cancellable,
    gint64 now_ms,
    GError **error);
CodexBarProvider *codexbar_amp_fetch_with_transport(const CodexBarProviderConfig *config,
                                                    CodexBarWebProviders2Transport transport,
                                                    gint64 now_ms,
                                                    GError **error);
CodexBarProvider *codexbar_amp_fetch_with_cancellable(const CodexBarProviderConfig *config,
                                                      GCancellable *cancellable,
                                                      GError **error);
CodexBarProvider *codexbar_amp_fetch(const CodexBarProviderConfig *config, GError **error);

gboolean codexbar_t3chat_has_auth(const CodexBarProviderConfig *config);
CodexBarProvider *codexbar_t3chat_parse_json_lines(const char *text,
                                                   size_t length,
                                                   gint64 now_ms,
                                                   GError **error);
CodexBarProvider *codexbar_t3chat_fetch_with_transport_and_cancellable(
    const CodexBarProviderConfig *config,
    CodexBarWebProviders2Transport transport,
    GCancellable *cancellable,
    gint64 now_ms,
    GError **error);
CodexBarProvider *codexbar_t3chat_fetch_with_transport(const CodexBarProviderConfig *config,
                                                       CodexBarWebProviders2Transport transport,
                                                       gint64 now_ms,
                                                       GError **error);
CodexBarProvider *codexbar_t3chat_fetch_with_cancellable(const CodexBarProviderConfig *config,
                                                         GCancellable *cancellable,
                                                         GError **error);
CodexBarProvider *codexbar_t3chat_fetch(const CodexBarProviderConfig *config, GError **error);
