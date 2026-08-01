#pragma once

#include "model.h"
#include "hooks.h"
#include "status.h"

#include <gio/gio.h>

typedef struct CodexBarRuntime CodexBarRuntime;

typedef CodexBarSnapshot *(*CodexBarRuntimeUsageFetcher)(GCancellable *cancellable,
                                                        gpointer user_data,
                                                        GError **error);
typedef void (*CodexBarRuntimeHookDispatcher)(json_object *hooks,
                                             const CodexBarHookEvent *event,
                                             gpointer user_data);
typedef void (*CodexBarRuntimeNotificationDispatcher)(const CodexBarHookEvent *event,
                                                      gpointer user_data);

CodexBarRuntime *codexbar_runtime_new(void);
CodexBarRuntime *codexbar_runtime_new_with_transports(CodexBarRuntimeUsageFetcher usage_fetcher,
                                                      gpointer usage_fetcher_data,
                                                      CodexBarStatusTransport status_transport);
void codexbar_runtime_free(CodexBarRuntime *runtime);
void codexbar_runtime_set_hook_dispatcher(CodexBarRuntime *runtime,
                                          CodexBarRuntimeHookDispatcher dispatcher,
                                          gpointer user_data);
void codexbar_runtime_set_notification_dispatcher(CodexBarRuntime *runtime,
                                                  CodexBarRuntimeNotificationDispatcher dispatcher,
                                                  gpointer user_data);
CodexBarSnapshot *codexbar_runtime_fetch(CodexBarRuntime *runtime,
                                        GCancellable *cancellable,
                                        GError **error);
