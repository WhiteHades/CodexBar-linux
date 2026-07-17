#pragma once

#include "config.h"
#include "model.h"

char *codexbar_proxy_provider_url(const char *base_url, const char *leaf, GError **error);
CodexBarProvider *codexbar_clawrouter_parse(const char *json, gint64 now_ms, GError **error);
CodexBarProvider *codexbar_llmproxy_parse(const char *json, gint64 now_ms, GError **error);
CodexBarProvider *codexbar_proxy_provider_fetch(const CodexBarProviderConfig *config, GError **error);
