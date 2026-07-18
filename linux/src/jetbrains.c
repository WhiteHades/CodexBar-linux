#define _GNU_SOURCE

#include "jetbrains.h"

#include <errno.h>
#include <fcntl.h>
#include <gio/gio.h>
#include <glib/gstdio.h>
#include <json-c/json.h>
#include <math.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define MAX_QUOTA_FILE_BYTES (4U * 1024U * 1024U)

typedef struct {
    guint depth;
    guint target_depth;
    char *quota_info;
    char *next_refill;
} MarkupState;

typedef struct {
    const char *prefix;
    const char *display_name;
} IDEPattern;

static const IDEPattern ide_patterns[] = {
    {"IntelliJIdea", "IntelliJ IDEA"},
    {"PyCharm", "PyCharm"},
    {"WebStorm", "WebStorm"},
    {"GoLand", "GoLand"},
    {"CLion", "CLion"},
    {"DataGrip", "DataGrip"},
    {"RubyMine", "RubyMine"},
    {"Rider", "Rider"},
    {"PhpStorm", "PhpStorm"},
    {"AppCode", "AppCode"},
    {"Fleet", "Fleet"},
    {"AndroidStudio", "Android Studio"},
    {"RustRover", "RustRover"},
    {"Aqua", "Aqua"},
    {"DataSpell", "DataSpell"},
};

static const char *attribute_value(const char **names, const char **values, const char *wanted) {
    if (!names || !values) return NULL;
    for (guint index = 0; names[index] && values[index]; index++) {
        if (g_str_equal(names[index], wanted)) return values[index];
    }
    return NULL;
}

static void start_element(GMarkupParseContext *context,
                          const char *element_name,
                          const char **attribute_names,
                          const char **attribute_values,
                          gpointer user_data,
                          GError **error) {
    (void)context;
    (void)error;
    MarkupState *state = user_data;
    state->depth++;
    if (state->target_depth == 0 && g_str_equal(element_name, "component")) {
        const char *name = attribute_value(attribute_names, attribute_values, "name");
        if (name && g_str_equal(name, "AIAssistantQuotaManager2")) state->target_depth = state->depth;
        return;
    }
    if (state->target_depth == 0 || state->depth != state->target_depth + 1 ||
        !g_str_equal(element_name, "option")) {
        return;
    }
    const char *name = attribute_value(attribute_names, attribute_values, "name");
    const char *value = attribute_value(attribute_names, attribute_values, "value");
    if (!name || !value) return;
    if (g_str_equal(name, "quotaInfo") && !state->quota_info) state->quota_info = g_strdup(value);
    if (g_str_equal(name, "nextRefill") && !state->next_refill) state->next_refill = g_strdup(value);
}

static void end_element(
    GMarkupParseContext *context, const char *element_name, gpointer user_data, GError **error) {
    (void)context;
    (void)element_name;
    (void)error;
    MarkupState *state = user_data;
    if (state->target_depth == state->depth) state->target_depth = 0;
    if (state->depth > 0) state->depth--;
}

static json_object *parse_json_object(const char *text) {
    if (!text) return NULL;
    json_tokener *tokener = json_tokener_new();
    if (!tokener) return NULL;
    json_tokener_set_flags(tokener, JSON_TOKENER_STRICT);
    json_object *object = json_tokener_parse_ex(tokener, text, (int)MIN(strlen(text), (size_t)G_MAXINT));
    enum json_tokener_error parse_error = json_tokener_get_error(tokener);
    size_t parsed = json_tokener_get_parse_end(tokener);
    while (text[parsed] != '\0' && g_ascii_isspace(text[parsed])) parsed++;
    gboolean valid = parse_error == json_tokener_success && text[parsed] == '\0' && object &&
                     json_object_is_type(object, json_type_object);
    json_tokener_free(tokener);
    if (valid) return object;
    if (object) json_object_put(object);
    return NULL;
}

static const char *json_string(json_object *object, const char *key) {
    json_object *value = NULL;
    return json_object_object_get_ex(object, key, &value) && json_object_is_type(value, json_type_string)
               ? json_object_get_string(value)
               : NULL;
}

static double json_string_number(json_object *object, const char *key) {
    const char *raw = json_string(object, key);
    if (!raw) return 0.0;
    char *end = NULL;
    double value = g_ascii_strtod(raw, &end);
    return end && end != raw && *end == '\0' && isfinite(value) ? value : 0.0;
}

static gboolean iso_timestamp_ms(const char *raw, gint64 *result) {
    if (!raw) return FALSE;
    GDateTime *time = g_date_time_new_from_iso8601(raw, NULL);
    if (!time) return FALSE;
    *result = g_date_time_to_unix(time) * 1000 + g_date_time_get_microsecond(time) / 1000;
    g_date_time_unref(time);
    return TRUE;
}

static char *reset_description(gint64 reset_ms, gint64 now_ms) {
    gint64 interval = reset_ms - now_ms;
    if (interval <= 0) return g_strdup("Expired");
    gint64 hours = interval / (60 * 60 * 1000);
    gint64 minutes = interval % (60 * 60 * 1000) / (60 * 1000);
    if (hours > 24) {
        return g_strdup_printf(
            "Resets in %" G_GINT64_FORMAT "d %" G_GINT64_FORMAT "h", hours / 24, hours % 24);
    }
    if (hours > 0) {
        return g_strdup_printf("Resets in %" G_GINT64_FORMAT "h %" G_GINT64_FORMAT "m", hours, minutes);
    }
    return g_strdup_printf("Resets in %" G_GINT64_FORMAT "m", minutes);
}

CodexBarProvider *codexbar_jetbrains_parse_xml(
    const char *xml, gsize length, const char *ide_display_name, gint64 now_ms, GError **error) {
    g_return_val_if_fail(error == NULL || *error == NULL, NULL);
    if (!xml || length == 0 || length > MAX_QUOTA_FILE_BYTES) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Could not parse JetBrains AI quota: invalid XML");
        return NULL;
    }
    MarkupState state = {0};
    GMarkupParser parser = {.start_element = start_element, .end_element = end_element};
    GMarkupParseContext *context = g_markup_parse_context_new(&parser, G_MARKUP_DEFAULT_FLAGS, &state, NULL);
    GError *parse_error = NULL;
    gboolean parsed = g_markup_parse_context_parse(context, xml, (gssize)length, &parse_error) &&
                      g_markup_parse_context_end_parse(context, &parse_error);
    g_markup_parse_context_free(context);
    if (!parsed) {
        g_set_error(error,
                    G_IO_ERROR,
                    G_IO_ERROR_INVALID_DATA,
                    "Could not parse JetBrains AI quota: %s",
                    parse_error ? parse_error->message : "invalid XML");
        g_clear_error(&parse_error);
        g_free(state.quota_info);
        g_free(state.next_refill);
        return NULL;
    }
    if (!state.quota_info || state.quota_info[0] == '\0') {
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_NOT_FOUND,
                            "No quota information found in the JetBrains AI configuration.");
        g_free(state.quota_info);
        g_free(state.next_refill);
        return NULL;
    }

    json_object *quota = parse_json_object(state.quota_info);
    if (!quota) {
        g_set_error_literal(
            error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Could not parse JetBrains AI quota: Invalid JSON format");
        g_free(state.quota_info);
        g_free(state.next_refill);
        return NULL;
    }
    double current = json_string_number(quota, "current");
    double maximum = json_string_number(quota, "maximum");
    const char *type = json_string(quota, "type");

    gboolean has_reset = FALSE;
    gint64 reset_ms = 0;
    if (state.next_refill && state.next_refill[0] != '\0') {
        json_object *refill = parse_json_object(state.next_refill);
        if (refill) {
            has_reset = iso_timestamp_ms(json_string(refill, "next"), &reset_ms);
            json_object_put(refill);
        }
    }

    CodexBarProvider *provider = codexbar_provider_new();
    provider->provider = g_strdup("jetbrains");
    provider->source = g_strdup("local");
    provider->has_updated_at = TRUE;
    provider->updated_at_ms = now_ms;
    provider->explicit_quota_slots = TRUE;
    provider->identity = g_new0(CodexBarProviderIdentity, 1);
    provider->identity->organization = ide_display_name ? g_strdup(ide_display_name) : NULL;
    provider->identity->login_method = type ? g_strdup(type) : NULL;
    CodexBarQuotaWindow *window = codexbar_quota_window_new("primary", "Current");
    window->usage_known = TRUE;
    window->used_percent = maximum > 0.0 ? CLAMP(current / maximum * 100.0, 0.0, 100.0) : 0.0;
    if (has_reset) {
        window->has_resets_at = TRUE;
        window->resets_at_ms = reset_ms;
        window->reset_description = reset_description(reset_ms, now_ms);
    }
    codexbar_provider_add_quota_window(provider, window);
    json_object_put(quota);
    g_free(state.quota_info);
    g_free(state.next_refill);
    return provider;
}

static gboolean parse_ide_directory(const char *name, const char **display_name, const char **version) {
    for (guint index = 0; index < G_N_ELEMENTS(ide_patterns); index++) {
        size_t prefix_length = strlen(ide_patterns[index].prefix);
        if (g_ascii_strncasecmp(name, ide_patterns[index].prefix, prefix_length) != 0) continue;
        *display_name = ide_patterns[index].display_name;
        *version = name[prefix_length] != '\0' ? name + prefix_length : "Unknown";
        return TRUE;
    }
    return FALSE;
}

static int compare_versions(const char *left, const char *right) {
    char **left_parts = g_strsplit(left, ".", -1);
    char **right_parts = g_strsplit(right, ".", -1);
    GArray *left_numbers = g_array_new(FALSE, FALSE, sizeof(gint64));
    GArray *right_numbers = g_array_new(FALSE, FALSE, sizeof(gint64));
    for (guint index = 0; left_parts[index]; index++) {
        char *end = NULL;
        errno = 0;
        gint64 value = g_ascii_strtoll(left_parts[index], &end, 10);
        if (errno != ERANGE && end != left_parts[index] && *end == '\0') {
            g_array_append_val(left_numbers, value);
        }
    }
    for (guint index = 0; right_parts[index]; index++) {
        char *end = NULL;
        errno = 0;
        gint64 value = g_ascii_strtoll(right_parts[index], &end, 10);
        if (errno != ERANGE && end != right_parts[index] && *end == '\0') {
            g_array_append_val(right_numbers, value);
        }
    }
    guint count = MAX(left_numbers->len, right_numbers->len);
    int result = 0;
    for (guint index = 0; index < count; index++) {
        gint64 left_value = index < left_numbers->len ? g_array_index(left_numbers, gint64, index) : 0;
        gint64 right_value = index < right_numbers->len ? g_array_index(right_numbers, gint64, index) : 0;
        if (left_value == right_value) continue;
        result = left_value > right_value ? 1 : -1;
        break;
    }
    g_array_free(left_numbers, TRUE);
    g_array_free(right_numbers, TRUE);
    g_strfreev(left_parts);
    g_strfreev(right_parts);
    return result;
}

static char *read_quota_file(const char *path, gsize *length, GError **error) {
    int descriptor = open(path, O_RDONLY | O_CLOEXEC | O_NONBLOCK);
    if (descriptor < 0) {
        int code = errno;
        g_set_error(error,
                    G_IO_ERROR,
                    g_io_error_from_errno(code),
                    "Could not parse JetBrains AI quota: failed to open file: %s",
                    g_strerror(code));
        return NULL;
    }
    struct stat information;
    if (fstat(descriptor, &information) < 0) {
        int code = errno;
        close(descriptor);
        g_set_error(error,
                    G_IO_ERROR,
                    g_io_error_from_errno(code),
                    "Could not parse JetBrains AI quota: failed to inspect file: %s",
                    g_strerror(code));
        return NULL;
    }
    if (!S_ISREG(information.st_mode) || information.st_size < 0 || information.st_size > MAX_QUOTA_FILE_BYTES) {
        close(descriptor);
        g_set_error(error,
                    G_IO_ERROR,
                    G_IO_ERROR_INVALID_DATA,
                    "Could not parse JetBrains AI quota: invalid file at %s",
                    path);
        return NULL;
    }
    GByteArray *contents = g_byte_array_sized_new((guint)information.st_size + 1);
    guint8 buffer[8192];
    while (TRUE) {
        ssize_t count = read(descriptor, buffer, sizeof(buffer));
        if (count > 0) {
            if ((gsize)count > MAX_QUOTA_FILE_BYTES - (gsize)contents->len) {
                close(descriptor);
                g_byte_array_unref(contents);
                g_set_error(error,
                            G_IO_ERROR,
                            G_IO_ERROR_INVALID_DATA,
                            "Could not parse JetBrains AI quota: file exceeds %u bytes",
                            MAX_QUOTA_FILE_BYTES);
                return NULL;
            }
            g_byte_array_append(contents, buffer, (guint)count);
            continue;
        }
        if (count == 0) break;
        if (errno == EINTR) continue;
        int code = errno;
        close(descriptor);
        g_byte_array_unref(contents);
        g_set_error(error,
                    G_IO_ERROR,
                    g_io_error_from_errno(code),
                    "Could not parse JetBrains AI quota: failed to read file: %s",
                    g_strerror(code));
        return NULL;
    }
    close(descriptor);
    *length = contents->len;
    g_byte_array_append(contents, (const guint8 *)"", 1);
    return (char *)g_byte_array_free(contents, FALSE);
}

CodexBarProvider *codexbar_jetbrains_fetch_from_home(
    const char *home_directory, gint64 now_ms, GError **error) {
    g_return_val_if_fail(error == NULL || *error == NULL, NULL);
    if (!home_directory || home_directory[0] == '\0') {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "JetBrains home directory is unavailable");
        return NULL;
    }
    const char *roots[] = {".config/JetBrains", ".local/share/JetBrains", ".config/Google"};
    char *selected_path = NULL;
    char *selected_ide = NULL;
    char *selected_name = NULL;
    char *selected_version = NULL;
    gint64 selected_mtime = G_MININT64;
    for (guint root_index = 0; root_index < G_N_ELEMENTS(roots); root_index++) {
        char *root = g_build_filename(home_directory, roots[root_index], NULL);
        GDir *directory = g_dir_open(root, 0, NULL);
        if (!directory) {
            g_free(root);
            continue;
        }
        const char *name;
        while ((name = g_dir_read_name(directory))) {
            const char *display_name = NULL;
            const char *version = NULL;
            if (!parse_ide_directory(name, &display_name, &version)) continue;
            char *quota_path = g_build_filename(root, name, "options", "AIAssistantQuotaManager2.xml", NULL);
            GStatBuf information;
            if (g_stat(quota_path, &information) != 0) {
                g_free(quota_path);
                continue;
            }
            gint64 mtime = (gint64)information.st_mtim.tv_sec * G_TIME_SPAN_SECOND +
                           information.st_mtim.tv_nsec / 1000;
            gboolean preferred_tie = mtime == selected_mtime && selected_name &&
                                     (g_strcmp0(display_name, selected_name) < 0 ||
                                      (g_str_equal(display_name, selected_name) &&
                                       compare_versions(version, selected_version) > 0));
            if (!selected_path || mtime > selected_mtime || preferred_tie) {
                g_free(selected_path);
                g_free(selected_ide);
                g_free(selected_name);
                g_free(selected_version);
                selected_path = quota_path;
                selected_ide = g_strdup_printf("%s %s", display_name, version);
                selected_name = g_strdup(display_name);
                selected_version = g_strdup(version);
                selected_mtime = mtime;
            } else {
                g_free(quota_path);
            }
        }
        g_dir_close(directory);
        g_free(root);
    }
    if (!selected_path) {
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_NOT_FOUND,
                            "No JetBrains IDE with AI Assistant detected. Install a JetBrains IDE and enable AI Assistant.");
        return NULL;
    }
    g_free(selected_name);
    g_free(selected_version);
    gsize length = 0;
    char *xml = read_quota_file(selected_path, &length, error);
    if (!xml) {
        g_free(selected_path);
        g_free(selected_ide);
        return NULL;
    }
    CodexBarProvider *provider = codexbar_jetbrains_parse_xml(xml, length, selected_ide, now_ms, error);
    g_free(xml);
    g_free(selected_path);
    g_free(selected_ide);
    return provider;
}

CodexBarProvider *codexbar_jetbrains_fetch(GError **error) {
    return codexbar_jetbrains_fetch_from_home(g_get_home_dir(), g_get_real_time() / 1000, error);
}
