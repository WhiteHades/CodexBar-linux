#pragma once

#include "model.h"

char *codexbar_render_waybar(const CodexBarSnapshot *snapshot);
char *codexbar_render_waybar_error(const char *message);
