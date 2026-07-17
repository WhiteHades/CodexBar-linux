#pragma once

#include "config.h"
#include "model.h"

CodexBarProvider *codexbar_simple_provider_fetch(const CodexBarProviderConfig *config, GError **error);
CodexBarProvider *codexbar_deepseek_parse(const char *json, GError **error);
CodexBarProvider *codexbar_moonshot_parse(const char *json, GError **error);
CodexBarProvider *codexbar_elevenlabs_parse(const char *json, GError **error);
