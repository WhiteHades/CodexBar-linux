#pragma once

#include "config.h"
#include "model.h"

CodexBarProvider *codexbar_simple_provider_fetch(const CodexBarProviderConfig *config, GError **error);
CodexBarProvider *codexbar_deepseek_parse(const char *json, GError **error);
CodexBarProvider *codexbar_moonshot_parse(const char *json, GError **error);
CodexBarProvider *codexbar_elevenlabs_parse(const char *json, GError **error);
CodexBarProvider *codexbar_crof_parse(const char *json, GError **error);
CodexBarProvider *codexbar_venice_parse(const char *json, GError **error);
CodexBarProvider *codexbar_zenmux_parse_subscription(const char *json, GError **error);
gboolean codexbar_zenmux_apply_payg(CodexBarProvider *provider, const char *json, GError **error);
