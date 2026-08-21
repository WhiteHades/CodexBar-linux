#pragma once

#include "config.h"
#include "http.h"
#include "model.h"

typedef CodexBarHttpResponse *(*CodexBarCodexTransport)(const CodexBarHttpRequest *request, GError **error);
typedef CodexBarProvider *(*CodexBarCodexCLIFetcher)(GError **error);

CodexBarProvider *codexbar_codex_fetch(const CodexBarProviderConfig *config,
                                       const char *source,
                                       GCancellable *cancellable,
                                       GError **error);
CodexBarProvider *codexbar_codex_fetch_with_adapters(const CodexBarProviderConfig *config,
                                                     const char *source,
                                                     CodexBarCodexTransport transport,
                                                     CodexBarCodexCLIFetcher cli_fetcher,
                                                     GCancellable *cancellable,
                                                     GError **error);
CodexBarProvider *codexbar_codex_fetch_with_home(const char *home_path, GError **error);
gboolean codexbar_codex_pat_is_available(const CodexBarProviderConfig *config);
CodexBarProvider *codexbar_codex_parse_rate_limits(const char *json, GError **error);
CodexBarProvider *codexbar_codex_parse_http_usage(const char *json,
                                                 const char *source,
                                                 gint64 updated_at_ms,
                                                 GError **error);
gboolean codexbar_codex_apply_account(CodexBarProvider *provider, const char *json, GError **error);
