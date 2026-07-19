#pragma once

#include "config.h"
#include "model.h"

char *codexbar_zai_quota_url(const CodexBarProviderConfig *config, GError **error);
CodexBarProvider *codexbar_zai_parse_usage(const char *json, GError **error);
CodexBarProvider *codexbar_zai_fetch(const CodexBarProviderConfig *config, GError **error);
