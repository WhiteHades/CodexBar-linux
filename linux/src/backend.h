#pragma once

#include "model.h"

CodexBarSnapshot *codexbar_backend_fetch(GError **error);
CodexBarSnapshot *codexbar_backend_fetch_all(GError **error);
CodexBarProvider *codexbar_backend_fetch_one(const char *provider, const char *source, GError **error);
