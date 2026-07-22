#pragma once

#include "model.h"

#include <gio/gio.h>

CodexBarSnapshot *codexbar_backend_fetch(GError **error);
CodexBarSnapshot *codexbar_backend_fetch_with_cancellable(GCancellable *cancellable, GError **error);
CodexBarSnapshot *codexbar_backend_fetch_all(GError **error);
CodexBarProvider *codexbar_backend_fetch_one(const char *provider, const char *source, GError **error);
