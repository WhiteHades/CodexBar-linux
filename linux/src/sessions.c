#define _POSIX_C_SOURCE 200809L

#include "sessions.h"

#include <errno.h>
#include <json-c/json.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

enum {
    SESSION_ACTIVE_SECONDS = 120,
    FILE_ONLY_SECONDS = 30 * 60,
    MAX_METADATA_LINE = 256 * 1024,
};

typedef struct {
    gint64 pid;
    CodexBarSessionProvider provider;
    CodexBarSessionSource source;
    char *cwd;
    gboolean has_started_at;
    gint64 started_at;
} AgentProcess;

typedef struct {
    char *id;
    char *cwd;
    CodexBarSessionSource source;
    char *path;
    gint64 modified_at;
    gboolean matched;
} Rollout;

typedef struct {
    char *path;
    gint64 modified_at;
} Transcript;

static void process_free(gpointer data) {
    AgentProcess *process = data;
    if (!process) return;
    g_free(process->cwd);
    g_free(process);
}

static void rollout_free(gpointer data) {
    Rollout *rollout = data;
    if (!rollout) return;
    g_free(rollout->id);
    g_free(rollout->cwd);
    g_free(rollout->path);
    g_free(rollout);
}

static void transcript_free(gpointer data) {
    Transcript *transcript = data;
    if (!transcript) return;
    g_free(transcript->path);
    g_free(transcript);
}

void codexbar_agent_session_free(CodexBarAgentSession *session) {
    if (!session) return;
    g_free(session->id);
    g_free(session->cwd);
    g_free(session->project_name);
    g_free(session->session_name);
    g_free(session->transcript_path);
    g_free(session->host);
    g_free(session);
}

void codexbar_remote_session_host_result_free(CodexBarRemoteSessionHostResult *result) {
    if (!result) return;
    g_free(result->host);
    if (result->sessions) g_ptr_array_unref(result->sessions);
    g_free(result->error);
    g_free(result);
}

const char *codexbar_session_provider_name(CodexBarSessionProvider provider) {
    return provider == CODEXBAR_SESSION_CODEX ? "codex" : "claude";
}

const char *codexbar_session_source_name(CodexBarSessionSource source) {
    switch (source) {
    case CODEXBAR_SESSION_CLI: return "cli";
    case CODEXBAR_SESSION_DESKTOP: return "desktopApp";
    case CODEXBAR_SESSION_IDE: return "ide";
    case CODEXBAR_SESSION_UNKNOWN: return "unknown";
    }
    return "unknown";
}

static char *clean_text(const char *text) {
    if (!text) return NULL;
    GString *result = g_string_sized_new(strlen(text));
    const char *cursor = text;
    while (*cursor != '\0') {
        gunichar character = g_utf8_get_char_validated(cursor, -1);
        if (character == (gunichar)-1 || character == (gunichar)-2) {
            g_string_append_c(result, '?');
            cursor++;
            continue;
        }
        if (g_unichar_iscntrl(character)) {
            g_string_append_c(result, ' ');
        } else {
            char encoded[6];
            int length = g_unichar_to_utf8(character, encoded);
            g_string_append_len(result, encoded, length);
        }
        cursor = g_utf8_next_char(cursor);
    }
    return g_string_free(result, FALSE);
}

static gboolean numeric_name(const char *name) {
    if (!name || name[0] == '\0') return FALSE;
    for (const char *cursor = name; *cursor != '\0'; cursor++) {
        if (!g_ascii_isdigit(*cursor)) return FALSE;
    }
    return TRUE;
}

static char *read_cmdline(const char *proc_root, const char *pid) {
    char *path = g_build_filename(proc_root, pid, "cmdline", NULL);
    char *contents = NULL;
    gsize length = 0;
    if (!g_file_get_contents(path, &contents, &length, NULL) || length == 0) {
        g_free(path);
        g_free(contents);
        return NULL;
    }
    for (gsize index = 0; index < length; index++) {
        if (contents[index] == '\0') contents[index] = ' ';
    }
    if (!g_utf8_validate(contents, length, NULL)) {
        g_free(path);
        g_free(contents);
        return NULL;
    }
    g_strstrip(contents);
    g_free(path);
    return contents;
}

static char *command_basename(const char *command) {
    const char *space = strchr(command, ' ');
    char *first = space ? g_strndup(command, (gsize)(space - command)) : g_strdup(command);
    char *base = g_path_get_basename(first);
    g_free(first);
    return base;
}

static gboolean contains_argument(const char *command, const char *argument) {
    char **parts = g_strsplit(command, " ", -1);
    gboolean found = FALSE;
    for (guint index = 1; parts[index]; index++) {
        if (g_str_equal(parts[index], argument)) {
            found = TRUE;
            break;
        }
    }
    g_strfreev(parts);
    return found;
}

static gboolean classify_process(const char *command,
                                 CodexBarSessionProvider *provider,
                                 CodexBarSessionSource *source) {
    char *base = command_basename(command);
    char *lower = g_ascii_strdown(base, -1);
    gboolean result = FALSE;
    if (g_str_equal(lower, "codex")) {
        if (!contains_argument(command, "app-server") && !contains_argument(command, "--help") &&
            !contains_argument(command, "--version")) {
            *provider = CODEXBAR_SESSION_CODEX;
            *source = CODEXBAR_SESSION_CLI;
            result = TRUE;
        }
    } else if (g_str_equal(lower, "claude")) {
        char *command_lower = g_ascii_strdown(command, -1);
        if (!contains_argument(command, "--help") && !contains_argument(command, "--version") &&
            !strstr(command_lower, "claude-code-acp")) {
            *provider = CODEXBAR_SESSION_CLAUDE;
            *source = strstr(command_lower, "application support/claude/claude-code")
                ? CODEXBAR_SESSION_DESKTOP
                : CODEXBAR_SESSION_CLI;
            result = TRUE;
        }
        g_free(command_lower);
    }
    g_free(lower);
    g_free(base);
    return result;
}

static char *read_cwd(const char *proc_root, const char *pid) {
    char *path = g_build_filename(proc_root, pid, "cwd", NULL);
    char *target = g_file_read_link(path, NULL);
    g_free(path);
    if (!target) return NULL;
    char *canonical = g_canonicalize_filename(target, NULL);
    g_free(target);
    return canonical;
}

static gint64 boot_time(const char *proc_root) {
    char *path = g_build_filename(proc_root, "stat", NULL);
    char *contents = NULL;
    g_file_get_contents(path, &contents, NULL, NULL);
    g_free(path);
    if (!contents) return 0;
    gint64 result = 0;
    char **lines = g_strsplit(contents, "\n", -1);
    for (guint index = 0; lines[index]; index++) {
        if (g_str_has_prefix(lines[index], "btime ")) {
            result = g_ascii_strtoll(lines[index] + strlen("btime "), NULL, 10);
            break;
        }
    }
    g_strfreev(lines);
    g_free(contents);
    return result;
}

static gboolean process_start_time(const char *proc_root,
                                   const char *pid,
                                   gint64 boot,
                                   gint64 *started_at) {
    if (boot <= 0) return FALSE;
    char *path = g_build_filename(proc_root, pid, "stat", NULL);
    char *contents = NULL;
    g_file_get_contents(path, &contents, NULL, NULL);
    g_free(path);
    if (!contents) return FALSE;
    char *end_name = strrchr(contents, ')');
    if (!end_name || end_name[1] != ' ') {
        g_free(contents);
        return FALSE;
    }
    char **fields = g_strsplit(end_name + 2, " ", -1);
    guint field_count = g_strv_length(fields);
    gboolean result = FALSE;
    if (field_count > 19) {
        gint64 ticks = g_ascii_strtoll(fields[19], NULL, 10);
        long ticks_per_second = sysconf(_SC_CLK_TCK);
        if (ticks >= 0 && ticks_per_second > 0) {
            *started_at = boot + ticks / ticks_per_second;
            result = TRUE;
        }
    }
    g_strfreev(fields);
    g_free(contents);
    return result;
}

static gint compare_process(gconstpointer left, gconstpointer right) {
    const AgentProcess *lhs = *(AgentProcess *const *)left;
    const AgentProcess *rhs = *(AgentProcess *const *)right;
    if (lhs->has_started_at != rhs->has_started_at) return lhs->has_started_at ? -1 : 1;
    if (lhs->started_at != rhs->started_at) return lhs->started_at > rhs->started_at ? -1 : 1;
    if (lhs->pid == rhs->pid) return 0;
    return lhs->pid > rhs->pid ? -1 : 1;
}

static GPtrArray *scan_processes(const char *proc_root, GError **error) {
    GError *directory_error = NULL;
    GDir *directory = g_dir_open(proc_root, 0, &directory_error);
    if (!directory) {
        g_propagate_prefixed_error(error, directory_error, "Could not scan %s: ", proc_root);
        return NULL;
    }
    GPtrArray *processes = g_ptr_array_new_with_free_func(process_free);
    gint64 boot = boot_time(proc_root);
    const char *name = NULL;
    while ((name = g_dir_read_name(directory))) {
        if (!numeric_name(name)) continue;
        char *command = read_cmdline(proc_root, name);
        if (!command) continue;
        CodexBarSessionProvider provider;
        CodexBarSessionSource source;
        if (classify_process(command, &provider, &source)) {
            AgentProcess *process = g_new0(AgentProcess, 1);
            process->pid = g_ascii_strtoll(name, NULL, 10);
            process->provider = provider;
            process->source = source;
            process->cwd = read_cwd(proc_root, name);
            process->has_started_at = process_start_time(proc_root, name, boot, &process->started_at);
            g_ptr_array_add(processes, process);
        }
        g_free(command);
    }
    g_dir_close(directory);
    g_ptr_array_sort(processes, compare_process);
    return processes;
}

static CodexBarSessionSource rollout_source(const char *originator, const char *source) {
    char *joined = g_strdup_printf("%s %s", originator ? originator : "", source ? source : "");
    char *lower = g_ascii_strdown(joined, -1);
    CodexBarSessionSource result = CODEXBAR_SESSION_UNKNOWN;
    if (strstr(lower, "desktop") || strstr(lower, "app-server")) {
        result = CODEXBAR_SESSION_DESKTOP;
    } else if (strstr(lower, "ide") || strstr(lower, "vscode") || strstr(lower, "cursor") ||
               strstr(lower, "zed")) {
        result = CODEXBAR_SESSION_IDE;
    } else if (strstr(lower, "codex_exec") || strstr(lower, "exec") || strstr(lower, "cli")) {
        result = CODEXBAR_SESSION_CLI;
    }
    g_free(lower);
    g_free(joined);
    return result;
}

static Rollout *read_rollout(const char *path, gint64 modified_at) {
    FILE *file = fopen(path, "r");
    if (!file) return NULL;
    char *line = g_malloc(MAX_METADATA_LINE + 2);
    char *read = fgets(line, MAX_METADATA_LINE + 2, file);
    fclose(file);
    if (!read) {
        g_free(line);
        return NULL;
    }
    json_object *object = json_tokener_parse(line);
    g_free(line);
    if (!object || !json_object_is_type(object, json_type_object)) {
        if (object) json_object_put(object);
        return NULL;
    }
    json_object *type_value = NULL;
    json_object *payload = NULL;
    json_object_object_get_ex(object, "type", &type_value);
    json_object_object_get_ex(object, "payload", &payload);
    if (!type_value || !payload || !g_str_equal(json_object_get_string(type_value), "session_meta")) {
        json_object_put(object);
        return NULL;
    }
    json_object *value = NULL;
    const char *id = NULL;
    if (json_object_object_get_ex(payload, "session_id", &value)) id = json_object_get_string(value);
    if (!id && json_object_object_get_ex(payload, "id", &value)) id = json_object_get_string(value);
    if (!id || id[0] == '\0') {
        json_object_put(object);
        return NULL;
    }
    const char *cwd = json_object_object_get_ex(payload, "cwd", &value) ? json_object_get_string(value) : NULL;
    const char *originator = json_object_object_get_ex(payload, "originator", &value) ? json_object_get_string(value) : NULL;
    const char *source = json_object_object_get_ex(payload, "source", &value) &&
            json_object_is_type(value, json_type_string)
        ? json_object_get_string(value)
        : NULL;
    Rollout *rollout = g_new0(Rollout, 1);
    rollout->id = clean_text(id);
    rollout->cwd = cwd && cwd[0] != '\0' ? g_canonicalize_filename(cwd, NULL) : NULL;
    rollout->source = rollout_source(originator, source);
    rollout->path = g_strdup(path);
    rollout->modified_at = modified_at;
    json_object_put(object);
    return rollout;
}

static void scan_rollout_directory(GPtrArray *rollouts, const char *directory) {
    GDir *dir = g_dir_open(directory, 0, NULL);
    if (!dir) return;
    const char *name = NULL;
    while ((name = g_dir_read_name(dir))) {
        if (!g_str_has_prefix(name, "rollout-") || !g_str_has_suffix(name, ".jsonl")) continue;
        char *path = g_build_filename(directory, name, NULL);
        struct stat status;
        if (lstat(path, &status) == 0 && S_ISREG(status.st_mode)) {
            Rollout *rollout = read_rollout(path, status.st_mtime);
            if (rollout) g_ptr_array_add(rollouts, rollout);
        }
        g_free(path);
    }
    g_dir_close(dir);
}

static gint compare_rollout(gconstpointer left, gconstpointer right) {
    const Rollout *lhs = *(Rollout *const *)left;
    const Rollout *rhs = *(Rollout *const *)right;
    if (lhs->modified_at == rhs->modified_at) return g_strcmp0(lhs->path, rhs->path);
    return lhs->modified_at > rhs->modified_at ? -1 : 1;
}

static GPtrArray *scan_rollouts(gint64 now) {
    const char *codex_home = g_getenv("CODEX_HOME");
    char *base = codex_home && codex_home[0] != '\0'
        ? g_build_filename(codex_home, "sessions", NULL)
        : g_build_filename(g_get_home_dir(), ".codex", "sessions", NULL);
    GPtrArray *rollouts = g_ptr_array_new_with_free_func(rollout_free);
    for (int offset = 0; offset >= -1; offset--) {
        GDateTime *date = g_date_time_new_from_unix_local(now + (gint64)offset * 86400);
        char *partition = g_date_time_format(date, "%Y/%m/%d");
        char *directory = g_build_filename(base, partition, NULL);
        scan_rollout_directory(rollouts, directory);
        g_free(directory);
        g_free(partition);
        g_date_time_unref(date);
    }
    g_free(base);
    g_ptr_array_sort(rollouts, compare_rollout);
    return rollouts;
}

static char *escaped_cwd(const char *cwd) {
    GString *escaped = g_string_new(NULL);
    const char *cursor = cwd;
    while (*cursor != '\0') {
        gunichar character = g_utf8_get_char_validated(cursor, -1);
        if (character == (gunichar)-1 || character == (gunichar)-2) {
            g_string_append_c(escaped, '-');
            cursor++;
            continue;
        }
        if (character < 128 && g_ascii_isalnum((char)character)) {
            g_string_append_c(escaped, (char)character);
        } else {
            g_string_append_c(escaped, '-');
        }
        cursor = g_utf8_next_char(cursor);
    }
    return g_string_free(escaped, FALSE);
}

static GPtrArray *claude_project_roots(void) {
    GPtrArray *roots = g_ptr_array_new_with_free_func(g_free);
    const char *configured = g_getenv("CLAUDE_CONFIG_DIR");
    if (configured && configured[0] != '\0') {
        char **parts = g_strsplit(configured, ",", -1);
        for (guint index = 0; parts[index]; index++) {
            char *trimmed = g_strstrip(g_strdup(parts[index]));
            if (trimmed[0] != '\0') {
                g_ptr_array_add(roots,
                                g_str_has_suffix(trimmed, "projects") ? g_strdup(trimmed)
                                                                      : g_build_filename(trimmed, "projects", NULL));
            }
            g_free(trimmed);
        }
        g_strfreev(parts);
    } else {
        g_ptr_array_add(roots, g_build_filename(g_get_home_dir(), ".claude", "projects", NULL));
    }
    return roots;
}

static gint compare_transcript(gconstpointer left, gconstpointer right) {
    const Transcript *lhs = *(Transcript *const *)left;
    const Transcript *rhs = *(Transcript *const *)right;
    if (lhs->modified_at == rhs->modified_at) return g_strcmp0(lhs->path, rhs->path);
    return lhs->modified_at > rhs->modified_at ? -1 : 1;
}

static GPtrArray *claude_transcripts(const char *cwd) {
    GPtrArray *transcripts = g_ptr_array_new_with_free_func(transcript_free);
    if (!cwd) return transcripts;
    char *escaped = escaped_cwd(cwd);
    GPtrArray *roots = claude_project_roots();
    for (guint root_index = 0; root_index < roots->len; root_index++) {
        char *directory = g_build_filename(g_ptr_array_index(roots, root_index), escaped, NULL);
        GDir *dir = g_dir_open(directory, 0, NULL);
        if (dir) {
            const char *name = NULL;
            while ((name = g_dir_read_name(dir))) {
                if (!g_str_has_suffix(name, ".jsonl")) continue;
                char *path = g_build_filename(directory, name, NULL);
                struct stat status;
                if (lstat(path, &status) == 0 && S_ISREG(status.st_mode)) {
                    Transcript *transcript = g_new0(Transcript, 1);
                    transcript->path = path;
                    transcript->modified_at = status.st_mtime;
                    g_ptr_array_add(transcripts, transcript);
                } else {
                    g_free(path);
                }
            }
            g_dir_close(dir);
        }
        g_free(directory);
    }
    g_ptr_array_unref(roots);
    g_free(escaped);
    g_ptr_array_sort(transcripts, compare_transcript);
    return transcripts;
}

static char *host_name(void) {
    char host[256];
    if (gethostname(host, sizeof(host)) != 0) return g_strdup("localhost");
    host[sizeof(host) - 1] = '\0';
    return clean_text(host);
}

static char *project_name(const char *cwd) {
    return cwd && cwd[0] != '\0' ? g_path_get_basename(cwd) : NULL;
}

static CodexBarAgentSession *make_session(const char *id,
                                         CodexBarSessionProvider provider,
                                         CodexBarSessionSource source,
                                         const AgentProcess *process,
                                         const char *cwd,
                                         const char *transcript,
                                         gboolean has_activity,
                                         gint64 activity,
                                         const char *host,
                                         gint64 now) {
    CodexBarAgentSession *session = g_new0(CodexBarAgentSession, 1);
    session->id = clean_text(id);
    session->provider = provider;
    session->source = source;
    session->active = has_activity ? now - activity <= SESSION_ACTIVE_SECONDS : process != NULL;
    session->has_pid = process != NULL;
    session->pid = process ? process->pid : 0;
    session->cwd = cwd ? g_strdup(cwd) : NULL;
    session->project_name = project_name(cwd);
    session->has_started_at = process && process->has_started_at;
    session->started_at = process ? process->started_at : 0;
    session->has_last_activity_at = has_activity;
    session->last_activity_at = activity;
    session->transcript_path = transcript ? g_strdup(transcript) : NULL;
    session->host = g_strdup(host);
    return session;
}

static Rollout *matching_rollout(GPtrArray *rollouts, const char *cwd) {
    if (!cwd) return NULL;
    for (guint index = 0; index < rollouts->len; index++) {
        Rollout *rollout = g_ptr_array_index(rollouts, index);
        if (!rollout->matched && rollout->cwd && g_str_equal(rollout->cwd, cwd)) return rollout;
    }
    return NULL;
}

static guint process_count_for_cwd(const GPtrArray *processes,
                                   CodexBarSessionProvider provider,
                                   const char *cwd) {
    guint count = 0;
    for (guint index = 0; index < processes->len; index++) {
        const AgentProcess *process = g_ptr_array_index(processes, index);
        if (process->provider == provider && g_strcmp0(process->cwd, cwd) == 0) count++;
    }
    return count;
}

static gint compare_session(gconstpointer left, gconstpointer right) {
    const CodexBarAgentSession *lhs = *(CodexBarAgentSession *const *)left;
    const CodexBarAgentSession *rhs = *(CodexBarAgentSession *const *)right;
    if (lhs->active != rhs->active) return lhs->active ? -1 : 1;
    gint64 lhs_time = lhs->has_last_activity_at ? lhs->last_activity_at : lhs->started_at;
    gint64 rhs_time = rhs->has_last_activity_at ? rhs->last_activity_at : rhs->started_at;
    if (lhs_time != rhs_time) return lhs_time > rhs_time ? -1 : 1;
    return g_strcmp0(lhs->id, rhs->id);
}

static gint64 scan_now(void) {
    const char *override = g_getenv("CODEXBAR_SESSION_NOW");
    if (override && override[0] != '\0') {
        char *end = NULL;
        gint64 value = g_ascii_strtoll(override, &end, 10);
        if (*end == '\0' && value > 0) return value;
    }
    return g_get_real_time() / G_USEC_PER_SEC;
}

GPtrArray *codexbar_sessions_scan(GError **error) {
    const char *proc_root = g_getenv("CODEXBAR_SESSION_PROC_ROOT");
    if (!proc_root || proc_root[0] == '\0') proc_root = "/proc";
    GPtrArray *processes = scan_processes(proc_root, error);
    if (!processes) return NULL;
    gint64 now = scan_now();
    GPtrArray *rollouts = scan_rollouts(now);
    GPtrArray *sessions = g_ptr_array_new_with_free_func((GDestroyNotify)codexbar_agent_session_free);
    char *host = host_name();

    for (guint index = 0; index < processes->len; index++) {
        const AgentProcess *process = g_ptr_array_index(processes, index);
        if (process->provider == CODEXBAR_SESSION_CODEX) {
            Rollout *rollout = matching_rollout(rollouts, process->cwd);
            if (rollout) rollout->matched = TRUE;
            char *fallback = rollout ? NULL : g_strdup_printf("pid:%" G_GINT64_FORMAT, process->pid);
            CodexBarSessionSource source = rollout && rollout->source != CODEXBAR_SESSION_UNKNOWN
                ? rollout->source
                : CODEXBAR_SESSION_CLI;
            g_ptr_array_add(sessions,
                            make_session(rollout ? rollout->id : fallback,
                                         CODEXBAR_SESSION_CODEX,
                                         source,
                                         process,
                                         process->cwd ? process->cwd : (rollout ? rollout->cwd : NULL),
                                         rollout ? rollout->path : NULL,
                                         rollout != NULL,
                                         rollout ? rollout->modified_at : 0,
                                         host,
                                         now));
            g_free(fallback);
        } else {
            Transcript *transcript = NULL;
            GPtrArray *transcripts = claude_transcripts(process->cwd);
            if (process_count_for_cwd(processes, CODEXBAR_SESSION_CLAUDE, process->cwd) == 1 && transcripts->len > 0) {
                Transcript *candidate = g_ptr_array_index(transcripts, 0);
                if (!process->has_started_at || candidate->modified_at >= process->started_at) transcript = candidate;
            }
            char *fallback = transcript ? NULL : g_strdup_printf("pid:%" G_GINT64_FORMAT, process->pid);
            char *identifier = transcript ? g_path_get_basename(transcript->path) : NULL;
            if (identifier && g_str_has_suffix(identifier, ".jsonl")) identifier[strlen(identifier) - 6] = '\0';
            g_ptr_array_add(sessions,
                            make_session(identifier ? identifier : fallback,
                                         CODEXBAR_SESSION_CLAUDE,
                                         process->source,
                                         process,
                                         process->cwd,
                                         transcript ? transcript->path : NULL,
                                         transcript != NULL,
                                         transcript ? transcript->modified_at : 0,
                                         host,
                                         now));
            g_free(identifier);
            g_free(fallback);
            g_ptr_array_unref(transcripts);
        }
    }

    for (guint index = 0; index < rollouts->len; index++) {
        const Rollout *rollout = g_ptr_array_index(rollouts, index);
        if (rollout->matched || now - rollout->modified_at > FILE_ONLY_SECONDS) continue;
        g_ptr_array_add(sessions,
                        make_session(rollout->id,
                                     CODEXBAR_SESSION_CODEX,
                                     rollout->source,
                                     NULL,
                                     rollout->cwd,
                                     rollout->path,
                                     TRUE,
                                     rollout->modified_at,
                                     host,
                                     now));
    }

    g_ptr_array_sort(sessions, compare_session);
    GHashTable *seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    for (guint index = 0; index < sessions->len;) {
        CodexBarAgentSession *session = g_ptr_array_index(sessions, index);
        char *key = g_strdup_printf("%s:%s", session->host, session->id);
        if (g_hash_table_contains(seen, key)) {
            g_free(key);
            g_ptr_array_remove_index(sessions, index);
        } else {
            g_hash_table_add(seen, key);
            index++;
        }
    }
    g_hash_table_unref(seen);
    g_free(host);
    g_ptr_array_unref(rollouts);
    g_ptr_array_unref(processes);
    return sessions;
}

static json_object *parse_json(const char *json, size_t length) {
    if (!json || length > INT_MAX) return NULL;
    json_tokener *tokener = json_tokener_new();
    json_object *root = json_tokener_parse_ex(tokener, json, (int)length);
    enum json_tokener_error code = json_tokener_get_error(tokener);
    size_t consumed = json_tokener_get_parse_end(tokener);
    while (consumed < length && g_ascii_isspace(json[consumed])) consumed++;
    json_tokener_free(tokener);
    if (code != json_tokener_success || consumed != length) {
        if (root) json_object_put(root);
        return NULL;
    }
    return root;
}

static char *first_dns_label(const char *value) {
    if (!value) return NULL;
    size_t start = 0;
    size_t end = strlen(value);
    while (start < end && value[start] == '.') start++;
    while (end > start && value[end - 1] == '.') end--;
    const char *dot = memchr(value + start, '.', end - start);
    if (dot) end = (size_t)(dot - value);
    return end > start ? g_strndup(value + start, end - start) : NULL;
}

static void add_local_label(GHashTable *labels, const char *value) {
    char *label = first_dns_label(value);
    if (!label) return;
    char *lower = g_utf8_strdown(label, -1);
    g_hash_table_add(labels, lower);
    g_free(label);
}

static void add_tailscale_peer(json_object *peer, GHashTable *local, GHashTable *seen, GPtrArray *hosts) {
    json_object *online = NULL;
    json_object *os = NULL;
    json_object *dns_name = NULL;
    if (!json_object_object_get_ex(peer, "Online", &online) ||
        !json_object_is_type(online, json_type_boolean) || !json_object_get_boolean(online) ||
        !json_object_object_get_ex(peer, "OS", &os) || !json_object_is_type(os, json_type_string) ||
        !json_object_object_get_ex(peer, "DNSName", &dns_name) ||
        !json_object_is_type(dns_name, json_type_string)) {
        return;
    }
    const char *os_name = json_object_get_string(os);
    if (!g_str_equal(os_name, "linux")) return;
    char *label = first_dns_label(json_object_get_string(dns_name));
    if (!label) return;
    char *lower = g_utf8_strdown(label, -1);
    if (!g_hash_table_contains(local, lower) && !g_hash_table_contains(seen, lower)) {
        g_hash_table_add(seen, lower);
        g_ptr_array_add(hosts, label);
        return;
    }
    g_free(lower);
    g_free(label);
}

static gint compare_strings(gconstpointer left, gconstpointer right) {
    return g_strcmp0(*(const char *const *)left, *(const char *const *)right);
}

GPtrArray *codexbar_tailscale_status_parse_hosts(const char *json, size_t length, const char *local_host) {
    json_object *root = parse_json(json, length);
    if (!root || !json_object_is_type(root, json_type_object)) {
        if (root) json_object_put(root);
        return NULL;
    }
    json_object *backend = NULL;
    if (json_object_object_get_ex(root, "BackendState", &backend) &&
        (!json_object_is_type(backend, json_type_string) ||
         g_ascii_strcasecmp(json_object_get_string(backend), "Running") != 0)) {
        json_object_put(root);
        return NULL;
    }
    json_object *self = NULL;
    gboolean has_self = json_object_object_get_ex(root, "Self", &self);
    if (has_self && !json_object_is_type(self, json_type_object)) {
        json_object_put(root);
        return NULL;
    }
    json_object *peers = NULL;
    gboolean has_peers = json_object_object_get_ex(root, "Peer", &peers);
    gboolean valid_peer_shape = has_peers &&
        (json_object_is_type(peers, json_type_object) || json_object_is_type(peers, json_type_array) ||
         json_object_is_type(peers, json_type_null));
    if (!has_self && !valid_peer_shape) {
        json_object_put(root);
        return NULL;
    }
    if (has_peers && !valid_peer_shape) {
        json_object_put(root);
        return NULL;
    }

    GPtrArray *hosts = g_ptr_array_new_with_free_func(g_free);
    GHashTable *local = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    GHashTable *seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    add_local_label(local, local_host);
    if (has_self) {
        json_object *value = NULL;
        if (json_object_object_get_ex(self, "DNSName", &value) && json_object_is_type(value, json_type_string)) {
            add_local_label(local, json_object_get_string(value));
        }
        if (json_object_object_get_ex(self, "HostName", &value) && json_object_is_type(value, json_type_string)) {
            add_local_label(local, json_object_get_string(value));
        }
    }
    gboolean valid_entries = TRUE;
    if (has_peers && json_object_is_type(peers, json_type_object)) {
        json_object_object_foreach(peers, key, peer) {
            (void)key;
            if (!json_object_is_type(peer, json_type_object)) {
                valid_entries = FALSE;
                break;
            }
            add_tailscale_peer(peer, local, seen, hosts);
        }
    } else if (has_peers && json_object_is_type(peers, json_type_array)) {
        size_t count = json_object_array_length(peers);
        for (size_t index = 0; index < count; index++) {
            json_object *peer = json_object_array_get_idx(peers, index);
            if (!json_object_is_type(peer, json_type_object)) {
                valid_entries = FALSE;
                break;
            }
            add_tailscale_peer(peer, local, seen, hosts);
        }
    }
    g_hash_table_unref(seen);
    g_hash_table_unref(local);
    json_object_put(root);
    if (!valid_entries) {
        g_ptr_array_unref(hosts);
        return NULL;
    }
    g_ptr_array_sort(hosts, compare_strings);
    return hosts;
}

GPtrArray *codexbar_session_hosts_sanitize(const char *const *hosts, size_t count) {
    GPtrArray *result = g_ptr_array_new_with_free_func(g_free);
    GHashTable *seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    for (size_t index = 0; index < count; index++) {
        if (!hosts || !hosts[index]) continue;
        if (!g_utf8_validate(hosts[index], -1, NULL)) continue;
        const char *start = hosts[index];
        while (*start && g_unichar_isspace(g_utf8_get_char(start))) start = g_utf8_next_char(start);
        const char *end = hosts[index] + strlen(hosts[index]);
        while (end > start) {
            const char *previous = g_utf8_find_prev_char(hosts[index], end);
            if (!previous || !g_unichar_isspace(g_utf8_get_char(previous))) break;
            end = previous;
        }
        char *host = g_strndup(start, (gsize)(end - start));
        gboolean valid = host[0] != '\0' && host[0] != '-';
        for (const char *cursor = host; valid && *cursor; cursor = g_utf8_next_char(cursor)) {
            gunichar character = g_utf8_get_char(cursor);
            if (g_unichar_iscntrl(character) || g_unichar_isspace(character)) valid = FALSE;
        }
        char *lower = valid ? g_utf8_strdown(host, -1) : NULL;
        if (valid && !g_hash_table_contains(seen, lower)) {
            g_hash_table_add(seen, lower);
            g_ptr_array_add(result, host);
        } else {
            g_free(lower);
            g_free(host);
        }
    }
    g_hash_table_unref(seen);
    return result;
}

GPtrArray *codexbar_session_hosts_from_csv(const char *csv) {
    if (!csv) return g_ptr_array_new_with_free_func(g_free);
    char **parts = g_strsplit(csv, ",", -1);
    size_t count = g_strv_length(parts);
    GPtrArray *result = codexbar_session_hosts_sanitize((const char *const *)parts, count);
    g_strfreev(parts);
    return result;
}

static gboolean read_required_string(json_object *object, const char *key, const char **value) {
    json_object *entry = NULL;
    if (!json_object_object_get_ex(object, key, &entry) || !json_object_is_type(entry, json_type_string)) {
        return FALSE;
    }
    *value = json_object_get_string(entry);
    return TRUE;
}

static char *read_optional_string(json_object *object, const char *key, gboolean *valid) {
    json_object *entry = NULL;
    if (!json_object_object_get_ex(object, key, &entry) || json_object_is_type(entry, json_type_null)) return NULL;
    if (!json_object_is_type(entry, json_type_string)) {
        *valid = FALSE;
        return NULL;
    }
    return g_strdup(json_object_get_string(entry));
}

static gboolean read_optional_time(json_object *object, const char *key, gboolean *has_value, gint64 *timestamp) {
    json_object *entry = NULL;
    if (!json_object_object_get_ex(object, key, &entry) || json_object_is_type(entry, json_type_null)) return TRUE;
    if (!json_object_is_type(entry, json_type_string)) return FALSE;
    GDateTime *date = g_date_time_new_from_iso8601(json_object_get_string(entry), NULL);
    if (!date) return FALSE;
    *has_value = TRUE;
    *timestamp = g_date_time_to_unix(date);
    g_date_time_unref(date);
    return TRUE;
}

GPtrArray *codexbar_remote_sessions_parse(const char *json, size_t length, const char *host, GError **error) {
    json_object *root = parse_json(json, length);
    if (!root || !json_object_is_type(root, json_type_array)) {
        if (root) json_object_put(root);
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Remote sessions output is not a JSON array");
        return NULL;
    }
    GPtrArray *sessions = g_ptr_array_new_with_free_func((GDestroyNotify)codexbar_agent_session_free);
    size_t count = json_object_array_length(root);
    for (size_t index = 0; index < count; index++) {
        json_object *entry = json_object_array_get_idx(root, index);
        const char *id = NULL;
        const char *provider = NULL;
        const char *source = NULL;
        const char *state = NULL;
        const char *encoded_host = NULL;
        gboolean valid = json_object_is_type(entry, json_type_object) &&
            read_required_string(entry, "id", &id) &&
            read_required_string(entry, "provider", &provider) &&
            read_required_string(entry, "source", &source) && read_required_string(entry, "state", &state) &&
            read_required_string(entry, "host", &encoded_host);
        CodexBarAgentSession *session = valid ? g_new0(CodexBarAgentSession, 1) : NULL;
        if (valid) {
            session->id = g_strdup(id);
            session->host = g_strdup(host);
            if (g_str_equal(provider, "codex")) session->provider = CODEXBAR_SESSION_CODEX;
            else if (g_str_equal(provider, "claude")) session->provider = CODEXBAR_SESSION_CLAUDE;
            else valid = FALSE;
            if (g_str_equal(source, "cli")) session->source = CODEXBAR_SESSION_CLI;
            else if (g_str_equal(source, "desktopApp")) session->source = CODEXBAR_SESSION_DESKTOP;
            else if (g_str_equal(source, "ide")) session->source = CODEXBAR_SESSION_IDE;
            else if (g_str_equal(source, "unknown")) session->source = CODEXBAR_SESSION_UNKNOWN;
            else valid = FALSE;
            if (g_str_equal(state, "active")) session->active = TRUE;
            else if (g_str_equal(state, "idle")) session->active = FALSE;
            else valid = FALSE;
            json_object *pid = NULL;
            if (json_object_object_get_ex(entry, "pid", &pid) && !json_object_is_type(pid, json_type_null)) {
                if (!json_object_is_type(pid, json_type_int)) valid = FALSE;
                else {
                    session->has_pid = TRUE;
                    session->pid = json_object_get_int64(pid);
                    if (session->pid < G_MININT32 || session->pid > G_MAXINT32) valid = FALSE;
                }
            }
            session->cwd = read_optional_string(entry, "cwd", &valid);
            session->project_name = read_optional_string(entry, "projectName", &valid);
            session->session_name = read_optional_string(entry, "sessionName", &valid);
            session->transcript_path = read_optional_string(entry, "transcriptPath", &valid);
            if (!read_optional_time(entry, "startedAt", &session->has_started_at, &session->started_at) ||
                !read_optional_time(entry, "lastActivityAt", &session->has_last_activity_at,
                                    &session->last_activity_at)) {
                valid = FALSE;
            }
        }
        if (!valid) {
            codexbar_agent_session_free(session);
            g_ptr_array_unref(sessions);
            json_object_put(root);
            g_set_error(error,
                        G_IO_ERROR,
                        G_IO_ERROR_INVALID_DATA,
                        "Invalid remote session at index %zu",
                        index);
            return NULL;
        }
        g_ptr_array_add(sessions, session);
    }
    json_object_put(root);
    return sessions;
}

static char *find_executable(const char *name, const char *const *fallbacks) {
    char *path = g_find_program_in_path(name);
    if (path) return path;
    for (size_t index = 0; fallbacks && fallbacks[index]; index++) {
        if (g_file_test(fallbacks[index], G_FILE_TEST_IS_EXECUTABLE)) return g_strdup(fallbacks[index]);
    }
    return NULL;
}

static char *shell_quote(const char *value) {
    GString *quoted = g_string_new("'");
    for (const char *cursor = value; *cursor; cursor++) {
        if (*cursor == '\'') g_string_append(quoted, "'\\''");
        else g_string_append_c(quoted, *cursor);
    }
    g_string_append_c(quoted, '\'');
    return g_string_free(quoted, FALSE);
}

static CodexBarProcessResult *run_session_process(CodexBarSessionProcessRunner runner,
                                                  const CodexBarProcessRequest *request,
                                                  GCancellable *cancellable,
                                                  GError **error) {
    return runner ? runner(request, cancellable, error) : codexbar_process_run(request, cancellable, error);
}

typedef struct {
    char *host;
    CodexBarSessionProcessRunner runner;
    GCancellable *cancellable;
    CodexBarRemoteSessionHostResult *result;
} RemoteFetchJob;

static const char *remote_list_command =
    "CODEXBAR_REMOTE_SESSIONS_LOCAL_ONLY=1 codexbar-linux sessions --json";

static CodexBarRemoteSessionHostResult *remote_fetch_one(const char *host,
                                                         CodexBarSessionProcessRunner runner,
                                                         GCancellable *cancellable) {
    CodexBarRemoteSessionHostResult *remote = g_new0(CodexBarRemoteSessionHostResult, 1);
    remote->host = g_strdup(host);
    remote->sessions = g_ptr_array_new_with_free_func((GDestroyNotify)codexbar_agent_session_free);
    const char *fallbacks[] = {"/usr/bin/ssh", "/bin/ssh", NULL};
    char *ssh = runner ? g_strdup("ssh") : find_executable("ssh", fallbacks);
    if (!ssh) {
        remote->error = g_strdup("ssh executable not found");
        return remote;
    }
    char *command = shell_quote(remote_list_command);
    const char *arguments[] = {
        ssh, "-o", "BatchMode=yes", "-o", "ConnectTimeout=3", host, "sh", "-lc", command, NULL};
    CodexBarProcessRequest request = {
        .arguments = arguments,
        .timeout_milliseconds = 5000,
        .termination_grace_milliseconds = 400,
        .maximum_output_bytes = 1024U * 1024U,
        .new_session = TRUE,
    };
    GError *error = NULL;
    CodexBarProcessResult *process = run_session_process(runner, &request, cancellable, &error);
    if (!process) {
        remote->error = g_strdup(error ? error->message : "Remote sessions command failed");
    } else if (!codexbar_process_result_succeeded(process)) {
        remote->error = g_strdup_printf("Remote sessions command exited with status %d", process->exit_status);
    } else {
        GPtrArray *sessions = codexbar_remote_sessions_parse(
            process->standard_output, process->standard_output_length, host, &error);
        if (sessions) {
            g_ptr_array_unref(remote->sessions);
            remote->sessions = sessions;
        } else {
            remote->error = g_strdup(error ? error->message : "Invalid remote sessions output");
        }
    }
    g_clear_error(&error);
    codexbar_process_result_free(process);
    g_free(command);
    g_free(ssh);
    return remote;
}

static void remote_fetch_worker(gpointer data, gpointer unused) {
    (void)unused;
    RemoteFetchJob *job = data;
    job->result = remote_fetch_one(job->host, job->runner, job->cancellable);
}

static gint compare_remote_results(gconstpointer left, gconstpointer right) {
    const CodexBarRemoteSessionHostResult *a = *(CodexBarRemoteSessionHostResult *const *)left;
    const CodexBarRemoteSessionHostResult *b = *(CodexBarRemoteSessionHostResult *const *)right;
    int folded = g_ascii_strcasecmp(a->host, b->host);
    return folded != 0 ? folded : g_strcmp0(a->host, b->host);
}

GPtrArray *codexbar_remote_sessions_fetch(const char *const *hosts,
                                          size_t count,
                                          CodexBarSessionProcessRunner runner,
                                          GCancellable *cancellable) {
    GPtrArray *clean_hosts = codexbar_session_hosts_sanitize(hosts, count);
    GPtrArray *results =
        g_ptr_array_new_with_free_func((GDestroyNotify)codexbar_remote_session_host_result_free);
    if (clean_hosts->len == 0) {
        g_ptr_array_unref(clean_hosts);
        return results;
    }
    RemoteFetchJob *jobs = g_new0(RemoteFetchJob, clean_hosts->len);
    GError *pool_error = NULL;
    GThreadPool *pool = g_thread_pool_new(remote_fetch_worker, NULL, 4, FALSE, &pool_error);
    for (guint index = 0; index < clean_hosts->len; index++) {
        jobs[index].host = g_ptr_array_index(clean_hosts, index);
        jobs[index].runner = runner;
        jobs[index].cancellable = cancellable;
        if (pool) g_thread_pool_push(pool, &jobs[index], NULL);
        else jobs[index].result = remote_fetch_one(jobs[index].host, runner, cancellable);
    }
    if (pool) g_thread_pool_free(pool, FALSE, TRUE);
    g_clear_error(&pool_error);
    for (guint index = 0; index < clean_hosts->len; index++) g_ptr_array_add(results, jobs[index].result);
    g_free(jobs);
    g_ptr_array_unref(clean_hosts);
    g_ptr_array_sort(results, compare_remote_results);
    return results;
}

GPtrArray *codexbar_remote_sessions_discover(CodexBarSessionProcessRunner runner, GCancellable *cancellable) {
    GPtrArray *candidates = g_ptr_array_new_with_free_func(g_free);
    GHashTable *seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    const char *path = g_getenv("PATH");
    char **directories = g_strsplit(path ? path : "", G_SEARCHPATH_SEPARATOR_S, -1);
    for (guint index = 0; directories[index]; index++) {
        if (directories[index][0] == '\0') continue;
        char *candidate = g_build_filename(directories[index], "tailscale", NULL);
        if (!g_hash_table_contains(seen, candidate)) {
            g_hash_table_add(seen, g_strdup(candidate));
            g_ptr_array_add(candidates, candidate);
        } else {
            g_free(candidate);
        }
    }
    g_strfreev(directories);
    const char *fixed[] = {"/usr/local/bin/tailscale", "/usr/bin/tailscale", NULL};
    for (size_t index = 0; fixed[index]; index++) {
        if (!g_hash_table_contains(seen, fixed[index])) {
            g_hash_table_add(seen, g_strdup(fixed[index]));
            g_ptr_array_add(candidates, g_strdup(fixed[index]));
        }
    }
    g_hash_table_unref(seen);
    char **environment = g_get_environ();
    if (!g_environ_getenv(environment, "TERM") && !g_environ_getenv(environment, "SHLVL")) {
        environment = g_environ_setenv(environment, "SHLVL", "1", TRUE);
    }
    char *local = host_name();
    for (guint index = 0; index < candidates->len; index++) {
        const char *candidate = g_ptr_array_index(candidates, index);
        if (!runner && !g_file_test(candidate, G_FILE_TEST_IS_EXECUTABLE)) continue;
        const char *arguments[] = {candidate, "status", "--json", NULL};
        CodexBarProcessRequest request = {
            .arguments = arguments,
            .environment = (const char *const *)environment,
            .timeout_milliseconds = 5000,
            .termination_grace_milliseconds = 400,
            .maximum_output_bytes = 1024U * 1024U,
            .new_session = TRUE,
        };
        GError *error = NULL;
        CodexBarProcessResult *process = run_session_process(runner, &request, cancellable, &error);
        g_clear_error(&error);
        if (!process) continue;
        GPtrArray *hosts = NULL;
        if (codexbar_process_result_succeeded(process)) {
            hosts = codexbar_tailscale_status_parse_hosts(
                process->standard_output, process->standard_output_length, local);
        }
        codexbar_process_result_free(process);
        if (hosts) {
            g_free(local);
            g_strfreev(environment);
            g_ptr_array_unref(candidates);
            return hosts;
        }
    }
    g_free(local);
    g_strfreev(environment);
    g_ptr_array_unref(candidates);
    return g_ptr_array_new_with_free_func(g_free);
}

gboolean codexbar_remote_session_focus(const char *session_id,
                                       const char *host,
                                       CodexBarSessionProcessRunner runner,
                                       GCancellable *cancellable,
                                       GError **error) {
    const char *raw_hosts[] = {host};
    GPtrArray *hosts = codexbar_session_hosts_sanitize(raw_hosts, 1);
    if (!session_id || session_id[0] == '\0' || hosts->len != 1) {
        g_ptr_array_unref(hosts);
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "Invalid remote session id or host");
        return FALSE;
    }
    const char *clean_host = g_ptr_array_index(hosts, 0);
    const char *fallbacks[] = {"/usr/bin/ssh", "/bin/ssh", NULL};
    char *ssh = runner ? g_strdup("ssh") : find_executable("ssh", fallbacks);
    if (!ssh) {
        g_ptr_array_unref(hosts);
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "ssh executable not found");
        return FALSE;
    }
    char *quoted_id = shell_quote(session_id);
    char *remote_command = g_strdup_printf("codexbar-linux sessions focus %s", quoted_id);
    char *command = shell_quote(remote_command);
    const char *arguments[] = {
        ssh, "-o", "BatchMode=yes", "-o", "ConnectTimeout=3", clean_host, "sh", "-lc", command, NULL};
    CodexBarProcessRequest request = {
        .arguments = arguments,
        .timeout_milliseconds = 5000,
        .termination_grace_milliseconds = 400,
        .maximum_output_bytes = 1024U * 1024U,
        .new_session = TRUE,
    };
    CodexBarProcessResult *process = run_session_process(runner, &request, cancellable, error);
    gboolean started = process != NULL;
    codexbar_process_result_free(process);
    g_free(command);
    g_free(remote_command);
    g_free(quoted_id);
    g_free(ssh);
    g_ptr_array_unref(hosts);
    return started;
}
