#pragma once

#include <glib.h>

gboolean codexbar_desktop_launch_tui(const char *program, GError **error);
int codexbar_status_item_run(const char *program);
