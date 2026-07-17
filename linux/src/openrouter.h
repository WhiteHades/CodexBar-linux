#pragma once

#include "config.h"
#include "model.h"

CodexBarProvider *codexbar_openrouter_fetch(const CodexBarProviderConfig *config, GError **error);
CodexBarProvider *codexbar_openrouter_parse_credits(const char *json, GError **error);
