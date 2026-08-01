#include "grok.h"

#include "process.h"

#include <json-c/json.h>
#include <math.h>
#include <string.h>

#define RESPONSE_LIMIT (1024U * 1024U)
#define CREDENTIAL_LIMIT 65536U

typedef struct {
    char *token;
    char *email;
    char *team_id;
    char *login_method;
    char *principal_type;
    gboolean has_expiry;
    gint64 expiry_ms;
} GrokCredentials;

typedef struct {
    gboolean has_percent;
    double percent;
    guint percent_depth;
    guint percent_order;
    guint next_order;
    gboolean has_preferred_reset;
    gint64 preferred_reset_ms;
    gboolean has_fallback_reset;
    gint64 fallback_reset_ms;
    gboolean has_usage_period;
} ProtoScan;

static json_object *parse_json(const char *text, size_t length) {
    if (!text || !length || length > RESPONSE_LIMIT || length > G_MAXINT ||
        memchr(text, '\0', length) || !g_utf8_validate(text, (gssize)length, NULL)) return NULL;
    json_tokener *tokener = json_tokener_new();
    json_tokener_set_flags(tokener, JSON_TOKENER_STRICT | JSON_TOKENER_VALIDATE_UTF8);
    json_object *root = json_tokener_parse_ex(tokener, text, (int)length);
    enum json_tokener_error parse_error = json_tokener_get_error(tokener);
    size_t consumed = json_tokener_get_parse_end(tokener);
    while (consumed < length && strchr(" \t\r\n", text[consumed])) consumed++;
    gboolean valid = parse_error == json_tokener_success && root && consumed == length;
    json_tokener_free(tokener);
    if (valid) return root;
    if (root) json_object_put(root);
    return NULL;
}

static json_object *member(json_object *object, const char *key) {
    json_object *value = NULL;
    return object && json_object_is_type(object, json_type_object) &&
                   json_object_object_get_ex(object, key, &value)
               ? value
               : NULL;
}

static char *string_member(json_object *object, const char *key) {
    json_object *value = member(object, key);
    if (!value || !json_object_is_type(value, json_type_string)) return NULL;
    const char *raw = json_object_get_string(value);
    size_t length = (size_t)json_object_get_string_len(value);
    if (!raw || length > CREDENTIAL_LIMIT || memchr(raw, '\0', length) ||
        !g_utf8_validate(raw, (gssize)length, NULL)) return NULL;
    char *copy = g_strstrip(g_strndup(raw, length));
    if (copy[0]) return copy;
    g_free(copy);
    return NULL;
}

static gboolean integer_value(json_object *value, gint64 *result) {
    if (!value || !json_object_is_type(value, json_type_int)) return FALSE;
    *result = json_object_get_int64(value);
    return TRUE;
}

static gboolean timestamp_text(const char *text, gint64 *result) {
    if (!text) return FALSE;
    GDateTime *date = g_date_time_new_from_iso8601(text, NULL);
    if (!date) return FALSE;
    *result = g_date_time_to_unix(date) * 1000 + g_date_time_get_microsecond(date) / 1000;
    g_date_time_unref(date);
    return TRUE;
}

static CodexBarProvider *new_provider(const char *source, gint64 now_ms) {
    CodexBarProvider *provider = codexbar_provider_new();
    provider->provider = g_strdup("grok");
    provider->source = g_strdup(source);
    provider->has_updated_at = TRUE;
    provider->updated_at_ms = now_ms;
    return provider;
}

CodexBarProvider *codexbar_grok_parse_billing(const char *json,
                                              size_t length,
                                              gint64 now_ms,
                                              GError **error) {
    json_object *root = parse_json(json, length);
    if (!root || !json_object_is_type(root, json_type_object)) {
        if (root) json_object_put(root);
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                            "Could not parse Grok billing response");
        return NULL;
    }
    CodexBarProvider *provider = new_provider("cli", now_ms);
    json_object *limit_object = member(root, "monthlyLimit");
    json_object *usage = member(root, "usage");
    json_object *used_object = member(usage, "totalUsed");
    gint64 limit = 0;
    gint64 used = 0;
    if (integer_value(member(limit_object, "val"), &limit) && limit > 0 &&
        integer_value(member(used_object, "val"), &used)) {
        CodexBarQuotaWindow *window = codexbar_quota_window_new("grok.credits", "Credits");
        window->usage_known = TRUE;
        window->used_percent = CLAMP((double)used / (double)limit * 100, 0, 100);
        json_object *cycle = member(root, "billingCycle");
        char *start_text = string_member(cycle, "billingPeriodStart");
        char *end_text = string_member(cycle, "billingPeriodEnd");
        gint64 start_ms = 0;
        gint64 end_ms = 0;
        if (timestamp_text(start_text, &start_ms) && timestamp_text(end_text, &end_ms) && end_ms > start_ms) {
            window->has_window_minutes = TRUE;
            window->window_minutes = (end_ms - start_ms) / 60000;
            window->has_resets_at = TRUE;
            window->resets_at_ms = end_ms;
            provider->has_subscription_renews_at = TRUE;
            provider->subscription_renews_at_ms = end_ms;
        }
        g_free(start_text);
        g_free(end_text);
        codexbar_provider_add_quota_window(provider, window);
    }
    json_object_put(root);
    return provider;
}

static gboolean read_varint(const guint8 *data, size_t length, size_t *offset, guint64 *result) {
    guint64 value = 0;
    for (guint index = 0; index < 10 && *offset < length; index++) {
        guint8 byte = data[(*offset)++];
        if (index == 9 && byte > 1) return FALSE;
        value |= (guint64)(byte & 0x7f) << (index * 7);
        if (!(byte & 0x80)) {
            *result = value;
            return TRUE;
        }
    }
    return FALSE;
}

static gboolean path_is(const guint64 *path, guint depth, guint64 first, guint64 second, guint64 third) {
    return depth == 3 && path[0] == first && path[1] == second && path[2] == third;
}

static void scan_protobuf(const guint8 *data,
                          size_t length,
                          guint depth,
                          guint64 *path,
                          ProtoScan *scan,
                          gint64 now_ms) {
    size_t offset = 0;
    while (offset < length) {
        size_t field_start = offset;
        guint64 key = 0;
        if (!read_varint(data, length, &offset, &key) || !key) {
            offset = field_start + 1;
            continue;
        }
        guint64 field = key >> 3;
        guint wire = (guint)(key & 7);
        path[depth] = field;
        guint path_depth = depth + 1;
        if (wire == 0) {
            guint64 value = 0;
            if (!read_varint(data, length, &offset, &value)) {
                offset = field_start + 1;
                continue;
            }
            if (value >= 1700000000 && value <= 2100000000 && (gint64)value * 1000 > now_ms) {
                gint64 reset_ms = (gint64)value * 1000;
                if (path_is(path, path_depth, 1, 5, 1)) {
                    if (!scan->has_preferred_reset || reset_ms < scan->preferred_reset_ms) {
                        scan->has_preferred_reset = TRUE;
                        scan->preferred_reset_ms = reset_ms;
                    }
                } else if (!scan->has_fallback_reset || reset_ms < scan->fallback_reset_ms) {
                    scan->has_fallback_reset = TRUE;
                    scan->fallback_reset_ms = reset_ms;
                }
            }
            if (path_depth >= 2 && path[0] == 1 && path[1] == 6) scan->has_usage_period = TRUE;
            if (path_is(path, path_depth, 1, 8, 1) && (value == 1 || value == 2)) {
                scan->has_usage_period = TRUE;
            }
        } else if (wire == 1) {
            if (length - offset < 8) return;
            offset += 8;
        } else if (wire == 2) {
            guint64 nested_length = 0;
            if (!read_varint(data, length, &offset, &nested_length) || nested_length > length - offset) {
                offset = field_start + 1;
                continue;
            }
            if (depth < 4) scan_protobuf(data + offset, (size_t)nested_length, depth + 1, path, scan, now_ms);
            offset += (size_t)nested_length;
        } else if (wire == 5) {
            if (length - offset < 4) return;
            guint32 bits = (guint32)data[offset] | (guint32)data[offset + 1] << 8 |
                           (guint32)data[offset + 2] << 16 | (guint32)data[offset + 3] << 24;
            float value = 0;
            memcpy(&value, &bits, sizeof(value));
            if (field == 1 && isfinite(value) && value >= 0 && value <= 100 &&
                (!scan->has_percent || path_depth < scan->percent_depth ||
                 (path_depth == scan->percent_depth && scan->next_order < scan->percent_order))) {
                scan->has_percent = TRUE;
                scan->percent = value;
                scan->percent_depth = path_depth;
                scan->percent_order = scan->next_order;
            }
            scan->next_order++;
            offset += 4;
        } else {
            offset = field_start + 1;
        }
    }
}

static gboolean grpc_status(const guint8 *data, size_t length, GError **error) {
    guint first_wire = length ? data[0] & 7 : 7;
    gboolean looks_like_protobuf = length && (data[0] >> 3) &&
                                   (first_wire == 0 || first_wire == 1 || first_wire == 2 || first_wire == 5);
    size_t offset = 0;
    while (offset < length) {
        if (length - offset < 5) return looks_like_protobuf;
        guint8 flags = data[offset];
        guint32 frame_length = (guint32)data[offset + 1] << 24 | (guint32)data[offset + 2] << 16 |
                               (guint32)data[offset + 3] << 8 | data[offset + 4];
        offset += 5;
        if (frame_length > length - offset) return looks_like_protobuf;
        if (flags & 0x80) {
            if (memchr(data + offset, '\0', frame_length) ||
                !g_utf8_validate((const char *)data + offset, (gssize)frame_length, NULL)) return FALSE;
            char *text = g_strndup((const char *)data + offset, frame_length);
            char **lines = g_strsplit(text, "\n", -1);
            gint64 grpc_code = 0;
            gboolean has_grpc_code = FALSE;
            char *grpc_message = NULL;
            for (size_t index = 0; lines[index]; index++) {
                char *separator = strchr(lines[index], ':');
                if (!separator) continue;
                *separator = '\0';
                char *key = g_ascii_strdown(g_strstrip(lines[index]), -1);
                char *value = g_strstrip(separator + 1);
                if (g_str_equal(key, "grpc-status")) {
                    grpc_code = g_ascii_strtoll(value, NULL, 10);
                    has_grpc_code = TRUE;
                } else if (g_str_equal(key, "grpc-message")) {
                    g_free(grpc_message);
                    grpc_message = g_uri_unescape_string(value, NULL);
                }
                g_free(key);
            }
            g_strfreev(lines);
            g_free(text);
            if (has_grpc_code && grpc_code != 0) {
                g_set_error(error, G_IO_ERROR,
                            grpc_code == 16 || grpc_code == 7 ? G_IO_ERROR_PERMISSION_DENIED : G_IO_ERROR_FAILED,
                            "Grok web billing RPC failed with status %" G_GINT64_FORMAT ": %s",
                            grpc_code, grpc_message ? grpc_message : "");
                g_free(grpc_message);
                return FALSE;
            }
            g_free(grpc_message);
        }
        offset += frame_length;
    }
    return TRUE;
}

CodexBarProvider *codexbar_grok_parse_web_billing(const guint8 *data,
                                                  size_t length,
                                                  gint64 now_ms,
                                                  GError **error) {
    if (!data || !length || length > RESPONSE_LIMIT || !grpc_status(data, length, error)) {
        if (!error || !*error) {
            g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                                "Grok web billing returned an invalid gRPC-web response");
        }
        return NULL;
    }
    ProtoScan scan = {0};
    guint64 path[5] = {0};
    size_t offset = 0;
    gboolean framed = TRUE;
    gboolean scanned = FALSE;
    while (offset < length) {
        if (length - offset < 5) {
            framed = FALSE;
            break;
        }
        guint8 flags = data[offset];
        guint32 frame_length = (guint32)data[offset + 1] << 24 | (guint32)data[offset + 2] << 16 |
                               (guint32)data[offset + 3] << 8 | data[offset + 4];
        offset += 5;
        if (frame_length > length - offset) {
            framed = FALSE;
            break;
        }
        if (!(flags & 0x80)) {
            scan_protobuf(data + offset, frame_length, 0, path, &scan, now_ms);
            scanned = TRUE;
        }
        offset += frame_length;
    }
    if (!framed) {
        guint wire = data[0] & 7;
        if ((data[0] >> 3) && (wire == 0 || wire == 1 || wire == 2 || wire == 5)) {
            scan = (ProtoScan){0};
            scan_protobuf(data, length, 0, path, &scan, now_ms);
            scanned = TRUE;
        }
    }
    gint64 reset_ms = scan.has_preferred_reset ? scan.preferred_reset_ms : scan.fallback_reset_ms;
    gboolean has_reset = scan.has_preferred_reset || scan.has_fallback_reset;
    if (!scanned || (!scan.has_percent && !(has_reset && scan.has_usage_period))) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                            "Could not parse Grok web billing usage");
        return NULL;
    }
    CodexBarProvider *provider = new_provider("web", now_ms);
    CodexBarQuotaWindow *window = codexbar_quota_window_new("grok.credits", "Credits");
    window->usage_known = TRUE;
    window->used_percent = scan.has_percent ? scan.percent : 0;
    window->has_resets_at = has_reset;
    window->resets_at_ms = reset_ms;
    codexbar_provider_add_quota_window(provider, window);
    return provider;
}

static void credentials_free(GrokCredentials *credentials) {
    if (!credentials) return;
    g_free(credentials->token);
    g_free(credentials->email);
    g_free(credentials->team_id);
    g_free(credentials->login_method);
    g_free(credentials->principal_type);
    g_free(credentials);
}

static gboolean safe_header(const char *value) {
    if (!value || !value[0] || strlen(value) > CREDENTIAL_LIMIT || !g_utf8_validate(value, -1, NULL)) return FALSE;
    for (const unsigned char *cursor = (const unsigned char *)value; *cursor; cursor++) {
        if (g_ascii_iscntrl(*cursor)) return FALSE;
    }
    return TRUE;
}

static char *config_string(const CodexBarProviderConfig *config, const char *key) {
    return string_member(config ? config->raw : NULL, key);
}

static char *auth_path(const CodexBarProviderConfig *config) {
    char *configured = config_string(config, "authPath");
    if (configured) return configured;
    const char *home = g_getenv("GROK_HOME");
    if (home && home[0]) return g_build_filename(home, "auth.json", NULL);
    return g_build_filename(g_get_home_dir(), ".grok", "auth.json", NULL);
}

static GrokCredentials *credentials_from_entry(json_object *entry) {
    GrokCredentials *credentials = g_new0(GrokCredentials, 1);
    credentials->token = string_member(entry, "key");
    credentials->email = string_member(entry, "email");
    credentials->team_id = string_member(entry, "team_id");
    credentials->principal_type = string_member(entry, "principal_type");
    char *auth_mode = string_member(entry, "auth_mode");
    if (auth_mode && g_ascii_strcasecmp(auth_mode, "oidc") == 0) {
        credentials->login_method = g_strdup("SuperGrok");
    } else {
        credentials->login_method = g_strdup(auth_mode);
    }
    g_free(auth_mode);
    char *expiry = string_member(entry, "expires_at");
    credentials->has_expiry = timestamp_text(expiry, &credentials->expiry_ms);
    g_free(expiry);
    return credentials;
}

static GrokCredentials *load_credentials(const CodexBarProviderConfig *config) {
    char *direct = config_string(config, "accessToken");
    if (!direct && config && config->api_key) direct = g_strdup(config->api_key);
    if (!direct) direct = g_strdup(g_getenv("GROK_ACCESS_TOKEN"));
    if (direct) {
        GrokCredentials *credentials = g_new0(GrokCredentials, 1);
        credentials->token = direct;
        return credentials;
    }
    char *path = auth_path(config);
    char *contents = NULL;
    gsize length = 0;
    gboolean loaded = g_file_get_contents(path, &contents, &length, NULL);
    g_free(path);
    if (!loaded) return NULL;
    json_object *root = parse_json(contents, length);
    g_free(contents);
    if (!root || !json_object_is_type(root, json_type_object)) {
        if (root) json_object_put(root);
        return NULL;
    }
    json_object *oidc = NULL;
    json_object *legacy = NULL;
    json_object_object_foreach(root, scope, entry) {
        if (!json_object_is_type(entry, json_type_object)) continue;
        char *token = string_member(entry, "key");
        gboolean usable = token != NULL;
        g_free(token);
        if (!usable) continue;
        if (g_str_has_prefix(scope, "https://auth.x.ai::")) oidc = entry;
        else if (g_str_equal(scope, "https://accounts.x.ai/sign-in") || strstr(scope, "/sign-in")) legacy = entry;
    }
    GrokCredentials *credentials = oidc ? credentials_from_entry(oidc)
                                       : legacy ? credentials_from_entry(legacy) : NULL;
    json_object_put(root);
    return credentials;
}

static void apply_identity(CodexBarProvider *provider, const GrokCredentials *credentials) {
    if (!provider || !credentials) return;
    provider->account = g_strdup(credentials->email);
    provider->identity = g_new0(CodexBarProviderIdentity, 1);
    provider->identity->organization = g_strdup(credentials->team_id);
    provider->identity->login_method = g_strdup(credentials->login_method);
}

static char *default_runner(const char *binary,
                            const char *input,
                            GCancellable *cancellable,
                            GError **error) {
    const char *argv[] = {"/usr/bin/env", binary, "agent", "stdio", NULL};
    CodexBarProcessRequest request = {
        .arguments = argv,
        .standard_input = input,
        .standard_input_length = strlen(input),
        .timeout_milliseconds = 10000,
        .termination_grace_milliseconds = 400,
        .maximum_output_bytes = RESPONSE_LIMIT,
        .new_session = TRUE,
    };
    CodexBarProcessResult *result = codexbar_process_run(&request, cancellable, error);
    if (!result) return NULL;
    if (!codexbar_process_result_succeeded(result)) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                    "Grok CLI exited with status %d: %s", result->exit_status,
                    result->standard_error_length ? result->standard_error : "no diagnostic output");
        codexbar_process_result_free(result);
        return NULL;
    }
    if (memchr(result->standard_output, '\0', result->standard_output_length) ||
        !g_utf8_validate(result->standard_output, (gssize)result->standard_output_length, NULL)) {
        codexbar_process_result_free(result);
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                            "Grok CLI returned invalid UTF-8 output");
        return NULL;
    }
    char *output = g_strndup(result->standard_output, result->standard_output_length);
    codexbar_process_result_free(result);
    return output;
}

static CodexBarProvider *parse_rpc_output(const char *output,
                                          gint64 now_ms,
                                          gboolean *method_missing,
                                          GError **error) {
    if (!output || strlen(output) > RESPONSE_LIMIT || !g_utf8_validate(output, -1, NULL)) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                            "Grok CLI returned invalid JSON-RPC output");
        return NULL;
    }
    char **lines = g_strsplit(output, "\n", -1);
    CodexBarProvider *provider = NULL;
    for (size_t index = 0; lines[index] && !provider; index++) {
        if (!lines[index][0]) continue;
        json_object *message = parse_json(lines[index], strlen(lines[index]));
        gint64 id = 0;
        if (!message || !integer_value(member(message, "id"), &id) || id != 2) {
            if (message) json_object_put(message);
            continue;
        }
        json_object *rpc_error = member(message, "error");
        if (rpc_error) {
            char *text = string_member(rpc_error, "message");
            char *lower = text ? g_ascii_strdown(text, -1) : NULL;
            *method_missing = lower && g_str_has_prefix(g_strstrip(lower), "method not found");
            g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                        "Grok request failed: %s", text ? text : "unknown JSON-RPC error");
            g_free(lower);
            g_free(text);
        } else {
            json_object *result = member(message, "result");
            if (!result) {
                g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                                    "Grok JSON-RPC response is missing its result");
            } else {
                const char *json = json_object_to_json_string_ext(result, JSON_C_TO_STRING_PLAIN);
                provider = codexbar_grok_parse_billing(json, strlen(json), now_ms, error);
            }
        }
        json_object_put(message);
    }
    g_strfreev(lines);
    if (!provider && (!error || !*error)) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                            "Grok CLI returned no billing response");
    }
    return provider;
}

static CodexBarProvider *fetch_cli(const CodexBarProviderConfig *config,
                                   CodexBarGrokRunner runner,
                                   GCancellable *cancellable,
                                   gint64 now_ms,
                                   gboolean *method_missing,
                                   GError **error) {
    char *binary = config_string(config, "binaryPath");
    if (!binary) binary = g_strdup("grok");
    const char *input =
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{"
        "\"protocolVersion\":\"1\",\"clientCapabilities\":{\"fs\":{\"readTextFile\":false,"
        "\"writeTextFile\":false},\"terminal\":false}}}\n"
        "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"x.ai/billing\",\"params\":{}}\n";
    char *output = runner ? runner(binary, input, cancellable, error) : NULL;
    g_free(binary);
    if (!runner && (!error || !*error)) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "Grok CLI runner is missing");
    }
    if (!output) return NULL;
    CodexBarProvider *provider = parse_rpc_output(output, now_ms, method_missing, error);
    g_free(output);
    return provider;
}

static gboolean same_origin(const CodexBarHttpResponse *response, const char *url) {
    if (!response || !response->effective_url) return TRUE;
    GUri *expected = g_uri_parse(url, G_URI_FLAGS_NONE, NULL);
    GUri *actual = g_uri_parse(response->effective_url, G_URI_FLAGS_NONE, NULL);
    const char *expected_scheme = expected ? g_uri_get_scheme(expected) : NULL;
    const char *actual_scheme = actual ? g_uri_get_scheme(actual) : NULL;
    const char *expected_host = expected ? g_uri_get_host(expected) : NULL;
    const char *actual_host = actual ? g_uri_get_host(actual) : NULL;
    gboolean same = expected_scheme && actual_scheme && expected_host && actual_host &&
                    g_ascii_strcasecmp(actual_scheme, "https") == 0 &&
                    g_ascii_strcasecmp(expected_scheme, actual_scheme) == 0 &&
                    g_ascii_strcasecmp(expected_host, actual_host) == 0 &&
                    g_uri_get_port(expected) == g_uri_get_port(actual);
    if (expected) g_uri_unref(expected);
    if (actual) g_uri_unref(actual);
    return same;
}

static gboolean team_billing_unavailable(const GError *error) {
    if (!error) return FALSE;
    char *lower = g_ascii_strdown(error->message, -1);
    gboolean unavailable = strstr(lower, "status 9") && strstr(lower, "no personal team");
    g_free(lower);
    return unavailable;
}

static CodexBarProvider *fetch_web(const CodexBarProviderConfig *config,
                                   CodexBarGrokTransport transport,
                                   GCancellable *cancellable,
                                   gint64 now_ms,
                                   GrokCredentials *credentials,
                                   GError **error) {
    char *cookie = config_string(config, "cookieHeader");
    if (!cookie) cookie = g_strdup(g_getenv("GROK_COOKIE"));
    gboolean credentials_fresh = credentials && credentials->token &&
                                 (!credentials->has_expiry || credentials->expiry_ms > now_ms);
    if (cookie && !safe_header(cookie)) {
        g_free(cookie);
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                            "Grok cookie header is invalid");
        return NULL;
    }
    if (!cookie && !credentials_fresh) {
        g_free(cookie);
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                            "Grok web billing requires a cookie session or fresh grok auth token");
        return NULL;
    }
    if (credentials_fresh && !safe_header(credentials->token)) {
        g_free(cookie);
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Grok access token is invalid");
        return NULL;
    }
    if (!transport) {
        g_free(cookie);
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "Grok web transport is missing");
        return NULL;
    }
    const char *url = "https://grok.com/grok_api_v2.GrokBuildBilling/GetGrokCreditsConfig";
    char *authorization = credentials_fresh ? g_strdup_printf("Bearer %s", credentials->token) : NULL;
    CodexBarHttpRequestHeader headers[8] = {
        {"Origin", "https://grok.com"}, {"Referer", "https://grok.com/?_s=usage"},
        {"Accept", "*/*"}, {"Content-Type", "application/grpc-web+proto"},
        {"x-grpc-web", "1"}, {"x-user-agent", "connect-es/2.1.1"},
        {"User-Agent", "CodexBar"}, {NULL, NULL},
    };
    size_t count = 7;
    if (authorization) headers[count++] = (CodexBarHttpRequestHeader){"Authorization", authorization};
    CodexBarHttpRequestHeader *expanded = g_new(CodexBarHttpRequestHeader, count + (cookie ? 1 : 0));
    memcpy(expanded, headers, count * sizeof(*expanded));
    if (cookie) expanded[count++] = (CodexBarHttpRequestHeader){"Cookie", cookie};
    const guint8 body[5] = {0};
    CodexBarHttpRequest request = {
        .url = url, .method = "POST", .headers = expanded, .header_count = count,
        .body = body, .body_length = sizeof(body), .timeout_seconds = 15,
        .maximum_response_bytes = RESPONSE_LIMIT, .protocol_policy = CODEXBAR_HTTP_HTTPS_ONLY,
        .redirect_policy = CODEXBAR_HTTP_REDIRECT_SAME_ORIGIN, .cancellable = cancellable,
    };
    CodexBarHttpResponse *response = transport(&request, error);
    g_free(expanded);
    g_free(authorization);
    g_free(cookie);
    if (cancellable && g_cancellable_is_cancelled(cancellable)) {
        codexbar_http_response_free(response);
        if (error && *error) g_clear_error(error);
        g_cancellable_set_error_if_cancelled(cancellable, error);
        return NULL;
    }
    if (!response) return NULL;
    if (!same_origin(response, url)) {
        codexbar_http_response_free(response);
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                            "Grok redirected outside its trusted origin");
        return NULL;
    }
    if (response->status != 200) {
        long status = response->status;
        codexbar_http_response_free(response);
        g_set_error(error, G_IO_ERROR,
                    status == 401 || status == 403 ? G_IO_ERROR_PERMISSION_DENIED : G_IO_ERROR_FAILED,
                    "Grok web billing returned HTTP %ld", status);
        return NULL;
    }
    const char *grpc_header = codexbar_http_response_header_first(response, "grpc-status");
    if (grpc_header && !g_str_equal(grpc_header, "0")) {
        gint64 status = g_ascii_strtoll(grpc_header, NULL, 10);
        const char *raw_message = codexbar_http_response_header_first(response, "grpc-message");
        char *message = raw_message ? g_uri_unescape_string(raw_message, NULL) : NULL;
        codexbar_http_response_free(response);
        g_set_error(error, G_IO_ERROR,
                    status == 16 || status == 7 ? G_IO_ERROR_PERMISSION_DENIED : G_IO_ERROR_FAILED,
                    "Grok web billing RPC failed with status %" G_GINT64_FORMAT ": %s",
                    status, message ? message : "");
        g_free(message);
        return NULL;
    }
    CodexBarProvider *provider = codexbar_grok_parse_web_billing(
        (const guint8 *)response->body, response->body_length, now_ms, error);
    codexbar_http_response_free(response);
    return provider;
}

CodexBarProvider *codexbar_grok_fetch_with_adapters(const CodexBarProviderConfig *config,
                                                    const char *source,
                                                    CodexBarGrokTransport transport,
                                                    CodexBarGrokRunner runner,
                                                    GCancellable *cancellable,
                                                    gint64 now_ms,
                                                    GError **error) {
    const char *selected = source ? source : "auto";
    GrokCredentials *credentials = load_credentials(config);
    CodexBarProvider *provider = NULL;
    gboolean method_missing = FALSE;
    if (!g_str_equal(selected, "web")) {
        provider = fetch_cli(config, runner, cancellable, now_ms, &method_missing, error);
        if (provider) {
            apply_identity(provider, credentials);
            credentials_free(credentials);
            return provider;
        }
        if (method_missing && credentials && credentials->principal_type &&
            g_ascii_strcasecmp(g_strstrip(credentials->principal_type), "team") == 0 &&
            (!credentials->has_expiry || credentials->expiry_ms > now_ms)) {
            if (error && *error) g_clear_error(error);
            provider = new_provider("cli", now_ms);
            provider->note = g_strdup(
                "Grok team usage is unavailable from the current billing surface; identity is still available.");
            apply_identity(provider, credentials);
            credentials_free(credentials);
            return provider;
        }
        if (g_str_equal(selected, "cli")) {
            credentials_free(credentials);
            return NULL;
        }
        if (error && *error) g_clear_error(error);
    }
    provider = fetch_web(config, transport, cancellable, now_ms, credentials, error);
    if (!provider && credentials && credentials->principal_type &&
        g_ascii_strcasecmp(g_strstrip(credentials->principal_type), "team") == 0 &&
        error && team_billing_unavailable(*error)) {
        g_clear_error(error);
        provider = new_provider("web", now_ms);
        provider->note = g_strdup(
            "Grok team usage is unavailable from the current billing surface; identity is still available.");
    }
    if (provider) apply_identity(provider, credentials);
    credentials_free(credentials);
    return provider;
}

CodexBarProvider *codexbar_grok_fetch_with_cancellable(const CodexBarProviderConfig *config,
                                                       const char *source,
                                                       GCancellable *cancellable,
                                                       GError **error) {
    return codexbar_grok_fetch_with_adapters(
        config, source, codexbar_http_send, default_runner, cancellable, g_get_real_time() / 1000, error);
}
