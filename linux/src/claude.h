#pragma once

#include "config.h"
#include "http.h"
#include "model.h"
#include "process.h"

typedef CodexBarHttpResponse *(*CodexBarClaudeTransport)(const CodexBarHttpRequest *request, GError **error);
typedef CodexBarProcessResult *(*CodexBarClaudeRunner)(const CodexBarProcessRequest *request,
                                                      GCancellable *cancellable,
                                                      GError **error);

CodexBarProvider *codexbar_claude_fetch(const CodexBarProviderConfig *config,
                                       const char *source,
                                       GCancellable *cancellable,
                                       GError **error);
CodexBarProvider *codexbar_claude_fetch_with_adapters(const CodexBarProviderConfig *config,
                                                     const char *source,
                                                     CodexBarClaudeTransport transport,
                                                     CodexBarClaudeRunner runner,
                                                     GCancellable *cancellable,
                                                     gint64 now_ms,
                                                     GError **error);
CodexBarProvider *codexbar_claude_parse_oauth_usage(const char *json,
                                                    const char *rate_limit_tier,
                                                    const char *subscription_type,
                                                    gint64 updated_at_ms,
                                                    GError **error);
CodexBarProvider *codexbar_claude_parse_web_usage(const char *json,
                                                 const char *organization,
                                                 const char *organization_id,
                                                 gint64 updated_at_ms,
                                                 GError **error);
CodexBarProvider *codexbar_claude_parse_cli_usage(const char *text, gint64 updated_at_ms, GError **error);
