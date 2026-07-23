#include "wayfinder.h"

#include <gio/gio.h>
#include <json-c/json.h>
#include <math.h>
#include <string.h>

#define WAYFINDER_DEFAULT_URL "http://127.0.0.1:8088"
#define WAYFINDER_MAXIMUM_RESPONSE_BYTES (1024U * 1024U)

static char *clean_value(const char *raw) {
    if (!raw || !g_utf8_validate(raw, -1, NULL)) return NULL;
    char *value = g_strdup(raw);
    g_strstrip(value);
    size_t length = strlen(value);
    if (length >= 1 && ((value[0] == '\'' && value[length - 1] == '\'') ||
                        (value[0] == '"' && value[length - 1] == '"'))) {
        value[length - 1] = '\0';
        memmove(value, value + 1, length - 1);
        g_strstrip(value);
    }
    if (value[0] != '\0') return value;
    g_free(value);
    return NULL;
}

static gboolean exact_loopback(const char *host) {
    if (g_ascii_strcasecmp(host, "localhost") == 0) return TRUE;
    GInetAddress *address = g_inet_address_new_from_string(host);
    gboolean loopback = address && g_inet_address_get_is_loopback(address);
    g_clear_object(&address);
    return loopback;
}

static void endpoint_error(GError **error) {
    g_set_error_literal(error,
                        G_IO_ERROR,
                        G_IO_ERROR_INVALID_ARGUMENT,
                        "Wayfinder gateway URL override WAYFINDER_GATEWAY_URL is invalid. Use an HTTPS URL, or "
                        "plain HTTP for loopback addresses only, without embedded credentials.");
}

static char *validate_base_url(const char *raw, GError **error) {
    char *clean = clean_value(raw);
    if (!clean || !strstr(clean, "://")) {
        g_free(clean);
        endpoint_error(error);
        return NULL;
    }
    GError *normalize_error = NULL;
    char *url = codexbar_http_normalize_endpoint(clean, CODEXBAR_HTTP_ALLOW_LOOPBACK_HTTP, &normalize_error);
    g_free(clean);
    if (!url) {
        g_clear_error(&normalize_error);
        endpoint_error(error);
        return NULL;
    }
    GUri *uri = g_uri_parse(url, G_URI_FLAGS_NONE, NULL);
    const char *scheme = uri ? g_uri_get_scheme(uri) : NULL;
    const char *host = uri ? g_uri_get_host(uri) : NULL;
    gboolean https = scheme && g_ascii_strcasecmp(scheme, "https") == 0;
    gboolean valid = uri && host && host[0] != '\0' && (https || exact_loopback(host));
    for (const char *cursor = host; valid && *cursor; cursor = g_utf8_next_char(cursor)) {
        gunichar character = g_utf8_get_char(cursor);
        valid = character != '%' && character != '\\' && !g_unichar_isspace(character) &&
                !g_unichar_iscntrl(character);
    }
    if (uri) g_uri_unref(uri);
    if (valid) return url;
    g_free(url);
    endpoint_error(error);
    return NULL;
}

char *codexbar_wayfinder_base_url_for_testing(const CodexBarProviderConfig *config, GError **error) {
    char *raw = clean_value(config ? config->enterprise_host : NULL);
    if (!raw) raw = clean_value(g_getenv("WAYFINDER_GATEWAY_URL"));
    if (!raw) return g_strdup(WAYFINDER_DEFAULT_URL);
    char *url = validate_base_url(raw, error);
    g_free(raw);
    return url;
}

char *codexbar_wayfinder_dashboard_url_for_testing(const CodexBarProviderConfig *config, GError **error) {
    char *base = codexbar_wayfinder_base_url_for_testing(config, error);
    if (!base) return NULL;
    GUri *uri = g_uri_parse(base, G_URI_FLAGS_ENCODED, error);
    g_free(base);
    if (!uri) return NULL;
    const char *base_path = g_uri_get_path(uri);
    char *trimmed = g_strdup(base_path && base_path[0] ? base_path : "");
    size_t length = strlen(trimmed);
    if (length > 0 && trimmed[length - 1] == '/') trimmed[length - 1] = '\0';
    char *path = g_strdup_printf("%s/router", trimmed);
    GUri *dashboard = g_uri_build(G_URI_FLAGS_ENCODED,
                                  g_uri_get_scheme(uri),
                                  NULL,
                                  g_uri_get_host(uri),
                                  g_uri_get_port(uri),
                                  path,
                                  NULL,
                                  NULL);
    char *result = dashboard ? g_uri_to_string(dashboard) : NULL;
    if (!result && (!error || !*error)) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "Wayfinder dashboard URL is invalid");
    }
    if (dashboard) g_uri_unref(dashboard);
    g_uri_unref(uri);
    g_free(trimmed);
    g_free(path);
    return result;
}

char *codexbar_wayfinder_endpoint_for_testing(const char *base_url,
                                               const char *path,
                                               const char *query,
                                               GError **error) {
    GUri *uri = g_uri_parse(base_url, G_URI_FLAGS_ENCODED, NULL);
    if (!uri || !path || path[0] == '\0') {
        if (uri) g_uri_unref(uri);
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "Wayfinder endpoint URL is invalid");
        return NULL;
    }
    const char *base_path = g_uri_get_path(uri);
    char *trimmed = g_strdup(base_path && base_path[0] ? base_path : "");
    size_t length = strlen(trimmed);
    if (length > 0 && trimmed[length - 1] == '/') trimmed[length - 1] = '\0';
    char *full_path = g_strdup_printf("%s/%s", trimmed, path);
    GUri *built = g_uri_build(G_URI_FLAGS_ENCODED,
                              g_uri_get_scheme(uri),
                              NULL,
                              g_uri_get_host(uri),
                              g_uri_get_port(uri),
                              full_path,
                              query,
                              g_uri_get_fragment(uri));
    char *result = built ? g_uri_to_string(built) : NULL;
    if (!result) g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "Wayfinder endpoint URL is invalid");
    if (built) g_uri_unref(built);
    g_uri_unref(uri);
    g_free(trimmed);
    g_free(full_path);
    return result;
}

static json_object *parse_object(const char *text) {
    if (!text || !g_utf8_validate(text, -1, NULL)) return NULL;
    size_t length = strlen(text);
    if (length > G_MAXINT) return NULL;
    json_tokener *tokener = json_tokener_new();
    json_tokener_set_flags(tokener, JSON_TOKENER_STRICT | JSON_TOKENER_VALIDATE_UTF8);
    json_object *root = json_tokener_parse_ex(tokener, text, (int)length);
    gboolean valid = json_tokener_get_error(tokener) == json_tokener_success && root &&
                     json_object_is_type(root, json_type_object);
    json_tokener_free(tokener);
    if (valid) return root;
    if (root) json_object_put(root);
    return NULL;
}

static gboolean valid_display_text(const char *text) {
    if (!text || !g_utf8_validate(text, -1, NULL)) return FALSE;
    for (const char *cursor = text; *cursor; cursor = g_utf8_next_char(cursor)) {
        if (g_unichar_iscntrl(g_utf8_get_char(cursor))) return FALSE;
    }
    return TRUE;
}

static gboolean required_string(json_object *object, const char *key, const char **value) {
    json_object *member = NULL;
    if (!json_object_object_get_ex(object, key, &member) || !json_object_is_type(member, json_type_string)) return FALSE;
    *value = json_object_get_string(member);
    return valid_display_text(*value);
}

static gboolean required_bool(json_object *object, const char *key, gboolean *value) {
    json_object *member = NULL;
    if (!json_object_object_get_ex(object, key, &member) || !json_object_is_type(member, json_type_boolean)) return FALSE;
    *value = json_object_get_boolean(member);
    return TRUE;
}

static gboolean required_int(json_object *object, const char *key, gint64 *value) {
    json_object *member = NULL;
    if (!json_object_object_get_ex(object, key, &member) || !json_object_is_type(member, json_type_int)) return FALSE;
    *value = json_object_get_int64(member);
    return TRUE;
}

static gboolean required_nonnegative_int(json_object *object, const char *key, gint64 *value) {
    return required_int(object, key, value) && *value >= 0;
}

static gboolean required_double(json_object *object, const char *key, double *value) {
    json_object *member = NULL;
    if (!json_object_object_get_ex(object, key, &member) ||
        (!json_object_is_type(member, json_type_int) && !json_object_is_type(member, json_type_double))) return FALSE;
    *value = json_object_get_double(member);
    return isfinite(*value);
}

double codexbar_wayfinder_average_decision_ms_for_testing(const char *text, gboolean *present) {
    double aggregate_sum = 0;
    double aggregate_count = 0;
    double all_sum = 0;
    double all_count = 0;
    gboolean has_aggregate_sum = FALSE;
    gboolean has_aggregate_count = FALSE;
    gboolean has_all_sum = FALSE;
    gboolean has_all_count = FALSE;
    char **lines = g_strsplit(text ? text : "", "\n", -1);
    const char *sum_name = "wayfinder_router_decision_latency_seconds_sum";
    const char *count_name = "wayfinder_router_decision_latency_seconds_count";
    for (guint index = 0; lines[index]; index++) {
        const char *name = g_str_has_prefix(lines[index], sum_name) ? sum_name
                           : g_str_has_prefix(lines[index], count_name) ? count_name
                                                                       : NULL;
        if (!name || (lines[index][strlen(name)] != ' ' && lines[index][strlen(name)] != '{')) continue;
        char *line = g_strstrip(lines[index]);
        char *token = strrchr(line, ' ');
        if (!token) continue;
        char *number = g_strstrip(token + 1);
        char *end = NULL;
        double value = g_ascii_strtod(number, &end);
        if (!end || end == number || *end != '\0' || !isfinite(value)) continue;
        gboolean unlabeled = line[strlen(name)] == ' ';
        gboolean all_routes = !unlabeled && strstr(line + strlen(name), "route=\"all\"") != NULL;
        if (!unlabeled && !all_routes) continue;
        if (name == sum_name && unlabeled) {
            aggregate_sum = value;
            has_aggregate_sum = TRUE;
        } else if (name == count_name && unlabeled) {
            aggregate_count = value;
            has_aggregate_count = TRUE;
        } else if (name == sum_name) {
            all_sum = value;
            has_all_sum = TRUE;
        } else {
            all_count = value;
            has_all_count = TRUE;
        }
    }
    g_strfreev(lines);
    gboolean has_aggregate = has_aggregate_sum && has_aggregate_count;
    gboolean has_all = has_all_sum && has_all_count;
    double sum = has_aggregate ? aggregate_sum : all_sum;
    double count = has_aggregate ? aggregate_count : all_count;
    *present = has_aggregate || has_all;
    double result = *present && sum >= 0 && count > 0 ? sum / count * 1000.0 : 0;
    *present = *present && sum >= 0 && count > 0 && isfinite(result);
    return *present ? result : 0;
}

static int route_compare(const void *left, const void *right) {
    json_object *a = *(json_object *const *)left;
    json_object *b = *(json_object *const *)right;
    gint64 ar = json_object_get_int64(json_object_object_get(a, "requests"));
    gint64 br = json_object_get_int64(json_object_object_get(b, "requests"));
    if (ar != br) return ar < br ? 1 : -1;
    return strcmp(json_object_get_string(json_object_object_get(a, "name")),
                  json_object_get_string(json_object_object_get(b, "name")));
}

static char *iso_time(gint64 milliseconds) {
    GDateTime *date = g_date_time_new_from_unix_utc(milliseconds / 1000);
    char *text = date ? g_date_time_format_iso8601(date) : NULL;
    if (date) g_date_time_unref(date);
    return text;
}

CodexBarProvider *codexbar_wayfinder_parse_usage(const char *health_text,
                                                  const char *models_text,
                                                  const char *savings_text,
                                                  const char *metrics,
                                                  gint64 now_ms,
                                                  GError **error) {
    json_object *health = parse_object(health_text);
    json_object *models = parse_object(models_text);
    json_object *savings = parse_object(savings_text);
    const char *status = NULL;
    gboolean offline = FALSE;
    gboolean dry_run = FALSE;
    json_object *model_array = NULL;
    json_object *missing = NULL;
    gboolean valid = health && models && savings && required_string(health, "status", &status) &&
                     required_bool(health, "offline", &offline) && required_bool(models, "dry_run", &dry_run) &&
                     json_object_object_get_ex(models, "models", &model_array) &&
                     json_object_is_type(model_array, json_type_array);
    if (valid && json_object_object_get_ex(health, "missing_keys", &missing)) {
        valid = json_object_is_type(missing, json_type_array);
        for (size_t index = 0; valid && index < json_object_array_length(missing); index++) {
            json_object *key = json_object_array_get_idx(missing, index);
            valid = json_object_is_type(key, json_type_string) && valid_display_text(json_object_get_string(key));
        }
    }
    for (size_t index = 0; valid && index < json_object_array_length(model_array); index++) {
        json_object *model = json_object_array_get_idx(model_array, index);
        const char *name = NULL;
        valid = json_object_is_type(model, json_type_object) && required_string(model, "name", &name);
    }
    gboolean priced = FALSE;
    gint64 requests = 0;
    gint64 tokens = 0;
    double realized = 0;
    double baseline = 0;
    double saved = 0;
    double saved_pct = 0;
    json_object *by_route = NULL;
    valid = valid && required_bool(savings, "priced", &priced) &&
            required_nonnegative_int(savings, "requests", &requests) &&
            required_nonnegative_int(savings, "tokens", &tokens) && required_double(savings, "realized", &realized) &&
            required_double(savings, "baseline", &baseline) && required_double(savings, "saved", &saved) &&
            required_double(savings, "saved_pct", &saved_pct) &&
            json_object_object_get_ex(savings, "by_route", &by_route) && json_object_is_type(by_route, json_type_object);
    json_object *routes = json_object_new_array();
    if (valid) {
        json_object_object_foreach(by_route, name, bucket) {
            gint64 route_requests = 0;
            gint64 route_tokens = 0;
            double route_saved = 0;
            if (!valid_display_text(name) || !json_object_is_type(bucket, json_type_object) ||
                !required_nonnegative_int(bucket, "requests", &route_requests) ||
                !required_nonnegative_int(bucket, "tokens", &route_tokens) ||
                !required_double(bucket, "saved", &route_saved)) {
                valid = FALSE;
                break;
            }
            json_object *route = json_object_new_object();
            json_object_object_add(route, "name", json_object_new_string(name));
            json_object_object_add(route, "requests", json_object_new_int64(route_requests));
            json_object_object_add(route, "saved", json_object_new_double(route_saved));
            json_object_object_add(route, "tokens", json_object_new_int64(route_tokens));
            json_object_array_add(routes, route);
        }
    }
    if (!valid) {
        if (health) json_object_put(health);
        if (models) json_object_put(models);
        if (savings) json_object_put(savings);
        json_object_put(routes);
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Could not parse Wayfinder gateway response");
        return NULL;
    }
    json_object_array_sort(routes, route_compare);
    json_object *usage = json_object_new_object();
    json_object_object_add(usage, "gatewayStatus", json_object_new_string(status));
    json_object_object_add(usage, "offline", json_object_new_boolean(offline));
    json_object_object_add(usage, "dryRun", json_object_new_boolean(dry_run));
    json_object_object_add(usage, "missingKeys", missing ? json_object_get(missing) : json_object_new_array());
    json_object_object_add(usage, "modelCount", json_object_new_int64((gint64)json_object_array_length(model_array)));
    json_object_object_add(usage, "requests", json_object_new_int64(requests));
    json_object_object_add(usage, "tokens", json_object_new_int64(tokens));
    json_object_object_add(usage, "realized", json_object_new_double(realized));
    json_object_object_add(usage, "baseline", json_object_new_double(baseline));
    json_object_object_add(usage, "saved", json_object_new_double(saved));
    json_object_object_add(usage, "savedPct", json_object_new_double(saved_pct));
    json_object_object_add(usage, "priced", json_object_new_boolean(priced));
    json_object_object_add(usage, "routes", routes);
    gboolean has_average = FALSE;
    double average = codexbar_wayfinder_average_decision_ms_for_testing(metrics, &has_average);
    if (has_average) json_object_object_add(usage, "avgDecisionMs", json_object_new_double(average));
    char *updated = iso_time(now_ms);
    if (updated) json_object_object_add(usage, "updatedAt", json_object_new_string(updated));
    g_free(updated);

    CodexBarProvider *provider = codexbar_provider_new();
    provider->provider = g_strdup("wayfinder");
    provider->source = g_strdup("api");
    provider->has_updated_at = TRUE;
    provider->updated_at_ms = now_ms;
    provider->identity = g_new0(CodexBarProviderIdentity, 1);
    guint model_count = (guint)json_object_array_length(model_array);
    provider->identity->organization = g_strdup_printf("%u model%s · local gateway", model_count, model_count == 1 ? "" : "s");
    if (offline) {
        provider->identity->login_method = g_strdup("Offline mode");
    } else if (dry_run) {
        provider->identity->login_method = g_strdup("Dry run");
    } else if (g_str_equal(status, "degraded")) {
        guint count = missing ? (guint)json_object_array_length(missing) : 0;
        provider->identity->login_method = count == 0 ? g_strdup("Degraded")
                                           : count == 1 ? g_strdup("Degraded — 1 key missing")
                                                        : g_strdup_printf("Degraded — %u keys missing", count);
    } else {
        provider->identity->login_method = g_strdup("Local gateway");
    }
    provider->usage_extensions = json_object_new_object();
    json_object_object_add(provider->usage_extensions, "wayfinderUsage", usage);
    json_object_object_add(provider->usage_extensions, "dataConfidence", json_object_new_string("exact"));
    json_object_put(health);
    json_object_put(models);
    json_object_put(savings);
    return provider;
}

static gboolean same_origin(const char *left, const char *right) {
    GUri *a = g_uri_parse(left, G_URI_FLAGS_NONE, NULL);
    GUri *b = g_uri_parse(right, G_URI_FLAGS_NONE, NULL);
    if (!a || !b) {
        if (a) g_uri_unref(a);
        if (b) g_uri_unref(b);
        return FALSE;
    }
    if (!g_uri_get_scheme(a) || !g_uri_get_scheme(b) || !g_uri_get_host(a) || !g_uri_get_host(b)) {
        g_uri_unref(a);
        g_uri_unref(b);
        return FALSE;
    }
    int ap = g_uri_get_port(a);
    int bp = g_uri_get_port(b);
    if (ap < 0) ap = g_ascii_strcasecmp(g_uri_get_scheme(a), "https") == 0 ? 443 : 80;
    if (bp < 0) bp = g_ascii_strcasecmp(g_uri_get_scheme(b), "https") == 0 ? 443 : 80;
    gboolean equal = g_ascii_strcasecmp(g_uri_get_scheme(a), g_uri_get_scheme(b)) == 0 &&
                     g_ascii_strcasecmp(g_uri_get_host(a), g_uri_get_host(b)) == 0 && ap == bp;
    g_uri_unref(a);
    g_uri_unref(b);
    return equal;
}

static CodexBarHttpResponse *get(const char *base,
                                 const char *path,
                                 const char *query,
                                 CodexBarWayfinderTransport transport,
                                 GCancellable *cancellable,
                                 gboolean optional,
                                 GError **error) {
    char *url = codexbar_wayfinder_endpoint_for_testing(base, path, query, error);
    if (!url) return NULL;
    GUri *base_uri = g_uri_parse(base, G_URI_FLAGS_NONE, NULL);
    gboolean https = base_uri && g_ascii_strcasecmp(g_uri_get_scheme(base_uri), "https") == 0;
    if (base_uri) g_uri_unref(base_uri);
    const CodexBarHttpRequest request = {
        .url = url,
        .method = "GET",
        .timeout_seconds = 5,
        .maximum_response_bytes = WAYFINDER_MAXIMUM_RESPONSE_BYTES,
        .protocol_policy = CODEXBAR_HTTP_ALLOW_LOOPBACK_HTTP,
        .redirect_policy = https ? CODEXBAR_HTTP_REDIRECT_SAME_ORIGIN : CODEXBAR_HTTP_REDIRECT_DENY,
        .cancellable = cancellable,
    };
    CodexBarHttpResponse *response = transport(&request, error);
    if (!response) {
        g_free(url);
        if (error && *error && (*error)->domain == G_IO_ERROR && (*error)->code == G_IO_ERROR_CANCELLED) return NULL;
        if (optional) {
            g_clear_error(error);
            return NULL;
        }
        g_clear_error(error);
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_CONNECTION_REFUSED,
                            "Could not reach the Wayfinder gateway. Start it with `wayfinder-router serve` "
                            "(default http://127.0.0.1:8088) or fix the Gateway URL in Settings.");
        return NULL;
    }
    gboolean origin_ok = same_origin(url, response->effective_url ? response->effective_url : url);
    g_free(url);
    if (!origin_ok) {
        codexbar_http_response_free(response);
        if (optional) return NULL;
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED, "Wayfinder gateway request was redirected to a different origin.");
        return NULL;
    }
    if (response->status < 200 || response->status >= 300) {
        long status = response->status;
        codexbar_http_response_free(response);
        if (optional) return NULL;
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Wayfinder gateway returned HTTP %ld.", status);
        return NULL;
    }
    return response;
}

static gboolean response_is_text(const CodexBarHttpResponse *response) {
    return response && response->body && !memchr(response->body, '\0', response->body_length) &&
           g_utf8_validate(response->body, (gssize)response->body_length, NULL) &&
           response->body_length == strlen(response->body);
}

CodexBarProvider *codexbar_wayfinder_fetch_with_transport_and_cancellable(const CodexBarProviderConfig *config,
                                                                           CodexBarWayfinderTransport transport,
                                                                           GCancellable *cancellable,
                                                                           gint64 now_ms,
                                                                           GError **error) {
    char *base = codexbar_wayfinder_base_url_for_testing(config, error);
    if (!base) return NULL;
    CodexBarHttpResponse *health = get(base, "healthz", NULL, transport, cancellable, FALSE, error);
    CodexBarHttpResponse *models = health ? get(base, "router/models", NULL, transport, cancellable, FALSE, error) : NULL;
    CodexBarHttpResponse *savings = models ? get(base, "v1/savings", "period=30d", transport, cancellable, FALSE, error) : NULL;
    CodexBarHttpResponse *metrics = savings ? get(base, "metrics", NULL, transport, cancellable, TRUE, error) : NULL;
    if (!health || !models || !savings || (error && *error)) {
        g_free(base);
        codexbar_http_response_free(health);
        codexbar_http_response_free(models);
        codexbar_http_response_free(savings);
        codexbar_http_response_free(metrics);
        return NULL;
    }
    if (!response_is_text(health) || !response_is_text(models) || !response_is_text(savings)) {
        g_free(base);
        codexbar_http_response_free(health);
        codexbar_http_response_free(models);
        codexbar_http_response_free(savings);
        codexbar_http_response_free(metrics);
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Could not parse Wayfinder gateway response");
        return NULL;
    }
    CodexBarProvider *provider = codexbar_wayfinder_parse_usage(
        health->body,
        models->body,
        savings->body,
        response_is_text(metrics) ? metrics->body : NULL,
        now_ms,
        error);
    if (provider) provider->dashboard_url = codexbar_wayfinder_dashboard_url_for_testing(config, NULL);
    g_free(base);
    codexbar_http_response_free(health);
    codexbar_http_response_free(models);
    codexbar_http_response_free(savings);
    codexbar_http_response_free(metrics);
    return provider;
}

CodexBarProvider *codexbar_wayfinder_fetch_with_transport(const CodexBarProviderConfig *config,
                                                           CodexBarWayfinderTransport transport,
                                                           gint64 now_ms,
                                                           GError **error) {
    return codexbar_wayfinder_fetch_with_transport_and_cancellable(config, transport, NULL, now_ms, error);
}

CodexBarProvider *codexbar_wayfinder_fetch_with_cancellable(const CodexBarProviderConfig *config,
                                                             GCancellable *cancellable,
                                                             GError **error) {
    return codexbar_wayfinder_fetch_with_transport_and_cancellable(
        config, codexbar_http_send, cancellable, g_get_real_time() / 1000, error);
}

CodexBarProvider *codexbar_wayfinder_fetch(const CodexBarProviderConfig *config, GError **error) {
    return codexbar_wayfinder_fetch_with_cancellable(config, NULL, error);
}
