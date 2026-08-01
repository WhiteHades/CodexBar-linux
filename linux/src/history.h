#pragma once

#include "model.h"

#include <gio/gio.h>
#include <json-c/json.h>

typedef struct CodexBarHistoryStore CodexBarHistoryStore;

CodexBarHistoryStore *codexbar_history_store_new(const char *directory);
void codexbar_history_store_free(CodexBarHistoryStore *store);
gboolean codexbar_history_store_record(CodexBarHistoryStore *store,
                                       const CodexBarSnapshot *snapshot,
                                       gint64 captured_at_ms,
                                       gboolean historical_tracking_enabled,
                                       GError **error);
json_object *codexbar_history_store_load_provider(CodexBarHistoryStore *store,
                                                  const char *provider,
                                                  GError **error);
json_object *codexbar_history_store_load_all(CodexBarHistoryStore *store);
