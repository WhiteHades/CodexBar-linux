#pragma once

#include "config.h"
#include "http.h"
#include "model.h"

typedef CodexBarHttpResponse *(*CodexBarWebProviders3Transport)(const CodexBarHttpRequest *request,
                                                               GError **error);

gboolean codexbar_sakana_has_auth(const CodexBarProviderConfig *config);
CodexBarProvider *codexbar_sakana_parse_billing_html(const char *html,
                                                      size_t length,
                                                      gint64 now_ms,
                                                      GError **error);
gboolean codexbar_sakana_apply_payg_html(CodexBarProvider *provider,
                                         const char *html,
                                         size_t length,
                                         GError **error);
CodexBarProvider *codexbar_sakana_fetch_with_transport_and_cancellable(
    const CodexBarProviderConfig *config,
    CodexBarWebProviders3Transport transport,
    GCancellable *cancellable,
    gint64 now_ms,
    GError **error);
CodexBarProvider *codexbar_sakana_fetch_with_transport(const CodexBarProviderConfig *config,
                                                       CodexBarWebProviders3Transport transport,
                                                       gint64 now_ms,
                                                       GError **error);
CodexBarProvider *codexbar_sakana_fetch_with_cancellable(const CodexBarProviderConfig *config,
                                                         GCancellable *cancellable,
                                                         GError **error);
CodexBarProvider *codexbar_sakana_fetch(const CodexBarProviderConfig *config, GError **error);

gboolean codexbar_abacus_has_auth(const CodexBarProviderConfig *config);
CodexBarProvider *codexbar_abacus_parse_results(const char *compute_json,
                                                size_t compute_length,
                                                const char *billing_json,
                                                size_t billing_length,
                                                gint64 now_ms,
                                                GError **error);
CodexBarProvider *codexbar_abacus_fetch_with_transport_and_cancellable(
    const CodexBarProviderConfig *config,
    CodexBarWebProviders3Transport transport,
    GCancellable *cancellable,
    gint64 now_ms,
    GError **error);
CodexBarProvider *codexbar_abacus_fetch_with_transport(const CodexBarProviderConfig *config,
                                                       CodexBarWebProviders3Transport transport,
                                                       gint64 now_ms,
                                                       GError **error);
CodexBarProvider *codexbar_abacus_fetch_with_cancellable(const CodexBarProviderConfig *config,
                                                         GCancellable *cancellable,
                                                         GError **error);
CodexBarProvider *codexbar_abacus_fetch(const CodexBarProviderConfig *config, GError **error);

gboolean codexbar_mistral_has_auth(const CodexBarProviderConfig *config);
char *codexbar_mistral_console_cookie_header(const char *cookie_header, GError **error);
CodexBarProvider *codexbar_mistral_parse_usage(const char *json,
                                               size_t length,
                                               gint64 now_ms,
                                               GError **error);
gboolean codexbar_mistral_apply_credits(CodexBarProvider *provider,
                                       const char *json,
                                       size_t length,
                                       GError **error);
gboolean codexbar_mistral_apply_vibe_usage(CodexBarProvider *provider,
                                          const char *json,
                                          size_t length,
                                          GError **error);
CodexBarProvider *codexbar_mistral_fetch_with_transport_and_cancellable(
    const CodexBarProviderConfig *config,
    CodexBarWebProviders3Transport transport,
    GCancellable *cancellable,
    gint64 now_ms,
    GError **error);
CodexBarProvider *codexbar_mistral_fetch_with_transport(const CodexBarProviderConfig *config,
                                                        CodexBarWebProviders3Transport transport,
                                                        gint64 now_ms,
                                                        GError **error);
CodexBarProvider *codexbar_mistral_fetch_with_cancellable(const CodexBarProviderConfig *config,
                                                          GCancellable *cancellable,
                                                          GError **error);
CodexBarProvider *codexbar_mistral_fetch(const CodexBarProviderConfig *config, GError **error);
