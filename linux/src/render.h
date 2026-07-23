#pragma once

#include "model.h"

char *codexbar_render_waybar(const CodexBarSnapshot *snapshot);
char *codexbar_render_waybar_error(const char *message);
char *codexbar_render_usage_json(const CodexBarSnapshot *snapshot, gboolean pretty);
char *codexbar_render_usage_text(const CodexBarSnapshot *snapshot);
char *codexbar_render_wayfinder_usage(const CodexBarProvider *provider);
