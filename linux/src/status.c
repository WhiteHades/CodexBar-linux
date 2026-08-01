#include "status.h"

#include <json-c/json.h>
#include <limits.h>
#include <string.h>

#define STATUS_RESPONSE_LIMIT (8U * 1024U * 1024U)
#define WORKSPACE_STATUS_URL "https://www.google.com/appsstatus/dashboard/incidents.json"

typedef struct {
    json_object *object;
    gint64 position;
    guint order;
} RawStatusComponent;

static void invalid_data(GError **error, const char *message) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, message);
}

static json_object *parse_json(const char *json, size_t length, GError **error) {
    if (!json || length > INT_MAX) {
        invalid_data(error, "Status response is not valid JSON");
        return NULL;
    }
    json_tokener *tokener = json_tokener_new();
    json_object *root = json_tokener_parse_ex(tokener, json, (int)length);
    enum json_tokener_error parse_error = json_tokener_get_error(tokener);
    size_t consumed = json_tokener_get_parse_end(tokener);
    while (consumed < length && g_ascii_isspace(json[consumed])) consumed++;
    gboolean valid = parse_error == json_tokener_success && root && consumed == length;
    json_tokener_free(tokener);
    if (!valid) {
        if (root) json_object_put(root);
        invalid_data(error, "Status response is not valid JSON");
        return NULL;
    }
    return root;
}

static gboolean required_string(json_object *object, const char *key, const char **value, GError **error) {
    json_object *entry = NULL;
    if (!object || !json_object_is_type(object, json_type_object) ||
        !json_object_object_get_ex(object, key, &entry) || !json_object_is_type(entry, json_type_string)) {
        invalid_data(error, "Status response is missing a required string");
        return FALSE;
    }
    *value = json_object_get_string(entry);
    return TRUE;
}

static gboolean optional_string(json_object *object,
                                const char *key,
                                const char **value,
                                GError **error) {
    json_object *entry = NULL;
    *value = NULL;
    if (!json_object_object_get_ex(object, key, &entry) || json_object_is_type(entry, json_type_null)) return TRUE;
    if (!json_object_is_type(entry, json_type_string)) {
        invalid_data(error, "Status response contains an invalid string");
        return FALSE;
    }
    *value = json_object_get_string(entry);
    return TRUE;
}

static gboolean optional_boolean(json_object *object,
                                 const char *key,
                                 gboolean *has_value,
                                 gboolean *value,
                                 GError **error) {
    json_object *entry = NULL;
    *has_value = FALSE;
    *value = FALSE;
    if (!json_object_object_get_ex(object, key, &entry) || json_object_is_type(entry, json_type_null)) return TRUE;
    if (!json_object_is_type(entry, json_type_boolean)) {
        invalid_data(error, "Status response contains an invalid boolean");
        return FALSE;
    }
    *has_value = TRUE;
    *value = json_object_get_boolean(entry);
    return TRUE;
}

static gboolean parse_date(const char *text, gint64 *timestamp, GError **error) {
    GDateTime *date = g_date_time_new_from_iso8601(text, NULL);
    if (!date) {
        invalid_data(error, "Status response contains an invalid date");
        return FALSE;
    }
    *timestamp = g_date_time_to_unix(date) * 1000 + g_date_time_get_microsecond(date) / 1000;
    g_date_time_unref(date);
    return TRUE;
}

static gboolean optional_date(json_object *object,
                              const char *key,
                              gboolean *has_value,
                              gint64 *timestamp,
                              GError **error) {
    const char *text = NULL;
    if (!optional_string(object, key, &text, error)) return FALSE;
    *has_value = text != NULL;
    return !text || parse_date(text, timestamp, error);
}

static CodexBarServiceStatusIndicator status_indicator(const char *raw) {
    if (g_str_equal(raw, "none")) return CODEXBAR_STATUS_NONE;
    if (g_str_equal(raw, "minor")) return CODEXBAR_STATUS_MINOR;
    if (g_str_equal(raw, "major")) return CODEXBAR_STATUS_MAJOR;
    if (g_str_equal(raw, "critical")) return CODEXBAR_STATUS_CRITICAL;
    if (g_str_equal(raw, "maintenance")) return CODEXBAR_STATUS_MAINTENANCE;
    if (g_str_equal(raw, "unknown")) return CODEXBAR_STATUS_UNKNOWN;
    return CODEXBAR_STATUS_UNKNOWN;
}

static CodexBarServiceStatusIndicator component_indicator(const char *raw) {
    if (g_str_equal(raw, "operational")) return CODEXBAR_STATUS_NONE;
    if (g_str_equal(raw, "degraded_performance")) return CODEXBAR_STATUS_MINOR;
    if (g_str_equal(raw, "partial_outage")) return CODEXBAR_STATUS_MAJOR;
    if (g_str_equal(raw, "major_outage") || g_str_equal(raw, "full_outage")) {
        return CODEXBAR_STATUS_CRITICAL;
    }
    if (g_str_equal(raw, "under_maintenance")) return CODEXBAR_STATUS_MAINTENANCE;
    return CODEXBAR_STATUS_UNKNOWN;
}

static int indicator_rank(CodexBarServiceStatusIndicator indicator) {
    switch (indicator) {
    case CODEXBAR_STATUS_NONE: return 0;
    case CODEXBAR_STATUS_MAINTENANCE:
    case CODEXBAR_STATUS_UNKNOWN: return 1;
    case CODEXBAR_STATUS_MINOR: return 2;
    case CODEXBAR_STATUS_MAJOR: return 3;
    case CODEXBAR_STATUS_CRITICAL: return 4;
    }
    return 1;
}

static char *normalized_name(const char *name) {
    if (!name) return NULL;
    char *copy = g_strdup(name);
    g_strstrip(copy);
    if (copy[0] == '\0') {
        g_free(copy);
        return NULL;
    }
    return copy;
}

static CodexBarStatusComponent *status_component_new(const char *id,
                                                     const char *name,
                                                     const char *raw_status) {
    CodexBarStatusComponent *component = g_new0(CodexBarStatusComponent, 1);
    component->id = g_strdup(id);
    component->name = g_strdup(name);
    component->indicator = component_indicator(raw_status);
    component->status = g_strdup(raw_status);
    component->children = g_ptr_array_new_with_free_func((GDestroyNotify)codexbar_status_component_free);
    return component;
}

void codexbar_status_component_free(CodexBarStatusComponent *component) {
    if (!component) return;
    g_free(component->id);
    g_free(component->name);
    g_free(component->status);
    g_ptr_array_unref(component->children);
    g_free(component);
}

void codexbar_status_summary_free(CodexBarStatusSummary *summary) {
    if (!summary) return;
    codexbar_service_status_free(summary->status);
    if (summary->components) g_ptr_array_unref(summary->components);
    g_free(summary);
}

CodexBarServiceStatus *codexbar_statuspage_parse_status(const char *json,
                                                        size_t length,
                                                        GError **error) {
    json_object *root = parse_json(json, length, error);
    if (!root) return NULL;
    json_object *status_object = NULL;
    const char *indicator = NULL;
    const char *description = NULL;
    if (!json_object_is_type(root, json_type_object) ||
        !json_object_object_get_ex(root, "status", &status_object) ||
        !json_object_is_type(status_object, json_type_object) ||
        !required_string(status_object, "indicator", &indicator, error) ||
        !optional_string(status_object, "description", &description, error)) {
        if (!error || !*error) invalid_data(error, "Status response is missing status information");
        json_object_put(root);
        return NULL;
    }

    gboolean has_updated_at = FALSE;
    gint64 updated_at_ms = 0;
    json_object *page = NULL;
    if (json_object_object_get_ex(root, "page", &page) && !json_object_is_type(page, json_type_null)) {
        if (!json_object_is_type(page, json_type_object) ||
            !optional_date(page, "updated_at", &has_updated_at, &updated_at_ms, error)) {
            if (!error || !*error) invalid_data(error, "Status response contains an invalid page");
            json_object_put(root);
            return NULL;
        }
    }

    CodexBarServiceStatus *result = g_new0(CodexBarServiceStatus, 1);
    result->indicator = status_indicator(indicator);
    result->description = g_strdup(description);
    result->has_updated_at = has_updated_at;
    result->updated_at_ms = updated_at_ms;
    json_object_put(root);
    return result;
}

static void raw_component_free(RawStatusComponent *component) {
    g_free(component);
}

static gint compare_raw_components(gconstpointer left, gconstpointer right) {
    const RawStatusComponent *left_component = *(RawStatusComponent *const *)left;
    const RawStatusComponent *right_component = *(RawStatusComponent *const *)right;
    if (left_component->position < right_component->position) return -1;
    if (left_component->position > right_component->position) return 1;
    return left_component->order < right_component->order ? -1 : left_component->order > right_component->order;
}

static gboolean optional_group_id(json_object *object, const char **group_id, GError **error) {
    return optional_string(object, "group_id", group_id, error);
}

static CodexBarStatusComponent *component_from_statuspage(json_object *object, GError **error) {
    const char *id = NULL;
    const char *name = NULL;
    const char *status = NULL;
    if (!required_string(object, "id", &id, error) || !required_string(object, "name", &name, error) ||
        !required_string(object, "status", &status, error)) {
        return NULL;
    }
    char *normalized = normalized_name(name);
    if (!normalized) return NULL;
    CodexBarStatusComponent *component = status_component_new(id, normalized, status);
    g_free(normalized);
    return component;
}

GPtrArray *codexbar_statuspage_parse_components(const char *json, size_t length, GError **error) {
    json_object *root = parse_json(json, length, error);
    if (!root) return NULL;
    json_object *array = NULL;
    if (!json_object_is_type(root, json_type_object)) {
        invalid_data(error, "Status components response is not an object");
        json_object_put(root);
        return NULL;
    }
    if (!json_object_object_get_ex(root, "components", &array) || json_object_is_type(array, json_type_null)) {
        json_object_put(root);
        return g_ptr_array_new_with_free_func((GDestroyNotify)codexbar_status_component_free);
    }
    if (!json_object_is_type(array, json_type_array)) {
        invalid_data(error, "Status components response is not an array");
        json_object_put(root);
        return NULL;
    }

    GPtrArray *raw = g_ptr_array_new_with_free_func((GDestroyNotify)raw_component_free);
    for (size_t index = 0; index < json_object_array_length(array); index++) {
        json_object *object = json_object_array_get_idx(array, index);
        const char *id = NULL;
        const char *name = NULL;
        const char *status = NULL;
        const char *group_id = NULL;
        gboolean has_group = FALSE;
        gboolean group = FALSE;
        json_object *position = NULL;
        if (!json_object_is_type(object, json_type_object) || !required_string(object, "id", &id, error) ||
            !required_string(object, "name", &name, error) || !required_string(object, "status", &status, error) ||
            !optional_boolean(object, "group", &has_group, &group, error) ||
            !optional_group_id(object, &group_id, error)) {
            g_ptr_array_unref(raw);
            json_object_put(root);
            return NULL;
        }
        gint64 position_value = 0;
        if (json_object_object_get_ex(object, "position", &position) && !json_object_is_type(position, json_type_null)) {
            if (!json_object_is_type(position, json_type_int)) {
                invalid_data(error, "Status component position is not an integer");
                g_ptr_array_unref(raw);
                json_object_put(root);
                return NULL;
            }
            position_value = json_object_get_int64(position);
        }
        RawStatusComponent *component = g_new0(RawStatusComponent, 1);
        component->object = object;
        component->position = position_value;
        component->order = (guint)index;
        g_ptr_array_add(raw, component);
    }
    g_ptr_array_sort(raw, compare_raw_components);

    GHashTable *children_by_group = g_hash_table_new_full(
        g_str_hash, g_str_equal, g_free, (GDestroyNotify)g_ptr_array_unref);
    for (guint index = 0; index < raw->len; index++) {
        RawStatusComponent *raw_component = g_ptr_array_index(raw, index);
        gboolean has_group = FALSE;
        gboolean group = FALSE;
        const char *group_id = NULL;
        optional_boolean(raw_component->object, "group", &has_group, &group, NULL);
        optional_group_id(raw_component->object, &group_id, NULL);
        if ((has_group && group) || !group_id) continue;
        CodexBarStatusComponent *child = component_from_statuspage(raw_component->object, NULL);
        if (!child) continue;
        GPtrArray *children = g_hash_table_lookup(children_by_group, group_id);
        if (!children) {
            children = g_ptr_array_new_with_free_func((GDestroyNotify)codexbar_status_component_free);
            g_hash_table_insert(children_by_group, g_strdup(group_id), children);
        }
        g_ptr_array_add(children, child);
    }

    GPtrArray *result = g_ptr_array_new_with_free_func((GDestroyNotify)codexbar_status_component_free);
    for (guint index = 0; index < raw->len; index++) {
        RawStatusComponent *raw_component = g_ptr_array_index(raw, index);
        gboolean has_group = FALSE;
        gboolean group = FALSE;
        const char *group_id = NULL;
        optional_boolean(raw_component->object, "group", &has_group, &group, NULL);
        optional_group_id(raw_component->object, &group_id, NULL);
        if (!(has_group && group) && group_id) continue;
        CodexBarStatusComponent *component = component_from_statuspage(raw_component->object, NULL);
        if (!component) continue;
        if (has_group && group) {
            GPtrArray *children = g_hash_table_lookup(children_by_group, component->id);
            if (children) {
                g_ptr_array_unref(component->children);
                component->children = g_ptr_array_ref(children);
            }
        }
        g_ptr_array_add(result, component);
    }

    g_hash_table_unref(children_by_group);
    g_ptr_array_unref(raw);
    json_object_put(root);
    return result;
}

static gboolean optional_object(json_object *object,
                                const char *key,
                                json_object **value,
                                GError **error) {
    *value = NULL;
    if (!json_object_object_get_ex(object, key, value) || json_object_is_type(*value, json_type_null)) {
        *value = NULL;
        return TRUE;
    }
    if (!json_object_is_type(*value, json_type_object)) {
        invalid_data(error, "Status response contains an invalid object");
        return FALSE;
    }
    return TRUE;
}

static gboolean optional_array(json_object *object,
                               const char *key,
                               json_object **value,
                               GError **error) {
    *value = NULL;
    if (!json_object_object_get_ex(object, key, value) || json_object_is_type(*value, json_type_null)) {
        *value = NULL;
        return TRUE;
    }
    if (!json_object_is_type(*value, json_type_array)) {
        invalid_data(error, "Status response contains an invalid array");
        return FALSE;
    }
    return TRUE;
}

static gboolean validate_hidden(json_object *object, gboolean *hidden, GError **error) {
    gboolean has_hidden = FALSE;
    return optional_boolean(object, "hidden", &has_hidden, hidden, error);
}

static CodexBarStatusComponent *incident_leaf(const char *id,
                                              const char *name,
                                              GHashTable *status_by_id) {
    const char *raw = g_hash_table_lookup(status_by_id, id);
    return status_component_new(id, name, raw ? raw : "operational");
}

CodexBarStatusSummary *codexbar_incident_io_parse_summary(const char *json,
                                                          size_t length,
                                                          GError **error) {
    json_object *root = parse_json(json, length, error);
    if (!root) return NULL;
    json_object *summary_object = NULL;
    json_object *structure = NULL;
    json_object *items = NULL;
    if (!json_object_is_type(root, json_type_object) ||
        !json_object_object_get_ex(root, "summary", &summary_object) ||
        !json_object_is_type(summary_object, json_type_object) ||
        !optional_object(summary_object, "structure", &structure, error) || !structure ||
        !optional_array(structure, "items", &items, error) || !items || json_object_array_length(items) == 0) {
        if (!error || !*error) invalid_data(error, "Incident.io summary has no component structure");
        json_object_put(root);
        return NULL;
    }

    GHashTable *status_by_id = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    json_object *affected = NULL;
    if (!optional_array(summary_object, "affected_components", &affected, error)) {
        g_hash_table_unref(status_by_id);
        json_object_put(root);
        return NULL;
    }
    for (size_t index = 0; affected && index < json_object_array_length(affected); index++) {
        json_object *object = json_object_array_get_idx(affected, index);
        const char *id = NULL;
        const char *status = NULL;
        if (!required_string(object, "component_id", &id, error) ||
            !optional_string(object, "status", &status, error)) {
            g_hash_table_unref(status_by_id);
            json_object_put(root);
            return NULL;
        }
        g_hash_table_replace(status_by_id, g_strdup(id), g_strdup(status));
    }

    GPtrArray *components = g_ptr_array_new_with_free_func((GDestroyNotify)codexbar_status_component_free);
    for (size_t index = 0; index < json_object_array_length(items); index++) {
        json_object *item = json_object_array_get_idx(items, index);
        json_object *group = NULL;
        json_object *top_component = NULL;
        if (!json_object_is_type(item, json_type_object) || !optional_object(item, "group", &group, error) ||
            !optional_object(item, "component", &top_component, error)) {
            g_ptr_array_unref(components);
            g_hash_table_unref(status_by_id);
            json_object_put(root);
            return NULL;
        }

        gboolean group_hidden = FALSE;
        const char *group_id = NULL;
        const char *group_name = NULL;
        json_object *children = NULL;
        if (group &&
            (!required_string(group, "id", &group_id, error) ||
             !optional_string(group, "name", &group_name, error) ||
             !validate_hidden(group, &group_hidden, error) ||
             !optional_array(group, "components", &children, error))) {
            g_ptr_array_unref(components);
            g_hash_table_unref(status_by_id);
            json_object_put(root);
            return NULL;
        }
        for (size_t child_index = 0; children && child_index < json_object_array_length(children); child_index++) {
            json_object *child = json_object_array_get_idx(children, child_index);
            const char *unused_id = NULL;
            const char *unused_name = NULL;
            gboolean unused_hidden = FALSE;
            if (!required_string(child, "component_id", &unused_id, error) ||
                !optional_string(child, "name", &unused_name, error) ||
                !validate_hidden(child, &unused_hidden, error)) {
                g_ptr_array_unref(components);
                g_hash_table_unref(status_by_id);
                json_object_put(root);
                return NULL;
            }
        }

        gboolean component_hidden = FALSE;
        const char *component_id = NULL;
        const char *component_name = NULL;
        if (top_component &&
            (!required_string(top_component, "component_id", &component_id, error) ||
             !optional_string(top_component, "name", &component_name, error) ||
             !validate_hidden(top_component, &component_hidden, error))) {
            g_ptr_array_unref(components);
            g_hash_table_unref(status_by_id);
            json_object_put(root);
            return NULL;
        }

        if (group && !group_hidden) {
            char *normalized_group_name = normalized_name(group_name);
            if (!normalized_group_name) continue;
            CodexBarStatusComponent *group_component = status_component_new(group_id, normalized_group_name, "operational");
            g_free(normalized_group_name);
            for (size_t child_index = 0; children && child_index < json_object_array_length(children); child_index++) {
                json_object *child = json_object_array_get_idx(children, child_index);
                gboolean hidden = FALSE;
                const char *id = NULL;
                const char *name = NULL;
                required_string(child, "component_id", &id, NULL);
                optional_string(child, "name", &name, NULL);
                validate_hidden(child, &hidden, NULL);
                char *normalized = !hidden ? normalized_name(name) : NULL;
                if (!normalized) continue;
                CodexBarStatusComponent *leaf = incident_leaf(id, normalized, status_by_id);
                g_free(normalized);
                g_ptr_array_add(group_component->children, leaf);
                if (indicator_rank(leaf->indicator) > indicator_rank(group_component->indicator)) {
                    group_component->indicator = leaf->indicator;
                    g_free(group_component->status);
                    group_component->status = g_strdup(leaf->status);
                }
            }
            g_ptr_array_add(components, group_component);
        } else if (top_component && !component_hidden) {
            char *normalized = normalized_name(component_name);
            if (!normalized) continue;
            g_ptr_array_add(components, incident_leaf(component_id, normalized, status_by_id));
            g_free(normalized);
        }
    }

    CodexBarServiceStatusIndicator overall = CODEXBAR_STATUS_NONE;
    for (guint index = 0; index < components->len; index++) {
        CodexBarStatusComponent *component = g_ptr_array_index(components, index);
        if (component->children->len > 0) {
            for (guint child_index = 0; child_index < component->children->len; child_index++) {
                CodexBarStatusComponent *child = g_ptr_array_index(component->children, child_index);
                if (indicator_rank(child->indicator) > indicator_rank(overall)) overall = child->indicator;
            }
        } else if (indicator_rank(component->indicator) > indicator_rank(overall)) {
            overall = component->indicator;
        }
    }
    CodexBarStatusSummary *result = g_new0(CodexBarStatusSummary, 1);
    result->status = g_new0(CodexBarServiceStatus, 1);
    result->status->indicator = overall;
    result->components = components;
    g_hash_table_unref(status_by_id);
    json_object_put(root);
    return result;
}

static gboolean product_array_contains(json_object *array,
                                       const char *product_id,
                                       gboolean *contains,
                                       GError **error) {
    *contains = FALSE;
    for (size_t index = 0; array && index < json_object_array_length(array); index++) {
        json_object *product = json_object_array_get_idx(array, index);
        const char *id = NULL;
        const char *title = NULL;
        if (!required_string(product, "id", &id, error) || !optional_string(product, "title", &title, error)) {
            return FALSE;
        }
        if (g_str_equal(id, product_id)) *contains = TRUE;
    }
    return TRUE;
}

static gboolean validate_workspace_update(json_object *update, GError **error) {
    const char *status = NULL;
    const char *text = NULL;
    gboolean has_when = FALSE;
    gint64 when_ms = 0;
    return json_object_is_type(update, json_type_object) &&
           optional_date(update, "when", &has_when, &when_ms, error) &&
           optional_string(update, "status", &status, error) && optional_string(update, "text", &text, error);
}

static CodexBarServiceStatusIndicator workspace_indicator(const char *status, const char *severity) {
    if (status) {
        if (g_ascii_strcasecmp(status, "AVAILABLE") == 0) return CODEXBAR_STATUS_NONE;
        if (g_ascii_strcasecmp(status, "SERVICE_INFORMATION") == 0) return CODEXBAR_STATUS_MINOR;
        if (g_ascii_strcasecmp(status, "SERVICE_DISRUPTION") == 0) return CODEXBAR_STATUS_MAJOR;
        if (g_ascii_strcasecmp(status, "SERVICE_OUTAGE") == 0) return CODEXBAR_STATUS_CRITICAL;
        if (g_ascii_strcasecmp(status, "SERVICE_MAINTENANCE") == 0 ||
            g_ascii_strcasecmp(status, "SCHEDULED_MAINTENANCE") == 0) {
            return CODEXBAR_STATUS_MAINTENANCE;
        }
    }
    if (severity) {
        if (g_ascii_strcasecmp(severity, "low") == 0) return CODEXBAR_STATUS_MINOR;
        if (g_ascii_strcasecmp(severity, "medium") == 0) return CODEXBAR_STATUS_MAJOR;
        if (g_ascii_strcasecmp(severity, "high") == 0) return CODEXBAR_STATUS_CRITICAL;
    }
    return CODEXBAR_STATUS_MINOR;
}

static char *replace_all(const char *text, const char *needle, const char *replacement) {
    char **parts = g_strsplit(text, needle, -1);
    char *result = g_strjoinv(replacement, parts);
    g_strfreev(parts);
    return result;
}

static char *workspace_summary(const char *text) {
    if (!text) return NULL;
    char *normalized = replace_all(text, "\r\n", "\n");
    char *without_cr = replace_all(normalized, "\r", "\n");
    g_free(normalized);
    char **lines = g_strsplit(without_cr, "\n", -1);
    g_free(without_cr);
    GRegex *link = g_regex_new("\\[([^]]+)\\]\\([^)]+\\)", 0, 0, NULL);
    char *result = NULL;
    for (guint index = 0; lines[index]; index++) {
        char *trimmed = g_strdup(lines[index]);
        g_strstrip(trimmed);
        if (trimmed[0] == '\0') {
            g_free(trimmed);
            continue;
        }
        char *lower = g_ascii_strdown(trimmed, -1);
        gboolean heading = g_str_has_prefix(lower, "**summary") ||
                           g_str_has_prefix(lower, "**description") || g_str_equal(lower, "summary");
        g_free(lower);
        if (heading) {
            g_free(trimmed);
            continue;
        }
        char *without_bold = replace_all(trimmed, "**", "");
        g_free(trimmed);
        char *without_links = g_regex_replace(link, without_bold, -1, 0, "\\1", 0, NULL);
        g_free(without_bold);
        if (g_str_has_prefix(without_links, "- ")) memmove(without_links, without_links + 2, strlen(without_links) - 1);
        g_strstrip(without_links);
        if (without_links[0] != '\0') {
            result = without_links;
            break;
        }
        g_free(without_links);
    }
    g_regex_unref(link);
    g_strfreev(lines);
    return result;
}

CodexBarServiceStatus *codexbar_workspace_parse_status(const char *json,
                                                       size_t length,
                                                       const char *product_id,
                                                       GError **error) {
    json_object *root = parse_json(json, length, error);
    if (!root) return NULL;
    if (!product_id || product_id[0] == '\0' || !json_object_is_type(root, json_type_array)) {
        invalid_data(error, "Workspace status response is invalid");
        json_object_put(root);
        return NULL;
    }

    json_object *best_incident = NULL;
    json_object *best_update = NULL;
    CodexBarServiceStatusIndicator best_indicator = CODEXBAR_STATUS_NONE;
    for (size_t index = 0; index < json_object_array_length(root); index++) {
        json_object *incident = json_object_array_get_idx(root, index);
        const char *external_description = NULL;
        const char *status_impact = NULL;
        const char *severity = NULL;
        gboolean has_begin = FALSE;
        gboolean has_end = FALSE;
        gboolean has_modified = FALSE;
        gint64 begin_ms = 0;
        gint64 end_ms = 0;
        gint64 modified_ms = 0;
        json_object *affected = NULL;
        json_object *currently_affected = NULL;
        json_object *recent = NULL;
        json_object *updates = NULL;
        if (!json_object_is_type(incident, json_type_object) ||
            !optional_date(incident, "begin", &has_begin, &begin_ms, error) ||
            !optional_date(incident, "end", &has_end, &end_ms, error) ||
            !optional_date(incident, "modified", &has_modified, &modified_ms, error) ||
            !optional_string(incident, "external_desc", &external_description, error) ||
            !optional_string(incident, "status_impact", &status_impact, error) ||
            !optional_string(incident, "severity", &severity, error) ||
            !optional_array(incident, "affected_products", &affected, error) ||
            !optional_array(incident, "currently_affected_products", &currently_affected, error) ||
            !optional_object(incident, "most_recent_update", &recent, error) ||
            !optional_array(incident, "updates", &updates, error)) {
            json_object_put(root);
            return NULL;
        }
        gboolean relevant = FALSE;
        if (!product_array_contains(currently_affected ? currently_affected : affected,
                                    product_id,
                                    &relevant,
                                    error)) {
            json_object_put(root);
            return NULL;
        }
        if (recent && !validate_workspace_update(recent, error)) {
            if (!error || !*error) invalid_data(error, "Workspace status update is invalid");
            json_object_put(root);
            return NULL;
        }
        for (size_t update_index = 0; updates && update_index < json_object_array_length(updates); update_index++) {
            if (!validate_workspace_update(json_object_array_get_idx(updates, update_index), error)) {
                if (!error || !*error) invalid_data(error, "Workspace status update is invalid");
                json_object_put(root);
                return NULL;
            }
        }
        if (has_end || !relevant) continue;
        json_object *update = recent;
        if (!update && updates && json_object_array_length(updates) > 0) {
            update = json_object_array_get_idx(updates, json_object_array_length(updates) - 1);
        }
        const char *update_status = NULL;
        if (update) optional_string(update, "status", &update_status, NULL);
        CodexBarServiceStatusIndicator indicator = workspace_indicator(update_status ? update_status : status_impact,
                                                                       severity);
        if (!best_incident || indicator_rank(indicator) > indicator_rank(best_indicator)) {
            best_incident = incident;
            best_update = update;
            best_indicator = indicator;
        }
    }

    CodexBarServiceStatus *result = g_new0(CodexBarServiceStatus, 1);
    result->indicator = best_indicator;
    if (best_incident) {
        const char *text = NULL;
        if (best_update) optional_string(best_update, "text", &text, NULL);
        if (!text) optional_string(best_incident, "external_desc", &text, NULL);
        result->description = workspace_summary(text);
        if (best_update && optional_date(best_update, "when", &result->has_updated_at, &result->updated_at_ms, NULL) &&
            result->has_updated_at) {
            json_object_put(root);
            return result;
        }
        optional_date(best_incident, "modified", &result->has_updated_at, &result->updated_at_ms, NULL);
        if (!result->has_updated_at) {
            optional_date(best_incident, "begin", &result->has_updated_at, &result->updated_at_ms, NULL);
        }
    }
    json_object_put(root);
    return result;
}

static CodexBarHttpResponse *default_transport(const CodexBarHttpRequest *request, GError **error) {
    return codexbar_http_send(request, error);
}

static CodexBarHttpResponse *fetch_url(const char *url,
                                      CodexBarStatusTransport transport,
                                      GCancellable *cancellable,
                                      GError **error) {
    CodexBarHttpRequest request = {
        .url = url,
        .method = "GET",
        .timeout_seconds = 10,
        .maximum_response_bytes = STATUS_RESPONSE_LIMIT,
        .protocol_policy = CODEXBAR_HTTP_HTTPS_ONLY,
        .redirect_policy = CODEXBAR_HTTP_REDIRECT_SAME_ORIGIN,
        .cancellable = cancellable,
    };
    return (transport ? transport : default_transport)(&request, error);
}

static char *append_path(const char *base_url, const char *path, GError **error) {
    GUri *uri = base_url ? g_uri_parse(base_url, G_URI_FLAGS_NONE, error) : NULL;
    if (!uri || !g_uri_get_scheme(uri) || !g_uri_get_host(uri)) {
        if (uri) g_uri_unref(uri);
        if (!error || !*error) invalid_data(error, "Status page URL is invalid");
        return NULL;
    }
    g_uri_unref(uri);
    char *base = g_strdup(base_url);
    while (strlen(base) > 0 && base[strlen(base) - 1] == '/') base[strlen(base) - 1] = '\0';
    char *result = g_strdup_printf("%s/%s", base, path);
    g_free(base);
    return result;
}

CodexBarStatusSummary *codexbar_statuspage_fetch_summary(const char *base_url,
                                                         CodexBarStatusTransport transport,
                                                         GCancellable *cancellable,
                                                         GError **error) {
    GUri *base = base_url ? g_uri_parse(base_url, G_URI_FLAGS_NONE, error) : NULL;
    const char *host = base ? g_uri_get_host(base) : NULL;
    if (!base || !host) {
        if (base) g_uri_unref(base);
        if (!error || !*error) invalid_data(error, "Status page URL is invalid");
        return NULL;
    }
    char *proxy_url = g_strdup_printf("https://%s/proxy/%s", host, host);
    g_uri_unref(base);

    GError *ignored = NULL;
    CodexBarHttpResponse *response = fetch_url(proxy_url, transport, cancellable, &ignored);
    CodexBarStatusSummary *incident = response
                                          ? codexbar_incident_io_parse_summary(
                                                response->body, response->body_length, &ignored)
                                          : NULL;
    codexbar_http_response_free(response);
    g_clear_error(&ignored);
    g_free(proxy_url);
    if (incident) {
        char *status_url = append_path(base_url, "api/v2/status.json", NULL);
        response = status_url ? fetch_url(status_url, transport, cancellable, &ignored) : NULL;
        CodexBarServiceStatus *overlay = response
                                             ? codexbar_statuspage_parse_status(
                                                   response->body, response->body_length, &ignored)
                                             : NULL;
        codexbar_http_response_free(response);
        g_clear_error(&ignored);
        g_free(status_url);
        if (overlay) {
            overlay->indicator = incident->status->indicator;
            codexbar_service_status_free(incident->status);
            incident->status = overlay;
        }
        return incident;
    }

    char *summary_url = append_path(base_url, "api/v2/summary.json", error);
    if (!summary_url) return NULL;
    response = fetch_url(summary_url, transport, cancellable, error);
    g_free(summary_url);
    if (!response) return NULL;
    CodexBarServiceStatus *status =
        codexbar_statuspage_parse_status(response->body, response->body_length, error);
    codexbar_http_response_free(response);
    if (!status) return NULL;

    char *components_url = append_path(base_url, "api/v2/components.json", NULL);
    response = components_url ? fetch_url(components_url, transport, cancellable, &ignored) : NULL;
    GPtrArray *components = response
                                ? codexbar_statuspage_parse_components(
                                      response->body, response->body_length, &ignored)
                                : NULL;
    codexbar_http_response_free(response);
    g_clear_error(&ignored);
    g_free(components_url);
    CodexBarStatusSummary *result = g_new0(CodexBarStatusSummary, 1);
    result->status = status;
    result->components = components;
    return result;
}

CodexBarServiceStatus *codexbar_workspace_fetch_status(const char *product_id,
                                                       CodexBarStatusTransport transport,
                                                       GCancellable *cancellable,
                                                       GError **error) {
    CodexBarHttpResponse *response = fetch_url(WORKSPACE_STATUS_URL, transport, cancellable, error);
    if (!response) return NULL;
    CodexBarServiceStatus *status =
        codexbar_workspace_parse_status(response->body, response->body_length, product_id, error);
    codexbar_http_response_free(response);
    return status;
}
