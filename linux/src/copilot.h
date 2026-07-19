#pragma once

#include "config.h"
#include "model.h"

char *codexbar_copilot_usage_url(const char *enterprise_host, GError **error);
CodexBarProvider *codexbar_copilot_parse_usage(const char *json, GError **error);
CodexBarProvider *codexbar_copilot_fetch(const CodexBarProviderConfig *config, GError **error);
