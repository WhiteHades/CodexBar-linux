#pragma once

#include "model.h"

CodexBarProvider *codexbar_jetbrains_parse_xml(
    const char *xml, gsize length, const char *ide_display_name, gint64 now_ms, GError **error);
CodexBarProvider *codexbar_jetbrains_fetch_from_home(
    const char *home_directory, gint64 now_ms, GError **error);
CodexBarProvider *codexbar_jetbrains_fetch(GError **error);
