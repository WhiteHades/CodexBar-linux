#pragma once

#include "model.h"

CodexBarProvider *codexbar_opencode_go_fetch_from_home(
    const char *home_directory, gint64 now_ms, GError **error);
CodexBarProvider *codexbar_opencode_go_fetch(GError **error);
