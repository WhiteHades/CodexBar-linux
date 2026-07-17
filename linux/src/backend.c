#include "backend.h"

#include <gio/gio.h>

CodexBarSnapshot *codexbar_backend_fetch(GError **error) {
    const char *backend = g_getenv("CODEXBAR_BACKEND");
    if (!backend || backend[0] == '\0') {
        backend = "codexbar";
    }

    const char *argv[] = {
        backend,
        "usage",
        "--format",
        "json",
        NULL,
    };

    GSubprocess *process = g_subprocess_newv(argv,
                                             G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_PIPE,
                                             error);
    if (!process) {
        return NULL;
    }

    char *stdout_text = NULL;
    char *stderr_text = NULL;
    gboolean communicated = g_subprocess_communicate_utf8(process, NULL, NULL, &stdout_text, &stderr_text, error);
    if (!communicated) {
        g_object_unref(process);
        g_free(stdout_text);
        g_free(stderr_text);
        return NULL;
    }

    if (!g_subprocess_get_successful(process)) {
        g_set_error(error,
                    G_IO_ERROR,
                    G_IO_ERROR_FAILED,
                    "Backend exited with status %d: %s",
                    g_subprocess_get_exit_status(process),
                    stderr_text && stderr_text[0] != '\0' ? stderr_text : "no diagnostic output");
        g_object_unref(process);
        g_free(stdout_text);
        g_free(stderr_text);
        return NULL;
    }

    CodexBarSnapshot *snapshot = codexbar_snapshot_parse(stdout_text, error);
    g_object_unref(process);
    g_free(stdout_text);
    g_free(stderr_text);
    return snapshot;
}
