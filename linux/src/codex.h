#pragma once

#include "model.h"

CodexBarProvider *codexbar_codex_fetch(GError **error);
CodexBarProvider *codexbar_codex_fetch_with_home(const char *home_path, GError **error);
CodexBarProvider *codexbar_codex_parse_rate_limits(const char *json, GError **error);
gboolean codexbar_codex_apply_account(CodexBarProvider *provider, const char *json, GError **error);
