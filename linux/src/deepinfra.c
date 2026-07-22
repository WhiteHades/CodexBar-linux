#include "deepinfra.h"

#include <curl/curl.h>
#include <errno.h>
#include <gio/gio.h>
#include <json-c/json.h>
#include <math.h>
#include <string.h>

#define DEEPINFRA_CHECKLIST_URL "https://api.deepinfra.com/payment/checklist?compute_owed=true"
#define DEEPINFRA_USAGE_URL "https://api.deepinfra.com/payment/usage?from=current"
#define DEEPINFRA_MAXIMUM_RESPONSE_BYTES (1024U * 1024U)

typedef struct {
    double stripe_balance;
    double recent;
    gboolean has_limit;
    double limit;
    gboolean suspended;
    char *suspend_reason;
} DeepInfraChecklist;

static char *clean_api_key(const char *raw) {
    if (!raw) return NULL;
    char *clean = g_strdup(raw);
    g_strstrip(clean);
    size_t length = strlen(clean);
    if (length >= 1 && ((clean[0] == '\'' && clean[length - 1] == '\'') ||
                        (clean[0] == '"' && clean[length - 1] == '"'))) {
        clean[length - 1] = '\0';
        memmove(clean, clean + 1, length - 1);
        g_strstrip(clean);
    }
    for (const unsigned char *cursor = (const unsigned char *)clean; *cursor; cursor++) {
        if (*cursor < 32 || *cursor == 127) {
            g_free(clean);
            return NULL;
        }
    }
    if (clean[0] != '\0') return clean;
    g_free(clean);
    return NULL;
}

static char *resolve_api_key(const CodexBarProviderConfig *config) {
    char *key = clean_api_key(config ? config->api_key : NULL);
    if (!key) key = clean_api_key(g_getenv("DEEPINFRA_API_KEY"));
    if (!key) key = clean_api_key(g_getenv("DEEPINFRA_TOKEN"));
    return key;
}

static json_object *parse_json_document(const char *json, size_t length) {
    if (!json || length > G_MAXINT || !g_utf8_validate(json, (gssize)length, NULL)) return NULL;
    json_tokener *tokener = json_tokener_new();
    json_tokener_set_flags(tokener, JSON_TOKENER_STRICT | JSON_TOKENER_VALIDATE_UTF8);
    json_object *root = json_tokener_parse_ex(tokener, json, (int)length);
    enum json_tokener_error parse_error = json_tokener_get_error(tokener);
    size_t consumed = json_tokener_get_parse_end(tokener);
    while (consumed < length &&
           (json[consumed] == ' ' || json[consumed] == '\t' || json[consumed] == '\n' || json[consumed] == '\r')) {
        consumed++;
    }
    gboolean valid = parse_error == json_tokener_success && root && consumed == length;
    json_tokener_free(tokener);
    if (valid) return root;
    if (root) json_object_put(root);
    return NULL;
}

static gboolean json_number(json_object *object, const char *key, gboolean optional, gboolean *present, double *value) {
    json_object *member = NULL;
    if (!json_object_object_get_ex(object, key, &member) || json_object_is_type(member, json_type_null)) {
        *present = FALSE;
        return optional;
    }
    if (!json_object_is_type(member, json_type_int) && !json_object_is_type(member, json_type_double)) return FALSE;
    *value = json_object_get_double(member);
    if (!isfinite(*value)) return FALSE;
    *present = TRUE;
    return TRUE;
}

static gboolean parse_checklist(json_object *root, DeepInfraChecklist *checklist) {
    if (!root || !json_object_is_type(root, json_type_object)) return FALSE;
    gboolean present = FALSE;
    if (!json_number(root, "stripe_balance", FALSE, &present, &checklist->stripe_balance) || !present) return FALSE;
    if (!json_number(root, "recent", FALSE, &present, &checklist->recent) || !present) return FALSE;
    if (!json_number(root, "limit", TRUE, &checklist->has_limit, &checklist->limit)) return FALSE;

    json_object *suspended = NULL;
    if (json_object_object_get_ex(root, "suspended", &suspended) &&
        !json_object_is_type(suspended, json_type_null)) {
        if (!json_object_is_type(suspended, json_type_boolean)) return FALSE;
        checklist->suspended = json_object_get_boolean(suspended);
    }
    json_object *reason = NULL;
    if (json_object_object_get_ex(root, "suspend_reason", &reason) && !json_object_is_type(reason, json_type_null)) {
        if (!json_object_is_type(reason, json_type_string) ||
            memchr(json_object_get_string(reason), '\0', (size_t)json_object_get_string_len(reason))) {
            return FALSE;
        }
        checklist->suspend_reason = g_strdup(json_object_get_string(reason));
    }
    return TRUE;
}

static gboolean parse_usage(json_object *root, gboolean *has_month, double *last_cost_cents) {
    if (!root || !json_object_is_type(root, json_type_object)) return FALSE;
    json_object *initial_month = NULL;
    if (json_object_object_get_ex(root, "initial_month", &initial_month) &&
        !json_object_is_type(initial_month, json_type_null) &&
        !json_object_is_type(initial_month, json_type_string)) {
        return FALSE;
    }
    json_object *months = NULL;
    if (!json_object_object_get_ex(root, "months", &months) || !json_object_is_type(months, json_type_array)) {
        return FALSE;
    }
    size_t count = json_object_array_length(months);
    for (size_t index = 0; index < count; index++) {
        json_object *month = json_object_array_get_idx(months, index);
        json_object *period = NULL;
        gboolean present = FALSE;
        double total_cost = 0.0;
        if (!month || !json_object_is_type(month, json_type_object) ||
            !json_object_object_get_ex(month, "period", &period) ||
            !json_object_is_type(period, json_type_string) ||
            !json_number(month, "total_cost", FALSE, &present, &total_cost) || !present) {
            return FALSE;
        }
        *has_month = TRUE;
        *last_cost_cents = total_cost;
    }
    return TRUE;
}

static void checklist_clear(DeepInfraChecklist *checklist) {
    g_free(checklist->suspend_reason);
}

CodexBarProvider *codexbar_deepinfra_parse_usage_bytes(const char *checklist_json,
                                                       size_t checklist_length,
                                                       const char *usage_json,
                                                       size_t usage_length,
                                                       gint64 now_ms,
                                                       GError **error) {
    json_object *checklist_root = parse_json_document(checklist_json, checklist_length);
    json_object *usage_root = parse_json_document(usage_json, usage_length);
    DeepInfraChecklist checklist = {0};
    gboolean has_month = FALSE;
    double last_cost_cents = 0.0;
    if (!parse_checklist(checklist_root, &checklist) || !parse_usage(usage_root, &has_month, &last_cost_cents)) {
        checklist_clear(&checklist);
        if (checklist_root) json_object_put(checklist_root);
        if (usage_root) json_object_put(usage_root);
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "DeepInfra billing response is malformed");
        return NULL;
    }

    double recent = checklist.recent > 0.0 ? checklist.recent : 0.0;
    double current_month = has_month ? (last_cost_cents > 0.0 ? last_cost_cents / 100.0 : 0.0) : recent;
    double net_balance = checklist.stripe_balance + recent;
    if (!isfinite(net_balance)) {
        checklist_clear(&checklist);
        json_object_put(checklist_root);
        json_object_put(usage_root);
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "DeepInfra billing response is malformed");
        return NULL;
    }
    double available = net_balance < 0.0 ? -net_balance : 0.0;
    double owed = net_balance > 0.0 ? net_balance : 0.0;
    gboolean exhausted = checklist.suspended || owed > 0.0 || available <= 0.0;
    char *balance_text = owed > 0.0 ? g_strdup_printf("$%.2f owed", owed)
                                   : g_strdup_printf("$%.2f available", available);
    char *spending_text = g_strdup_printf("$%.2f spent this month", current_month);
    char *reason = checklist.suspend_reason ? g_strstrip(g_strdup(checklist.suspend_reason)) : NULL;
    char *detail = NULL;
    if (checklist.suspended && reason && reason[0] != '\0') {
        detail = g_strdup_printf("Suspended: %s \xC2\xB7 %s \xC2\xB7 %s", reason, balance_text, spending_text);
    } else if (checklist.suspended) {
        detail = g_strdup_printf("Suspended \xC2\xB7 %s \xC2\xB7 %s", balance_text, spending_text);
    } else {
        detail = g_strdup_printf("%s \xC2\xB7 %s", balance_text, spending_text);
    }

    CodexBarProvider *provider = codexbar_provider_new();
    provider->provider = g_strdup("deepinfra");
    provider->source = g_strdup("api");
    provider->identity = g_new0(CodexBarProviderIdentity, 1);
    provider->has_updated_at = TRUE;
    provider->updated_at_ms = now_ms;
    provider->explicit_quota_slots = TRUE;
    CodexBarQuotaWindow *window = codexbar_quota_window_new("primary", "Balance");
    window->usage_known = TRUE;
    window->used_percent = exhausted ? 100.0 : 0.0;
    window->reset_description = detail;
    codexbar_provider_add_quota_window(provider, window);
    if (checklist.has_limit && checklist.limit > 0.0) {
        provider->provider_cost = g_new0(CodexBarProviderCost, 1);
        provider->provider_cost->used = recent;
        provider->provider_cost->limit = checklist.limit;
        provider->provider_cost->currency = g_strdup("USD");
        provider->provider_cost->period = g_strdup("Billing cycle");
        provider->provider_cost->has_updated_at = TRUE;
        provider->provider_cost->updated_at_ms = now_ms;
    }
    provider->usage_extensions = json_object_new_object();
    json_object_object_add(provider->usage_extensions, "dataConfidence", json_object_new_string("exact"));

    g_free(reason);
    g_free(balance_text);
    g_free(spending_text);
    checklist_clear(&checklist);
    json_object_put(checklist_root);
    json_object_put(usage_root);
    return provider;
}

CodexBarProvider *codexbar_deepinfra_parse_usage(
    const char *checklist_json, const char *usage_json, gint64 now_ms, GError **error) {
    return codexbar_deepinfra_parse_usage_bytes(checklist_json,
                                                checklist_json ? strlen(checklist_json) : 0,
                                                usage_json,
                                                usage_json ? strlen(usage_json) : 0,
                                                now_ms,
                                                error);
}

static gboolean retryable_status(long status) {
    const long statuses[] = {408, 429, 500, 502, 503, 504};
    for (guint index = 0; index < G_N_ELEMENTS(statuses); index++) {
        if (status == statuses[index]) return TRUE;
    }
    return FALSE;
}

static gboolean retryable_error(const GError *error) {
    if (!error) return FALSE;
    if (error->domain == G_IO_ERROR) {
        return error->code == G_IO_ERROR_TIMED_OUT || error->code == G_IO_ERROR_CONNECTION_CLOSED ||
               error->code == G_IO_ERROR_CONNECTION_REFUSED || error->code == G_IO_ERROR_HOST_NOT_FOUND;
    }
    if (error->domain != g_quark_from_static_string("codexbar-http-error")) return FALSE;
    return error->code == CURLE_OPERATION_TIMEDOUT || error->code == CURLE_RECV_ERROR ||
           error->code == CURLE_SEND_ERROR || error->code == CURLE_PARTIAL_FILE ||
           error->code == CURLE_COULDNT_CONNECT ||
           error->code == CURLE_COULDNT_RESOLVE_HOST;
}

double codexbar_deepinfra_retry_delay_for_testing(const CodexBarHttpResponse *response) {
    const char *header = codexbar_http_response_header_first(response, "Retry-After");
    if (!header) return 1.0;
    char *clean = g_strstrip(g_strdup(header));
    char *end = NULL;
    double seconds = g_ascii_strtod(clean, &end);
    gboolean valid = clean[0] != '\0' && end && *end == '\0' && isfinite(seconds) && seconds >= 0.0;
    g_free(clean);
    return valid ? MIN(seconds, 10.0) : 1.0;
}

static gboolean wait_for_retry(double delay, GCancellable *cancellable, GError **error) {
    if (cancellable && g_cancellable_set_error_if_cancelled(cancellable, error)) return FALSE;
    if (delay <= 0.0) return TRUE;
    if (!cancellable) {
        g_usleep((gulong)(delay * G_USEC_PER_SEC));
        return TRUE;
    }

    gint64 deadline = g_get_monotonic_time() + (gint64)(delay * G_USEC_PER_SEC);
    GPollFD poll_fd;
    if (g_cancellable_make_pollfd(cancellable, &poll_fd)) {
        while (!g_cancellable_is_cancelled(cancellable)) {
            gint64 remaining = deadline - g_get_monotonic_time();
            if (remaining <= 0) break;
            gint timeout_ms = (gint)MIN((remaining + 999) / 1000, G_MAXINT);
            int result = g_poll(&poll_fd, 1, timeout_ms);
            if (result >= 0) break;
            if (errno != EINTR) {
                int poll_error = errno;
                g_cancellable_release_fd(cancellable);
                g_set_error(error,
                            G_IO_ERROR,
                            g_io_error_from_errno(poll_error),
                            "DeepInfra retry wait failed: %s",
                            g_strerror(poll_error));
                return FALSE;
            }
        }
        g_cancellable_release_fd(cancellable);
    } else {
        while (!g_cancellable_is_cancelled(cancellable)) {
            gint64 remaining = deadline - g_get_monotonic_time();
            if (remaining <= 0) break;
            g_usleep((gulong)MIN(remaining, 10000));
        }
    }
    return !g_cancellable_set_error_if_cancelled(cancellable, error);
}

static CodexBarHttpResponse *send_with_retry(
    const CodexBarHttpRequest *request, CodexBarDeepInfraTransport transport, GError **error) {
    for (guint attempt = 0; attempt < 2; attempt++) {
        if (request->cancellable && g_cancellable_set_error_if_cancelled(request->cancellable, error)) return NULL;
        GError *request_error = NULL;
        CodexBarHttpResponse *response = transport(request, &request_error);
        if (request->cancellable && g_cancellable_is_cancelled(request->cancellable)) {
            codexbar_http_response_free(response);
            g_clear_error(&request_error);
            g_cancellable_set_error_if_cancelled(request->cancellable, error);
            return NULL;
        }
        gboolean retry = attempt == 0 && ((response && retryable_status(response->status)) ||
                                          (!response && retryable_error(request_error)));
        if (!retry) {
            if (response) {
                g_clear_error(&request_error);
                return response;
            }
            if (request_error) {
                g_propagate_error(error, request_error);
            } else {
                g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, "DeepInfra network request failed");
            }
            return NULL;
        }
        double delay = response ? codexbar_deepinfra_retry_delay_for_testing(response) : 1.0;
        codexbar_http_response_free(response);
        g_clear_error(&request_error);
        if (!wait_for_retry(delay, request->cancellable, error)) return NULL;
    }
    g_assert_not_reached();
}

static gboolean validate_response(CodexBarHttpResponse *response, GError **error) {
    long status = response->status;
    if (status == 200) return TRUE;
    if (status == 401) {
        g_set_error_literal(
            error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED, "DeepInfra API error: API key rejected (HTTP 401).");
    } else if (status == 403) {
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_PERMISSION_DENIED,
                            "DeepInfra API error: API key cannot access billing data (HTTP 403).");
    } else {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "DeepInfra API error: HTTP %ld", status);
    }
    return FALSE;
}

static void wrap_network_error(GError **error) {
    if (!error || !*error || ((*error)->domain == G_IO_ERROR && (*error)->code == G_IO_ERROR_CANCELLED)) return;
    char *message = g_strdup((*error)->message);
    g_clear_error(error);
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "DeepInfra network error: %s", message);
    g_free(message);
}

CodexBarProvider *codexbar_deepinfra_fetch_with_transport_and_cancellable(const CodexBarProviderConfig *config,
                                                                          CodexBarDeepInfraTransport transport,
                                                                          GCancellable *cancellable,
                                                                          gint64 now_ms,
                                                                          GError **error) {
    g_return_val_if_fail(transport != NULL, NULL);
    char *api_key = resolve_api_key(config);
    if (!api_key) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "Missing DeepInfra API key.");
        return NULL;
    }
    char *authorization = g_strdup_printf("Bearer %s", api_key);
    g_free(api_key);
    const CodexBarHttpRequestHeader headers[] = {
        {"Authorization", authorization},
        {"Accept", "application/json"},
    };
    CodexBarHttpRequest request = {
        .method = "GET",
        .headers = headers,
        .header_count = G_N_ELEMENTS(headers),
        .timeout_seconds = 30,
        .maximum_response_bytes = DEEPINFRA_MAXIMUM_RESPONSE_BYTES,
        .protocol_policy = CODEXBAR_HTTP_HTTPS_ONLY,
        .redirect_policy = CODEXBAR_HTTP_REDIRECT_SAME_ORIGIN,
        .cancellable = cancellable,
    };
    request.url = DEEPINFRA_CHECKLIST_URL;
    CodexBarHttpResponse *checklist = send_with_retry(&request, transport, error);
    if (!checklist) {
        wrap_network_error(error);
        g_free(authorization);
        return NULL;
    }
    if (!validate_response(checklist, error)) {
        codexbar_http_response_free(checklist);
        g_free(authorization);
        return NULL;
    }
    request.url = DEEPINFRA_USAGE_URL;
    CodexBarHttpResponse *usage = send_with_retry(&request, transport, error);
    g_free(authorization);
    if (!usage) {
        wrap_network_error(error);
        codexbar_http_response_free(checklist);
        return NULL;
    }
    if (!validate_response(usage, error)) {
        codexbar_http_response_free(checklist);
        codexbar_http_response_free(usage);
        return NULL;
    }
    CodexBarProvider *provider = codexbar_deepinfra_parse_usage_bytes(
        checklist->body, checklist->body_length, usage->body, usage->body_length, now_ms, error);
    codexbar_http_response_free(checklist);
    codexbar_http_response_free(usage);
    return provider;
}

CodexBarProvider *codexbar_deepinfra_fetch_with_transport(const CodexBarProviderConfig *config,
                                                           CodexBarDeepInfraTransport transport,
                                                           gint64 now_ms,
                                                           GError **error) {
    return codexbar_deepinfra_fetch_with_transport_and_cancellable(config, transport, NULL, now_ms, error);
}

CodexBarProvider *codexbar_deepinfra_fetch(const CodexBarProviderConfig *config, GError **error) {
    return codexbar_deepinfra_fetch_with_cancellable(config, NULL, error);
}

CodexBarProvider *codexbar_deepinfra_fetch_with_cancellable(const CodexBarProviderConfig *config,
                                                            GCancellable *cancellable,
                                                            GError **error) {
    return codexbar_deepinfra_fetch_with_transport_and_cancellable(
        config, codexbar_http_send, cancellable, g_get_real_time() / 1000, error);
}
