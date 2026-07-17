#pragma once

#include "model.h"

CodexBarProvider *codexbar_codex_fetch(GError **error);
CodexBarProvider *codexbar_codex_parse_rate_limits(const char *json, GError **error);
