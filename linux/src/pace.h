#pragma once

#include "model.h"

CodexBarPace *codexbar_pace_calculate(const CodexBarQuotaWindow *window,
                                      gint64 now_ms,
                                      gint64 default_window_minutes);
void codexbar_pace_attach_snapshot(CodexBarSnapshot *snapshot, gint64 now_ms);
