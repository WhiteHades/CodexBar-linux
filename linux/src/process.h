#pragma once

#include <gio/gio.h>
#include <stddef.h>

typedef struct {
    const char *const *arguments;
    const char *const *environment;
    const char *working_directory;
    guint timeout_milliseconds;
    guint termination_grace_milliseconds;
    size_t maximum_output_bytes;
    gboolean new_session;
} CodexBarProcessRequest;

typedef struct {
    char *standard_output;
    size_t standard_output_length;
    char *standard_error;
    size_t standard_error_length;
    int exit_status;
    int termination_signal;
} CodexBarProcessResult;

CodexBarProcessResult *codexbar_process_run(
    const CodexBarProcessRequest *request, GCancellable *cancellable, GError **error);
gboolean codexbar_process_result_succeeded(const CodexBarProcessResult *result);
void codexbar_process_result_free(CodexBarProcessResult *result);
