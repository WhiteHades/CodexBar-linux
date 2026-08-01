#pragma once

#include "config.h"
#include "http.h"
#include "model.h"

typedef CodexBarHttpResponse *(*CodexBarSimpleProviderTransport)(const CodexBarHttpRequest *request,
                                                                GError **error);

CodexBarProvider *codexbar_simple_provider_fetch(const CodexBarProviderConfig *config, GError **error);
CodexBarProvider *codexbar_deepseek_parse(const char *json, GError **error);
CodexBarProvider *codexbar_deepseek_parse_platform_balance(const char *json,
                                                           size_t length,
                                                           GError **error);
CodexBarProvider *codexbar_deepseek_fetch_for_source_with_transport_and_cancellable(
    const CodexBarProviderConfig *config,
    const char *source,
    CodexBarSimpleProviderTransport transport,
    GCancellable *cancellable,
    GError **error);
CodexBarProvider *codexbar_deepseek_fetch_for_source_with_cancellable(
    const CodexBarProviderConfig *config,
    const char *source,
    GCancellable *cancellable,
    GError **error);
CodexBarProvider *codexbar_moonshot_parse(const char *json, GError **error);
CodexBarProvider *codexbar_elevenlabs_parse(const char *json, GError **error);
CodexBarProvider *codexbar_crof_parse(const char *json, GError **error);
CodexBarProvider *codexbar_venice_parse(const char *json, GError **error);
CodexBarProvider *codexbar_zenmux_parse_subscription(const char *json, GError **error);
gboolean codexbar_zenmux_apply_payg(CodexBarProvider *provider, const char *json, GError **error);
