#pragma once

#include "config.h"
#include "model.h"

CodexBarProvider *codexbar_codebuff_parse_usage(const char *json, gint64 now_ms, GError **error);
CodexBarProvider *codexbar_codebuff_fetch(const CodexBarProviderConfig *config, GError **error);
