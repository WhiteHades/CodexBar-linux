#include "azure_openai.h"

#include <gio/gio.h>
#include <json-c/json.h>
#include <string.h>

#define AZURE_OPENAI_DEFAULT_API_VERSION "2024-10-21"
#define AZURE_OPENAI_MAXIMUM_RESPONSE_BYTES (1024U * 1024U)

static void strip_unicode_whitespace(char *value) {
    char *start = value;
    while (*start && g_unichar_isspace(g_utf8_get_char(start))) start = g_utf8_next_char(start);
    char *end = value + strlen(value);
    while (end > start) {
        char *previous = g_utf8_find_prev_char(start, end);
        if (!previous || !g_unichar_isspace(g_utf8_get_char(previous))) break;
        end = previous;
    }
    size_t length = (size_t)(end - start);
    memmove(value, start, length);
    value[length] = '\0';
}

static char *clean_setting(const char *raw, gboolean reject_controls) {
    if (!raw || !g_utf8_validate(raw, -1, NULL)) return NULL;
    char *clean = g_strdup(raw);
    strip_unicode_whitespace(clean);
    size_t length = strlen(clean);
    if (length >= 1 && ((clean[0] == '\'' && clean[length - 1] == '\'') ||
                        (clean[0] == '"' && clean[length - 1] == '"'))) {
        clean[length - 1] = '\0';
        memmove(clean, clean + 1, length - 1);
        strip_unicode_whitespace(clean);
    }
    if (reject_controls) {
        for (const char *cursor = clean; *cursor; cursor = g_utf8_next_char(cursor)) {
            if (g_unichar_iscntrl(g_utf8_get_char(cursor))) {
                g_free(clean);
                return NULL;
            }
        }
    }
    if (clean[0] != '\0') return clean;
    g_free(clean);
    return NULL;
}

static char *resolve_setting(const char *configured, const char *environment, gboolean reject_controls) {
    char *value = clean_setting(configured, reject_controls);
    if (!value) value = clean_setting(g_getenv(environment), reject_controls);
    return value;
}

static char *resolve_api_key(const CodexBarProviderConfig *config) {
    return resolve_setting(config ? config->api_key : NULL, "AZURE_OPENAI_API_KEY", TRUE);
}

gboolean codexbar_azure_openai_has_api_key(const CodexBarProviderConfig *config) {
    char *key = resolve_api_key(config);
    gboolean present = key != NULL;
    g_free(key);
    return present;
}

static gboolean valid_endpoint_host(const char *endpoint, const char *host) {
    if (!host || host[0] == '\0' || !g_utf8_validate(host, -1, NULL)) return FALSE;
    gboolean bracketed_ipv6 = FALSE;
    const char *authority = strstr(endpoint, "://");
    if (authority) bracketed_ipv6 = authority[3] == '[';
    for (const char *cursor = host; *cursor; cursor = g_utf8_next_char(cursor)) {
        gunichar character = g_utf8_get_char(cursor);
        if (character == '%' || character == '\\' || g_unichar_isspace(character) ||
            g_unichar_iscntrl(character)) {
            return FALSE;
        }
        if (bracketed_ipv6) {
            if (!(g_unichar_isxdigit(character) || character == ':' || character == '.')) return FALSE;
        } else if (character == '/' || character == '?' || character == '#' || character == '@' || character == ':') {
            return FALSE;
        }
    }
    return TRUE;
}

static void set_endpoint_error(GError **error) {
    g_set_error_literal(error,
                        G_IO_ERROR,
                        G_IO_ERROR_INVALID_ARGUMENT,
                        "Azure OpenAI endpoint override AZURE_OPENAI_ENDPOINT is not allowed. "
                        "Use an HTTPS endpoint without user info or encoded host tricks.");
}

char *codexbar_azure_openai_endpoint_for_testing(const char *raw, GError **error) {
    char *clean = clean_setting(raw, FALSE);
    if (!clean) {
        set_endpoint_error(error);
        return NULL;
    }
    for (const char *cursor = clean; *cursor; cursor = g_utf8_next_char(cursor)) {
        if (g_unichar_iscntrl(g_utf8_get_char(cursor)) || *cursor == '\\') {
            g_free(clean);
            set_endpoint_error(error);
            return NULL;
        }
    }
    GError *normalize_error = NULL;
    char *endpoint = codexbar_http_normalize_endpoint(clean, CODEXBAR_HTTP_HTTPS_ONLY, &normalize_error);
    g_free(clean);
    if (!endpoint) {
        g_clear_error(&normalize_error);
        set_endpoint_error(error);
        return NULL;
    }
    GUri *uri = g_uri_parse(endpoint, G_URI_FLAGS_NONE, NULL);
    if (!uri || !valid_endpoint_host(endpoint, g_uri_get_host(uri))) {
        if (uri) g_uri_unref(uri);
        g_free(endpoint);
        set_endpoint_error(error);
        return NULL;
    }
    g_uri_unref(uri);
    return endpoint;
}

static GPtrArray *path_components(const char *path) {
    GPtrArray *components = g_ptr_array_new_with_free_func(g_free);
    char **parts = g_strsplit(path ? path : "", "/", -1);
    for (guint index = 0; parts[index]; index++) {
        if (parts[index][0] != '\0') g_ptr_array_add(components, g_strdup(parts[index]));
    }
    g_strfreev(parts);
    return components;
}

static gboolean component_equal(const char *encoded, const char *expected) {
    char *decoded = g_uri_unescape_string(encoded, NULL);
    gboolean equal = decoded && g_ascii_strcasecmp(decoded, expected) == 0;
    g_free(decoded);
    return equal;
}

static guint shared_component_count(GPtrArray *existing, const char *const *expected, guint expected_count) {
    guint maximum = MIN(existing->len, expected_count);
    for (guint count = maximum;; count--) {
        gboolean matches = TRUE;
        for (guint index = 0; index < count; index++) {
            const char *actual = g_ptr_array_index(existing, existing->len - count + index);
            if (!component_equal(actual, expected[index])) {
                matches = FALSE;
                break;
            }
        }
        if (matches) return count;
        if (count == 0) break;
    }
    return 0;
}

static void append_encoded_component(GString *path, const char *component) {
    g_string_append_c(path, '/');
    g_string_append(path, component);
}

static void append_escaped_component(GString *path, const char *component) {
    char *escaped = g_uri_escape_string(component, NULL, TRUE);
    append_encoded_component(path, escaped);
    g_free(escaped);
}

static gboolean uses_v1_api(const char *api_version) {
    char *clean = clean_setting(api_version, FALSE);
    gboolean result = clean && g_ascii_strcasecmp(clean, "v1") == 0;
    g_free(clean);
    return result;
}

char *codexbar_azure_openai_chat_url_for_testing(const char *endpoint,
                                                  const char *deployment,
                                                  const char *api_version,
                                                  GError **error) {
    GUri *uri = g_uri_parse(endpoint, G_URI_FLAGS_ENCODED, NULL);
    if (!uri) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "Azure OpenAI validation URL is invalid.");
        return NULL;
    }
    char *clean_deployment = clean_setting(deployment, FALSE);
    char *clean_version = clean_setting(api_version, FALSE);
    if (!clean_deployment || !clean_version) {
        g_uri_unref(uri);
        g_free(clean_deployment);
        g_free(clean_version);
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "Azure OpenAI validation URL is invalid.");
        return NULL;
    }
    if (g_str_equal(clean_deployment, ".") || g_str_equal(clean_deployment, "..")) {
        g_uri_unref(uri);
        g_free(clean_deployment);
        g_free(clean_version);
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "Azure OpenAI validation URL is invalid.");
        return NULL;
    }
    gboolean v1 = uses_v1_api(clean_version);
    const char *v1_root[] = {"openai", "v1"};
    const char *legacy_root[] = {"openai"};
    const char *const *expected = v1 ? v1_root : legacy_root;
    guint expected_count = v1 ? G_N_ELEMENTS(v1_root) : G_N_ELEMENTS(legacy_root);
    GPtrArray *existing = path_components(g_uri_get_path(uri));
    guint shared = shared_component_count(existing, expected, expected_count);
    GString *path = g_string_new(NULL);
    for (guint index = 0; index < existing->len; index++) {
        append_encoded_component(path, g_ptr_array_index(existing, index));
    }
    for (guint index = shared; index < expected_count; index++) append_escaped_component(path, expected[index]);
    if (v1) {
        append_escaped_component(path, "chat");
        append_escaped_component(path, "completions");
    } else {
        append_escaped_component(path, "deployments");
        append_escaped_component(path, clean_deployment);
        append_escaped_component(path, "chat");
        append_escaped_component(path, "completions");
    }
    if (path->len == 0) g_string_append_c(path, '/');
    char *query_value = v1 ? NULL : g_uri_escape_string(clean_version, NULL, TRUE);
    char *query = query_value ? g_strdup_printf("api-version=%s", query_value) : NULL;
    GUri *built = g_uri_build(G_URI_FLAGS_ENCODED,
                              g_uri_get_scheme(uri),
                              NULL,
                              g_uri_get_host(uri),
                              g_uri_get_port(uri),
                              path->str,
                              v1 ? g_uri_get_query(uri) : query,
                              g_uri_get_fragment(uri));
    char *result = built ? g_uri_to_string(built) : NULL;
    if (!result) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "Azure OpenAI validation URL is invalid.");
    }
    if (built) g_uri_unref(built);
    g_uri_unref(uri);
    g_ptr_array_free(existing, TRUE);
    g_string_free(path, TRUE);
    g_free(query_value);
    g_free(query);
    g_free(clean_deployment);
    g_free(clean_version);
    return result;
}

static json_object *parse_json_document(const char *json, size_t length) {
    if (!json || length > G_MAXINT || !g_utf8_validate(json, (gssize)length, NULL)) return NULL;
    json_tokener *tokener = json_tokener_new();
    json_tokener_set_flags(tokener, JSON_TOKENER_STRICT | JSON_TOKENER_VALIDATE_UTF8);
    json_object *root = json_tokener_parse_ex(tokener, json, (int)length);
    enum json_tokener_error parse_error = json_tokener_get_error(tokener);
    size_t consumed = json_tokener_get_parse_end(tokener);
    while (consumed < length && strchr(" \t\n\r", json[consumed])) consumed++;
    gboolean valid = parse_error == json_tokener_success && root && consumed == length &&
                     json_object_is_type(root, json_type_object);
    json_tokener_free(tokener);
    if (valid) return root;
    if (root) json_object_put(root);
    return NULL;
}

static char *response_model(const char *body, size_t length, GError **error) {
    json_object *root = parse_json_document(body, length);
    if (!root) {
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_INVALID_DATA,
                            "Azure OpenAI response parse error: invalid JSON response");
        return NULL;
    }
    json_object *value = NULL;
    char *model = NULL;
    if (json_object_object_get_ex(root, "model", &value) && !json_object_is_type(value, json_type_null)) {
        if (!json_object_is_type(value, json_type_string)) {
            json_object_put(root);
            g_set_error_literal(error,
                                G_IO_ERROR,
                                G_IO_ERROR_INVALID_DATA,
                                "Azure OpenAI response parse error: model must be a string");
            return NULL;
        }
        const char *raw = json_object_get_string(value);
        size_t raw_length = (size_t)json_object_get_string_len(value);
        if (memchr(raw, '\0', raw_length)) {
            json_object_put(root);
            g_set_error_literal(error,
                                G_IO_ERROR,
                                G_IO_ERROR_INVALID_DATA,
                                "Azure OpenAI response parse error: model contains invalid text");
            return NULL;
        }
        model = g_strdup(raw);
    }
    json_object_put(root);
    return model;
}

static char *response_summary(const char *body, size_t length) {
    if (!body || !g_utf8_validate(body, (gssize)length, NULL)) return g_strdup("");
    GString *summary = g_string_new(NULL);
    gboolean pending_space = FALSE;
    const char *end = body + length;
    for (const char *cursor = body; cursor < end && *cursor; cursor = g_utf8_next_char(cursor)) {
        gunichar character = g_utf8_get_char(cursor);
        if (g_unichar_isspace(character)) {
            pending_space = summary->len > 0;
            continue;
        }
        if (g_unichar_iscntrl(character)) continue;
        if (pending_space) g_string_append_c(summary, ' ');
        pending_space = FALSE;
        g_string_append_unichar(summary, character);
    }
    if (g_utf8_strlen(summary->str, -1) <= 240) return g_string_free(summary, FALSE);
    char *truncated = g_utf8_substring(summary->str, 0, 240);
    char *result = g_strconcat(truncated, "\xE2\x80\xA6 [truncated]", NULL);
    g_free(truncated);
    g_string_free(summary, TRUE);
    return result;
}

static char *display_text(const char *raw) {
    char *clean = clean_setting(raw, TRUE);
    if (!clean || g_utf8_strlen(clean, -1) <= 240) return clean;
    char *truncated = g_utf8_substring(clean, 0, 240);
    char *result = g_strconcat(truncated, "\xE2\x80\xA6", NULL);
    g_free(truncated);
    g_free(clean);
    return result;
}

static char *validation_body(const char *deployment, gboolean v1) {
    json_object *payload = json_object_new_object();
    json_object *messages = json_object_new_array();
    json_object *message = json_object_new_object();
    json_object_object_add(message, "role", json_object_new_string("user"));
    json_object_object_add(message, "content", json_object_new_string("ping"));
    json_object_array_add(messages, message);
    json_object_object_add(payload, "messages", messages);
    if (v1) {
        json_object_object_add(payload, "model", json_object_new_string(deployment));
        json_object_object_add(payload, "max_completion_tokens", json_object_new_int(1));
    } else {
        json_object_object_add(payload, "max_tokens", json_object_new_int(1));
    }
    char *body = g_strdup(json_object_to_json_string_ext(payload, JSON_C_TO_STRING_PLAIN));
    json_object_put(payload);
    return body;
}

static CodexBarProvider *provider_from_response(const char *endpoint,
                                                const char *deployment,
                                                char *model,
                                                gint64 now_ms) {
    CodexBarProvider *provider = codexbar_provider_new();
    provider->provider = g_strdup("azureopenai");
    provider->source = g_strdup("deployment");
    provider->explicit_quota_slots = TRUE;
    provider->has_updated_at = TRUE;
    provider->updated_at_ms = now_ms;
    provider->identity = g_new0(CodexBarProviderIdentity, 1);
    GUri *uri = g_uri_parse(endpoint, G_URI_FLAGS_NONE, NULL);
    provider->identity->organization = g_strdup(uri ? g_uri_get_host(uri) : endpoint);
    if (uri) g_uri_unref(uri);
    char *display_deployment = display_text(deployment);
    provider->identity->login_method = g_strdup_printf("Deployment: %s", display_deployment);
    CodexBarQuotaWindow *window = codexbar_quota_window_new("primary", "Azure OpenAI");
    window->usage_known = TRUE;
    window->used_percent = 0.0;
    char *display_model = display_text(model);
    window->detail = display_model
                         ? g_strdup_printf("Deployment: %s · Model: %s", display_deployment, display_model)
                         : g_strdup_printf("Deployment: %s", display_deployment);
    codexbar_provider_add_quota_window(provider, window);
    provider->usage_extensions = json_object_new_object();
    json_object_object_add(provider->usage_extensions, "dataConfidence", json_object_new_string("unknown"));
    g_free(display_deployment);
    g_free(display_model);
    g_free(model);
    return provider;
}

static void wrap_network_error(GError **error) {
    if (error && *error && (*error)->domain == G_IO_ERROR && (*error)->code == G_IO_ERROR_CANCELLED) return;
    char *message = error && *error ? g_strdup((*error)->message) : g_strdup("request failed");
    g_clear_error(error);
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Azure OpenAI network error: %s", message);
    g_free(message);
}

CodexBarProvider *codexbar_azure_openai_fetch_with_transport_and_cancellable(
    const CodexBarProviderConfig *config,
    CodexBarAzureOpenAITransport transport,
    GCancellable *cancellable,
    gint64 now_ms,
    GError **error) {
    g_return_val_if_fail(transport != NULL, NULL);
    char *api_key = resolve_api_key(config);
    if (!api_key) {
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_NOT_FOUND,
                            "Azure OpenAI API key not configured. Set AZURE_OPENAI_API_KEY or configure an API key in Settings.");
        return NULL;
    }
    char *raw_endpoint = resolve_setting(
        config ? config->enterprise_host : NULL, "AZURE_OPENAI_ENDPOINT", FALSE);
    if (!raw_endpoint) {
        g_free(api_key);
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_NOT_FOUND,
                            "Azure OpenAI endpoint not configured. Set AZURE_OPENAI_ENDPOINT or configure an endpoint in Settings.");
        return NULL;
    }
    char *endpoint = codexbar_azure_openai_endpoint_for_testing(raw_endpoint, error);
    g_free(raw_endpoint);
    if (!endpoint) {
        g_free(api_key);
        return NULL;
    }
    char *deployment = resolve_setting(
        config ? config->workspace_id : NULL, "AZURE_OPENAI_DEPLOYMENT_NAME", TRUE);
    if (!deployment) {
        g_free(api_key);
        g_free(endpoint);
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_NOT_FOUND,
                            "Azure OpenAI deployment not configured. Set AZURE_OPENAI_DEPLOYMENT_NAME or configure a deployment in Settings.");
        return NULL;
    }
    char *api_version = clean_setting(g_getenv("AZURE_OPENAI_API_VERSION"), FALSE);
    if (!api_version) api_version = g_strdup(AZURE_OPENAI_DEFAULT_API_VERSION);
    char *url = codexbar_azure_openai_chat_url_for_testing(endpoint, deployment, api_version, error);
    if (!url) {
        g_free(api_key);
        g_free(endpoint);
        g_free(deployment);
        g_free(api_version);
        return NULL;
    }
    gboolean v1 = uses_v1_api(api_version);
    char *body = validation_body(deployment, v1);
    const CodexBarHttpRequestHeader headers[] = {
        {"api-key", api_key},
        {"Accept", "application/json"},
        {"Content-Type", "application/json"},
    };
    const CodexBarHttpRequest request = {
        .url = url,
        .method = "POST",
        .headers = headers,
        .header_count = G_N_ELEMENTS(headers),
        .body = body,
        .body_length = strlen(body),
        .timeout_seconds = 20,
        .maximum_response_bytes = AZURE_OPENAI_MAXIMUM_RESPONSE_BYTES,
        .protocol_policy = CODEXBAR_HTTP_HTTPS_ONLY,
        .redirect_policy = CODEXBAR_HTTP_REDIRECT_SAME_ORIGIN,
        .cancellable = cancellable,
    };
    if (cancellable && g_cancellable_set_error_if_cancelled(cancellable, error)) {
        g_free(api_key);
        g_free(url);
        g_free(body);
        g_free(endpoint);
        g_free(deployment);
        g_free(api_version);
        return NULL;
    }
    CodexBarHttpResponse *response = transport(&request, error);
    g_free(api_key);
    g_free(url);
    g_free(body);
    if (!response) {
        wrap_network_error(error);
        g_free(endpoint);
        g_free(deployment);
        g_free(api_version);
        return NULL;
    }
    if (response->status < 200 || response->status >= 300) {
        char *summary = response_summary(response->body, response->body_length);
        long status = response->status;
        codexbar_http_response_free(response);
        if (summary[0] == '\0') {
            g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Azure OpenAI API error: HTTP %ld", status);
        } else {
            g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Azure OpenAI API error: HTTP %ld: %s", status, summary);
        }
        g_free(summary);
        g_free(endpoint);
        g_free(deployment);
        g_free(api_version);
        return NULL;
    }
    GError *parse_error = NULL;
    char *model = response_model(response->body, response->body_length, &parse_error);
    codexbar_http_response_free(response);
    if (parse_error) {
        g_propagate_error(error, parse_error);
        g_free(endpoint);
        g_free(deployment);
        g_free(api_version);
        return NULL;
    }
    CodexBarProvider *provider = provider_from_response(endpoint, deployment, model, now_ms);
    g_free(endpoint);
    g_free(deployment);
    g_free(api_version);
    return provider;
}

CodexBarProvider *codexbar_azure_openai_fetch_with_transport(const CodexBarProviderConfig *config,
                                                              CodexBarAzureOpenAITransport transport,
                                                              gint64 now_ms,
                                                              GError **error) {
    return codexbar_azure_openai_fetch_with_transport_and_cancellable(config, transport, NULL, now_ms, error);
}

CodexBarProvider *codexbar_azure_openai_fetch_with_cancellable(const CodexBarProviderConfig *config,
                                                                GCancellable *cancellable,
                                                                GError **error) {
    return codexbar_azure_openai_fetch_with_transport_and_cancellable(
        config, codexbar_http_send, cancellable, g_get_real_time() / 1000, error);
}

CodexBarProvider *codexbar_azure_openai_fetch(const CodexBarProviderConfig *config, GError **error) {
    return codexbar_azure_openai_fetch_with_cancellable(config, NULL, error);
}
