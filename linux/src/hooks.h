#pragma once

#include "process.h"

#include <json-c/json.h>

enum {
    CODEXBAR_MAX_HOOK_RULES = 32,
    CODEXBAR_MAX_HOOK_ID_BYTES = 128,
    CODEXBAR_MAX_HOOK_ARGUMENTS = 32,
    CODEXBAR_MAX_HOOK_STRING_BYTES = 4096,
    CODEXBAR_MAX_HOOK_COMMAND_BYTES = 32 * 1024,
    CODEXBAR_MAX_HOOK_PAYLOAD_BYTES = 4096,
    CODEXBAR_MAX_HOOK_OUTPUT_BYTES = 1024 * 1024,
};

typedef struct {
    const char *event;
    const char *provider;
    const char *account;
    const char *window;
    const char *status;
    const char *timestamp;
    gboolean has_usage_percent;
    double usage_percent;
    gboolean has_used;
    double used;
    gboolean has_limit;
    double limit;
    const char *reset_at;
} CodexBarHookEvent;

typedef CodexBarProcessResult *(*CodexBarHookProcessRunner)(const CodexBarProcessRequest *request,
                                                            gpointer user_data,
                                                            GError **error);

gboolean codexbar_hook_event_is_known(const char *event);
gboolean codexbar_hooks_enabled(json_object *hooks);
json_object *codexbar_hook_events(json_object *hooks);
json_object *codexbar_hooks_dispatch(json_object *hooks,
                                     const CodexBarHookEvent *event,
                                     CodexBarHookProcessRunner process_runner,
                                     gpointer process_runner_data);
