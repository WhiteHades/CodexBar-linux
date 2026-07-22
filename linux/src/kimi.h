#pragma once

#include "config.h"
#include "model.h"

char *codexbar_kimi_usage_url(const char *base_url, GError **error);
CodexBarProvider *codexbar_kimi_parse_usage(const char *json, gint64 now_ms, GError **error);
CodexBarProvider *codexbar_kimi_fetch(const CodexBarProviderConfig *config, GError **error);
