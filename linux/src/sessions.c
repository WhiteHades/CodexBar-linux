#define _POSIX_C_SOURCE 200809L

#include "sessions.h"

#include <errno.h>
#include <json-c/json.h>
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
    g_free(session->transcript_path);
    g_free(session->host);
    g_free(session);
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
