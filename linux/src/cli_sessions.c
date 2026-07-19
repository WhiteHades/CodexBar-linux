#include "cli_sessions.h"

#include "sessions.h"

#include <json-c/json.h>
#include <stdio.h>
#include <string.h>

static char *iso_time(gint64 timestamp) {
    GDateTime *date = g_date_time_new_from_unix_utc(timestamp);
    if (!date) return NULL;
    char *value = g_date_time_format_iso8601(date);
    g_date_time_unref(date);
    return value;
}

static json_object *session_json(const CodexBarAgentSession *session) {
    json_object *object = json_object_new_object();
    json_object_object_add(object, "id", json_object_new_string(session->id));
    json_object_object_add(object, "provider", json_object_new_string(codexbar_session_provider_name(session->provider)));
    json_object_object_add(object, "source", json_object_new_string(codexbar_session_source_name(session->source)));
    json_object_object_add(object, "state", json_object_new_string(session->active ? "active" : "idle"));
    json_object_object_add(object, "pid", session->has_pid ? json_object_new_int64(session->pid) : NULL);
    json_object_object_add(object, "cwd", session->cwd ? json_object_new_string(session->cwd) : NULL);
    json_object_object_add(object,
                           "projectName",
                           session->project_name ? json_object_new_string(session->project_name) : NULL);
    char *started = session->has_started_at ? iso_time(session->started_at) : NULL;
    char *activity = session->has_last_activity_at ? iso_time(session->last_activity_at) : NULL;
    json_object_object_add(object, "startedAt", started ? json_object_new_string(started) : NULL);
    json_object_object_add(object, "lastActivityAt", activity ? json_object_new_string(activity) : NULL);
    json_object_object_add(object,
                           "transcriptPath",
                           session->transcript_path ? json_object_new_string(session->transcript_path) : NULL);
    json_object_object_add(object, "host", json_object_new_string(session->host));
    g_free(activity);
    g_free(started);
    return object;
}

static char *age_text(const CodexBarAgentSession *session, gint64 now) {
    gint64 timestamp = session->has_last_activity_at ? session->last_activity_at
        : (session->has_started_at ? session->started_at : now);
    gint64 seconds = MAX((gint64)0, now - timestamp);
    if (seconds < 60) return g_strdup_printf("%" G_GINT64_FORMAT "s", seconds);
    if (seconds < 3600) return g_strdup_printf("%" G_GINT64_FORMAT "m", seconds / 60);
    if (seconds < 86400) return g_strdup_printf("%" G_GINT64_FORMAT "h", seconds / 3600);
    return g_strdup_printf("%" G_GINT64_FORMAT "d", seconds / 86400);
}

static void print_table(const GPtrArray *sessions) {
    if (sessions->len == 0) {
        puts("No agent sessions found.");
        return;
    }
    puts("STATE   PROVIDER  SOURCE      PROJECT  ACTIVITY  ID");
    gint64 now = g_get_real_time() / G_USEC_PER_SEC;
    const char *override = g_getenv("CODEXBAR_SESSION_NOW");
    if (override && override[0] != '\0') now = g_ascii_strtoll(override, NULL, 10);
    for (guint index = 0; index < sessions->len; index++) {
        const CodexBarAgentSession *session = g_ptr_array_index(sessions, index);
        char *age = age_text(session, now);
        printf("%-7s %-9s %-11s %-8s %-9s %s\n",
               session->active ? "active" : "idle",
               codexbar_session_provider_name(session->provider),
               codexbar_session_source_name(session->source),
               session->project_name ? session->project_name : "-",
               age,
               session->id);
        g_free(age);
    }
}

static int list_sessions(int argc, char **argv) {
    gboolean json = FALSE;
    gboolean pretty = FALSE;
    for (int index = 0; index < argc; index++) {
        if (g_str_equal(argv[index], "--json")) {
            json = TRUE;
        } else if (g_str_equal(argv[index], "--pretty")) {
            pretty = TRUE;
        } else if (g_str_equal(argv[index], "--help") || g_str_equal(argv[index], "-h")) {
            puts("Usage: codexbar-linux sessions [list] [--json] [--pretty]\n"
                 "       codexbar-linux sessions focus <id>");
            return 0;
        } else {
            fprintf(stderr, "Unknown argument: %s\n", argv[index]);
            return 1;
        }
    }
    GError *error = NULL;
    GPtrArray *sessions = codexbar_sessions_scan(&error);
    if (!sessions) {
        fprintf(stderr, "Error: %s\n", error ? error->message : "Session scan failed.");
        g_clear_error(&error);
        return 1;
    }
    if (json) {
        json_object *array = json_object_new_array_ext((int)sessions->len);
        for (guint index = 0; index < sessions->len; index++) {
            json_object_array_add(array, session_json(g_ptr_array_index(sessions, index)));
        }
        puts(json_object_to_json_string_ext(array, pretty ? JSON_C_TO_STRING_PRETTY : JSON_C_TO_STRING_PLAIN));
        json_object_put(array);
    } else {
        print_table(sessions);
    }
    g_ptr_array_unref(sessions);
    return 0;
}

static int focus_session(int argc, char **argv) {
    if (argc != 1 || argv[0][0] == '\0') {
        fputs("Missing session id.\n", stderr);
        return 1;
    }
    GError *error = NULL;
    GPtrArray *sessions = codexbar_sessions_scan(&error);
    if (!sessions) {
        fprintf(stderr, "Error: %s\n", error ? error->message : "Session scan failed.");
        g_clear_error(&error);
        return 1;
    }
    gboolean found = FALSE;
    for (guint index = 0; index < sessions->len; index++) {
        const CodexBarAgentSession *session = g_ptr_array_index(sessions, index);
        if (g_str_equal(session->id, argv[0])) {
            found = TRUE;
            break;
        }
    }
    g_ptr_array_unref(sessions);
    if (!found) {
        fprintf(stderr, "Unknown session: %s\n", argv[0]);
        return 1;
    }
    fputs("Session focus is not available on Linux.\n", stderr);
    return 2;
}

int codexbar_cli_sessions_run(int argc, char **argv) {
    if (argc > 0 && g_str_equal(argv[0], "focus")) return focus_session(argc - 1, argv + 1);
    if (argc > 0 && g_str_equal(argv[0], "list")) return list_sessions(argc - 1, argv + 1);
    return list_sessions(argc, argv);
}
