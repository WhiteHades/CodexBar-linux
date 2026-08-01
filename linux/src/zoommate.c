#include "zoommate.h"

#include <gio/gio.h>
#include <json-c/json.h>
#include <math.h>
#include <string.h>

#define ZOOMMATE_AI_HOST "ai.zoom.us"
#define ZOOMMATE_WEB_HOST "zoommate.zoom.us"
#define ZOOMMATE_STATUS_PATH "/ai-computer/api/v1/credits/status"
#define ZOOMMATE_MAXIMUM_RESPONSE_BYTES (1024U * 1024U)

static const char *const zoommate_hosts[] = {ZOOMMATE_AI_HOST, ZOOMMATE_WEB_HOST};
static const char zoommate_origin[] = "https://zoommate.zoom.us";
static const char zoommate_user_agent[] =
    "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) "
    "Chrome/143.0.0.0 Safari/537.36";

typedef struct {
    char *authorization;
    char *cookie;
    char *accept;
    char *accept_language;
    char *user_agent;
    char *sec_fetch_dest;
    char *sec_fetch_mode;
    char *sec_fetch_site;
    guint preferred_host;
    gboolean captured;
} ZoomMateRequestContext;

typedef struct {
    gboolean has_budget_cap;
    double budget_cap;
    gboolean has_used_credit;
    double used_credit;
    gboolean has_remaining_credit;
    double remaining_credit;
    gboolean has_overage_credit;
    double overage_credit;
    gboolean has_allow_overage;
    gboolean allow_overage;
    gboolean has_cycle_start;
    gint64 cycle_start;
    gboolean has_cycle_end;
    gint64 cycle_end;
    gboolean has_quota_available;
    gboolean quota_available;
    gboolean has_unlimited;
    gboolean unlimited;
} ZoomMateCreditStatus;

static void request_context_clear(ZoomMateRequestContext *context) {
    if (!context) return;
    g_free(context->authorization);
    g_free(context->cookie);
    g_free(context->accept);
    g_free(context->accept_language);
    g_free(context->user_agent);
    g_free(context->sec_fetch_dest);
    g_free(context->sec_fetch_mode);
    g_free(context->sec_fetch_site);
    memset(context, 0, sizeof(*context));
}

static gboolean safe_header_value(const char *value) {
    if (!value || !g_utf8_validate(value, -1, NULL)) return FALSE;
    for (const unsigned char *cursor = (const unsigned char *)value; *cursor; cursor++) {
        if (*cursor < 32 || *cursor == 127) return FALSE;
    }
    return TRUE;
}

static char *clean_value(const char *raw) {
    if (!raw || !g_utf8_validate(raw, -1, NULL)) return NULL;
    char *value = g_strdup(raw);
    g_strstrip(value);
    size_t length = strlen(value);
    if (length >= 2 && ((value[0] == '\'' && value[length - 1] == '\'') ||
                        (value[0] == '"' && value[length - 1] == '"'))) {
        value[length - 1] = '\0';
        memmove(value, value + 1, length - 1);
        g_strstrip(value);
    }
    if (value[0] != '\0' && safe_header_value(value)) return value;
    g_free(value);
    return NULL;
}

static char *bearer_header(const char *raw) {
    char *value = clean_value(raw);
    if (!value) return NULL;
    const char *token = value;
    if (g_ascii_strncasecmp(value, "Bearer ", strlen("Bearer ")) == 0) token += strlen("Bearer ");
    while (*token == ' ' || *token == '\t') token++;
    if (*token == '\0') {
        g_free(value);
        return NULL;
    }
    for (const unsigned char *cursor = (const unsigned char *)token; *cursor; cursor++) {
        if (g_ascii_isspace(*cursor) || *cursor < 33 || *cursor == 127) {
            g_free(value);
            return NULL;
        }
    }
    char *authorization = g_strdup_printf("Bearer %s", token);
    g_free(value);
    return authorization;
}

static gboolean exact_capture_url(const char *raw, guint *host_index) {
    if (!raw || !g_utf8_validate(raw, -1, NULL) || strchr(raw, '%')) return FALSE;
    GError *parse_error = NULL;
    GUri *uri = g_uri_parse(raw, G_URI_FLAGS_ENCODED, &parse_error);
    g_clear_error(&parse_error);
    if (!uri) return FALSE;
    const char *scheme = g_uri_get_scheme(uri);
    const char *host = g_uri_get_host(uri);
    const char *path = g_uri_get_path(uri);
    gboolean valid = scheme && g_ascii_strcasecmp(scheme, "https") == 0 && host && path &&
                     strcmp(path, ZOOMMATE_STATUS_PATH) == 0 && g_uri_get_port(uri) == -1 &&
                     g_uri_get_userinfo(uri) == NULL && g_uri_get_query(uri) == NULL &&
                     g_uri_get_fragment(uri) == NULL;
    guint matched = G_N_ELEMENTS(zoommate_hosts);
    for (guint index = 0; valid && index < G_N_ELEMENTS(zoommate_hosts); index++) {
        if (g_ascii_strcasecmp(host, zoommate_hosts[index]) == 0) {
            matched = index;
            break;
        }
    }
    valid = valid && matched < G_N_ELEMENTS(zoommate_hosts);
    if (valid) *host_index = matched;
    g_uri_unref(uri);
    return valid;
}

static void replace_header_value(char **destination, const char *raw) {
    char *value = clean_value(raw);
    if (!value) return;
    g_free(*destination);
    *destination = value;
}

static gboolean capture_header(ZoomMateRequestContext *context, const char *raw) {
    const char *colon = raw ? strchr(raw, ':') : NULL;
    if (!colon) return TRUE;
    char *name = g_strndup(raw, (gsize)(colon - raw));
    g_strstrip(name);
    const char *value = colon + 1;
    while (*value == ' ' || *value == '\t') value++;
    gboolean valid = safe_header_value(value);
    if (!valid) {
        g_free(name);
        return FALSE;
    }
    if (g_ascii_strcasecmp(name, "Authorization") == 0) {
        char *authorization = bearer_header(value);
        if (!authorization) valid = FALSE;
        if (valid) {
            g_free(context->authorization);
            context->authorization = authorization;
        }
    } else if (g_ascii_strcasecmp(name, "Cookie") == 0) {
        replace_header_value(&context->cookie, value);
    } else if (g_ascii_strcasecmp(name, "Accept") == 0) {
        replace_header_value(&context->accept, value);
    } else if (g_ascii_strcasecmp(name, "Accept-Language") == 0) {
        replace_header_value(&context->accept_language, value);
    } else if (g_ascii_strcasecmp(name, "User-Agent") == 0) {
        replace_header_value(&context->user_agent, value);
    } else if (g_ascii_strcasecmp(name, "Sec-Fetch-Dest") == 0) {
        replace_header_value(&context->sec_fetch_dest, value);
    } else if (g_ascii_strcasecmp(name, "Sec-Fetch-Mode") == 0) {
        replace_header_value(&context->sec_fetch_mode, value);
    } else if (g_ascii_strcasecmp(name, "Sec-Fetch-Site") == 0) {
        replace_header_value(&context->sec_fetch_site, value);
    }
    g_free(name);
    return valid;
}

static gboolean option_with_value(const char *argument) {
    static const char *const options[] = {
        "-A", "--user-agent", "-e", "--referer", "-o", "--output", "--connect-timeout",
        "--max-time", "--retry", "--retry-delay", "--proxy", "-x", "--cacert", "--cert", "--key",
    };
    for (guint index = 0; index < G_N_ELEMENTS(options); index++) {
        if (strcmp(argument, options[index]) == 0) return TRUE;
    }
    return FALSE;
}

static gboolean parse_capture(const char *capture, ZoomMateRequestContext *context) {
    char *clean = capture ? g_strdup(capture) : NULL;
    if (!clean || !g_utf8_validate(clean, -1, NULL)) {
        g_free(clean);
        return FALSE;
    }
    g_strstrip(clean);
    int argc = 0;
    char **argv = NULL;
    GError *shell_error = NULL;
    gboolean parsed = clean[0] != '\0' && g_shell_parse_argv(clean, &argc, &argv, &shell_error);
    g_clear_error(&shell_error);
    g_free(clean);
    if (!parsed || argc < 2) {
        g_strfreev(argv);
        return FALSE;
    }
    const char *command = strrchr(argv[0], '/');
    command = command ? command + 1 : argv[0];
    if (strcmp(command, "curl") != 0) {
        g_strfreev(argv);
        return FALSE;
    }

    char *url = NULL;
    gboolean valid = TRUE;
    for (int index = 1; valid && index < argc; index++) {
        const char *argument = argv[index];
        if (strcmp(argument, "-H") == 0 || strcmp(argument, "--header") == 0) {
            valid = ++index < argc && capture_header(context, argv[index]);
        } else if (g_str_has_prefix(argument, "--header=")) {
            valid = capture_header(context, argument + strlen("--header="));
        } else if (strcmp(argument, "-b") == 0 || strcmp(argument, "--cookie") == 0) {
            valid = ++index < argc;
            if (valid) replace_header_value(&context->cookie, argv[index]);
        } else if (g_str_has_prefix(argument, "--cookie=")) {
            replace_header_value(&context->cookie, argument + strlen("--cookie="));
        } else if (strcmp(argument, "--url") == 0) {
            valid = ++index < argc && url == NULL;
            if (valid) url = g_strdup(argv[index]);
        } else if (g_str_has_prefix(argument, "--url=")) {
            valid = url == NULL;
            if (valid) url = g_strdup(argument + strlen("--url="));
        } else if (strcmp(argument, "-X") == 0 || strcmp(argument, "--request") == 0) {
            valid = ++index < argc && g_ascii_strcasecmp(argv[index], "GET") == 0;
        } else if (g_str_has_prefix(argument, "--request=")) {
            valid = g_ascii_strcasecmp(argument + strlen("--request="), "GET") == 0;
        } else if (option_with_value(argument)) {
            valid = ++index < argc;
        } else if (argument[0] != '-') {
            valid = url == NULL;
            if (valid) url = g_strdup(argument);
        }
    }

    guint host_index = 0;
    valid = valid && context->authorization && exact_capture_url(url, &host_index);
    if (valid) {
        context->captured = TRUE;
        context->preferred_host = host_index;
    }
    g_free(url);
    g_strfreev(argv);
    return valid;
}

gboolean codexbar_zoommate_capture_is_valid_for_testing(const char *capture) {
    ZoomMateRequestContext context = {0};
    gboolean valid = parse_capture(capture, &context);
    request_context_clear(&context);
    return valid;
}

static const char *config_string(const CodexBarProviderConfig *config, const char *key) {
    json_object *value = NULL;
    if (!config || !config->raw || !json_object_is_type(config->raw, json_type_object) ||
        !json_object_object_get_ex(config->raw, key, &value) || !json_object_is_type(value, json_type_string)) {
        return NULL;
    }
    return json_object_get_string(value);
}

static gboolean resolve_context(const CodexBarProviderConfig *config,
                                ZoomMateRequestContext *context,
                                GError **error) {
    const char *source = config_string(config, "cookieSource");
    if (source && g_str_equal(source, "off")) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "ZoomMate cookies are disabled.");
        return FALSE;
    }
    const char *raw = source && g_str_equal(source, "manual") ? config_string(config, "cookieHeader") : NULL;
    if ((!raw || raw[0] == '\0') && (!source || !g_str_equal(source, "manual"))) {
        raw = g_getenv("ZOOMMATE_CAPTURE");
    }
    if (raw && raw[0] != '\0') {
        char *clean = g_strdup(raw);
        g_strstrip(clean);
        gboolean capture = g_str_has_prefix(clean, "curl ") || g_str_has_prefix(clean, "/usr/bin/curl ") ||
                           g_str_has_prefix(clean, "/bin/curl ");
        gboolean valid = capture ? parse_capture(clean, context) : (context->authorization = bearer_header(clean)) != NULL;
        g_free(clean);
        if (!valid) {
            request_context_clear(context);
            g_set_error_literal(error,
                                G_IO_ERROR,
                                G_IO_ERROR_INVALID_ARGUMENT,
                                "ZoomMate credential is invalid. Use a bearer token or an exact HTTPS credits/status cURL capture.");
            return FALSE;
        }
        return TRUE;
    }
    if (source && g_str_equal(source, "manual")) {
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_NOT_FOUND,
                            "Missing ZoomMate manual cURL capture in cookieHeader.");
        return FALSE;
    }
    raw = g_getenv("ZOOMMATE_TOKEN");
    if (!raw || raw[0] == '\0') raw = g_getenv("ZOOMMATE_BEARER_TOKEN");
    context->authorization = bearer_header(raw);
    if (context->authorization) return TRUE;
    g_set_error_literal(error,
                        G_IO_ERROR,
                        G_IO_ERROR_NOT_FOUND,
                        "Missing ZoomMate credential. Set ZOOMMATE_TOKEN or provide a credits/status cURL capture.");
    return FALSE;
}

static json_object *parse_json_document(const char *json, size_t length) {
    if (!json || length > G_MAXINT || !g_utf8_validate(json, (gssize)length, NULL)) return NULL;
    json_tokener *tokener = json_tokener_new();
    json_tokener_set_flags(tokener, JSON_TOKENER_STRICT | JSON_TOKENER_VALIDATE_UTF8);
    json_object *root = json_tokener_parse_ex(tokener, json, (int)length);
    enum json_tokener_error parse_error = json_tokener_get_error(tokener);
    size_t consumed = json_tokener_get_parse_end(tokener);
    while (consumed < length && g_ascii_isspace((guchar)json[consumed])) consumed++;
    gboolean valid = parse_error == json_tokener_success && root && consumed == length;
    json_tokener_free(tokener);
    if (valid) return root;
    if (root) json_object_put(root);
    return NULL;
}

static gboolean optional_number(
    json_object *object, const char *name, gboolean *present, double *value) {
    json_object *member = NULL;
    if (!json_object_object_get_ex(object, name, &member) || json_object_is_type(member, json_type_null)) {
        *present = FALSE;
        return TRUE;
    }
    if (!json_object_is_type(member, json_type_int) && !json_object_is_type(member, json_type_double)) return FALSE;
    *value = json_object_get_double(member);
    *present = isfinite(*value);
    return *present;
}

static gboolean optional_int64(
    json_object *object, const char *name, gboolean *present, gint64 *value) {
    json_object *member = NULL;
    if (!json_object_object_get_ex(object, name, &member) || json_object_is_type(member, json_type_null)) {
        *present = FALSE;
        return TRUE;
    }
    if (!json_object_is_type(member, json_type_int)) return FALSE;
    *value = json_object_get_int64(member);
    *present = TRUE;
    return TRUE;
}

static gboolean optional_bool(
    json_object *object, const char *name, gboolean *present, gboolean *value) {
    json_object *member = NULL;
    if (!json_object_object_get_ex(object, name, &member) || json_object_is_type(member, json_type_null)) {
        *present = FALSE;
        return TRUE;
    }
    if (!json_object_is_type(member, json_type_boolean)) return FALSE;
    *value = json_object_get_boolean(member);
    *present = TRUE;
    return TRUE;
}

static gboolean parse_credit_status(json_object *root, ZoomMateCreditStatus *status) {
    json_object *data = NULL;
    json_object *credit_status = NULL;
    if (!root || !json_object_is_type(root, json_type_object) ||
        !json_object_object_get_ex(root, "data", &data) || !json_object_is_type(data, json_type_object) ||
        !json_object_object_get_ex(data, "credit_status", &credit_status) ||
        !json_object_is_type(credit_status, json_type_object)) {
        return FALSE;
    }
    return optional_number(credit_status, "budget_cap", &status->has_budget_cap, &status->budget_cap) &&
           optional_number(credit_status, "used_credit", &status->has_used_credit, &status->used_credit) &&
           optional_number(
               credit_status, "remaining_credit", &status->has_remaining_credit, &status->remaining_credit) &&
           optional_number(credit_status, "overage_credit", &status->has_overage_credit, &status->overage_credit) &&
           optional_bool(credit_status, "allow_overage", &status->has_allow_overage, &status->allow_overage) &&
           optional_int64(credit_status, "cycle_start_date", &status->has_cycle_start, &status->cycle_start) &&
           optional_int64(credit_status, "cycle_end_date", &status->has_cycle_end, &status->cycle_end) &&
           optional_bool(credit_status,
                         "is_quota_available",
                         &status->has_quota_available,
                         &status->quota_available) &&
           optional_bool(credit_status, "is_unlimited", &status->has_unlimited, &status->unlimited);
}

CodexBarProvider *codexbar_zoommate_parse_usage_bytes(
    const char *json, size_t length, gint64 now_ms, GError **error) {
    json_object *root = parse_json_document(json, length);
    ZoomMateCreditStatus status = {0};
    if (!parse_credit_status(root, &status)) {
        if (root) json_object_put(root);
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Could not parse ZoomMate usage response");
        return NULL;
    }

    double budget = status.has_budget_cap ? status.budget_cap : 0.0;
    double used = status.has_used_credit ? status.used_credit : 0.0;
    gboolean unlimited = status.has_unlimited && status.unlimited;
    double used_percent = !unlimited && budget > 0.0 ? CLAMP(used / budget * 100.0, 0.0, 100.0) : 0.0;

    CodexBarProvider *provider = codexbar_provider_new();
    provider->provider = g_strdup("zoommate");
    provider->source = g_strdup("web");
    provider->has_updated_at = TRUE;
    provider->updated_at_ms = now_ms;
    provider->explicit_quota_slots = TRUE;
    CodexBarQuotaWindow *window = codexbar_quota_window_new("primary", "Credits");
    window->usage_known = TRUE;
    window->used_percent = used_percent;
    window->reset_description = g_strdup("Credits");
    if (!unlimited && budget > 0.0 && status.has_cycle_end && status.cycle_end > 0) {
        window->has_resets_at = TRUE;
        window->resets_at_ms = status.cycle_end;
    }
    codexbar_provider_add_quota_window(provider, window);
    provider->usage_extensions = json_object_new_object();
    json_object_object_add(provider->usage_extensions, "dataConfidence", json_object_new_string("exact"));
    if (status.has_budget_cap)
        json_object_object_add(provider->usage_extensions, "budgetCap", json_object_new_double(status.budget_cap));
    if (status.has_used_credit)
        json_object_object_add(provider->usage_extensions, "usedCredit", json_object_new_double(status.used_credit));
    if (status.has_remaining_credit)
        json_object_object_add(
            provider->usage_extensions, "remainingCredit", json_object_new_double(status.remaining_credit));
    if (status.has_overage_credit)
        json_object_object_add(
            provider->usage_extensions, "overageCredit", json_object_new_double(status.overage_credit));
    if (status.has_allow_overage)
        json_object_object_add(
            provider->usage_extensions, "allowOverage", json_object_new_boolean(status.allow_overage));
    if (status.has_quota_available)
        json_object_object_add(
            provider->usage_extensions, "quotaAvailable", json_object_new_boolean(status.quota_available));
    if (status.has_unlimited)
        json_object_object_add(provider->usage_extensions, "unlimited", json_object_new_boolean(status.unlimited));
    json_object_put(root);
    return provider;
}

CodexBarProvider *codexbar_zoommate_parse_usage(const char *json, gint64 now_ms, GError **error) {
    return codexbar_zoommate_parse_usage_bytes(json, json ? strlen(json) : 0, now_ms, error);
}

static const char *context_value(const char *captured, const char *fallback) {
    return captured ? captured : fallback;
}

static CodexBarHttpResponse *send_request(const ZoomMateRequestContext *context,
                                         guint host_index,
                                         CodexBarZoomMateTransport transport,
                                         GCancellable *cancellable,
                                         GError **error) {
    char *url = g_strdup_printf("https://%s%s", zoommate_hosts[host_index], ZOOMMATE_STATUS_PATH);
    CodexBarHttpRequestHeader headers[10] = {
        {"Authorization", context->authorization},
        {"Accept", context_value(context->accept, "application/json, text/plain, */*")},
        {"Accept-Language", context_value(context->accept_language, "en-US,en;q=0.9")},
        {"User-Agent", context_value(context->user_agent, zoommate_user_agent)},
        {"Sec-Fetch-Dest", context_value(context->sec_fetch_dest, "empty")},
        {"Sec-Fetch-Mode", context_value(context->sec_fetch_mode, "cors")},
        {"Sec-Fetch-Site", context_value(context->sec_fetch_site, "same-site")},
        {"Origin", zoommate_origin},
        {"Referer", zoommate_origin},
    };
    size_t header_count = 9;
    if (context->captured && context->cookie && host_index == context->preferred_host) {
        headers[header_count++] = (CodexBarHttpRequestHeader){"Cookie", context->cookie};
    }
    const CodexBarHttpRequest request = {
        .url = url,
        .method = "GET",
        .headers = headers,
        .header_count = header_count,
        .timeout_seconds = 15,
        .maximum_response_bytes = ZOOMMATE_MAXIMUM_RESPONSE_BYTES,
        .protocol_policy = CODEXBAR_HTTP_HTTPS_ONLY,
        .redirect_policy = CODEXBAR_HTTP_REDIRECT_DENY,
        .cancellable = cancellable,
    };
    CodexBarHttpResponse *response = transport(&request, error);
    g_free(url);
    return response;
}

static gboolean cancelled(GCancellable *cancellable, GError **error) {
    return cancellable && g_cancellable_set_error_if_cancelled(cancellable, error);
}

CodexBarProvider *codexbar_zoommate_fetch_with_transport_and_cancellable(const CodexBarProviderConfig *config,
                                                                         CodexBarZoomMateTransport transport,
                                                                         GCancellable *cancellable,
                                                                         gint64 now_ms,
                                                                         GError **error) {
    g_return_val_if_fail(transport != NULL, NULL);
    ZoomMateRequestContext context = {0};
    if (!resolve_context(config, &context, error)) return NULL;
    guint order[] = {context.preferred_host, context.preferred_host == 0 ? 1 : 0};
    for (guint attempt = 0; attempt < G_N_ELEMENTS(order); attempt++) {
        if (cancelled(cancellable, error)) {
            request_context_clear(&context);
            return NULL;
        }
        GError *request_error = NULL;
        CodexBarHttpResponse *response =
            send_request(&context, order[attempt], transport, cancellable, &request_error);
        if (cancellable && g_cancellable_is_cancelled(cancellable)) {
            codexbar_http_response_free(response);
            g_clear_error(&request_error);
            cancelled(cancellable, error);
            request_context_clear(&context);
            return NULL;
        }
        if (!response) {
            if (attempt == 0) {
                g_clear_error(&request_error);
                continue;
            }
            if (request_error) {
                char *message = g_strdup(request_error->message);
                g_clear_error(&request_error);
                g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "ZoomMate network error: %s", message);
                g_free(message);
            } else {
                g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, "ZoomMate network request failed");
            }
            request_context_clear(&context);
            return NULL;
        }
        g_clear_error(&request_error);
        if (response->status == 401 || response->status == 403) {
            codexbar_http_response_free(response);
            request_context_clear(&context);
            g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED, "ZoomMate credentials were rejected.");
            return NULL;
        }
        if (response->status == 200) {
            CodexBarProvider *provider =
                codexbar_zoommate_parse_usage_bytes(response->body, response->body_length, now_ms, error);
            codexbar_http_response_free(response);
            request_context_clear(&context);
            return provider;
        }
        long status = response->status;
        codexbar_http_response_free(response);
        if (attempt + 1 == G_N_ELEMENTS(order)) {
            request_context_clear(&context);
            g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "ZoomMate API error: HTTP %ld", status);
            return NULL;
        }
    }
    g_assert_not_reached();
}

CodexBarProvider *codexbar_zoommate_fetch_with_transport(const CodexBarProviderConfig *config,
                                                          CodexBarZoomMateTransport transport,
                                                          gint64 now_ms,
                                                          GError **error) {
    return codexbar_zoommate_fetch_with_transport_and_cancellable(config, transport, NULL, now_ms, error);
}

CodexBarProvider *codexbar_zoommate_fetch_with_cancellable(const CodexBarProviderConfig *config,
                                                            GCancellable *cancellable,
                                                            GError **error) {
    return codexbar_zoommate_fetch_with_transport_and_cancellable(
        config, codexbar_http_send, cancellable, g_get_real_time() / 1000, error);
}

CodexBarProvider *codexbar_zoommate_fetch(const CodexBarProviderConfig *config, GError **error) {
    return codexbar_zoommate_fetch_with_cancellable(config, NULL, error);
}
