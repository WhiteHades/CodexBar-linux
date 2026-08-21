#include "fireworks.h"

#include <gio/gio.h>
#include <json-c/json.h>
#include <math.h>
#include <string.h>

#define FIREWORKS_MAXIMUM_RESPONSE_BYTES (1024U * 1024U)
#define FIREWORKS_MAXIMUM_ACCOUNT_PAGES 100U

static char *clean_value(const char *raw) {
    if (!raw) return NULL;
    char *clean = g_strstrip(g_strdup(raw));
    size_t length = strlen(clean);
    if (length >= 2 && ((clean[0] == '\'' && clean[length - 1] == '\'') ||
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

static char *config_string(const CodexBarProviderConfig *config, const char *key) {
    json_object *value = NULL;
    if (!config || !config->raw || !json_object_object_get_ex(config->raw, key, &value) ||
        !json_object_is_type(value, json_type_string)) {
        return NULL;
    }
    const char *text = json_object_get_string(value);
    size_t length = (size_t)json_object_get_string_len(value);
    return memchr(text, '\0', length) ? NULL : clean_value(text);
}

static char *resolve_api_key(const CodexBarProviderConfig *config) {
    char *key = clean_value(config ? config->api_key : NULL);
    if (!key) key = clean_value(g_getenv("FIREWORKS_API_KEY"));
    if (!key) key = clean_value(g_getenv("FIREWORKS_KEY"));
    return key;
}

static char *resolve_account_slug(const CodexBarProviderConfig *config) {
    char *slug = config_string(config, "accountSlug");
    if (!slug) slug = clean_value(g_getenv("FIREWORKS_ACCOUNT_SLUG"));
    return slug;
}

gboolean codexbar_fireworks_account_slug_is_valid(const char *slug) {
    if (!slug || slug[0] == '\0') return FALSE;
    for (const unsigned char *cursor = (const unsigned char *)slug; *cursor; cursor++) {
        if (!g_ascii_isalnum(*cursor) && *cursor != '.' && *cursor != '_' && *cursor != '-') return FALSE;
    }
    return TRUE;
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

static gboolean money_value(json_object *item, char **currency, double *value) {
    json_object *cost = NULL;
    if (!json_object_object_get_ex(item, "totalCost", &cost) || json_object_is_type(cost, json_type_null)) {
        return FALSE;
    }
    if (!json_object_is_type(cost, json_type_object)) return FALSE;
    json_object *currency_value = NULL;
    json_object *units_value = NULL;
    json_object *nanos_value = NULL;
    if (!json_object_object_get_ex(cost, "currencyCode", &currency_value) ||
        !json_object_is_type(currency_value, json_type_string) ||
        !json_object_object_get_ex(cost, "units", &units_value) ||
        !json_object_is_type(units_value, json_type_string) ||
        !json_object_object_get_ex(cost, "nanos", &nanos_value) ||
        !json_object_is_type(nanos_value, json_type_int)) {
        return FALSE;
    }
    const char *raw_currency = json_object_get_string(currency_value);
    const char *raw_units = json_object_get_string(units_value);
    if (memchr(raw_currency, '\0', (size_t)json_object_get_string_len(currency_value)) ||
        memchr(raw_units, '\0', (size_t)json_object_get_string_len(units_value))) {
        return FALSE;
    }
    char *clean_currency = g_strstrip(g_strdup(raw_currency));
    char *end = NULL;
    double units = g_ascii_strtod(raw_units, &end);
    if (clean_currency[0] == '\0' || raw_units[0] == '\0' || !end || *end != '\0' || !isfinite(units)) {
        g_free(clean_currency);
        return FALSE;
    }
    double total = units + (double)json_object_get_int64(nanos_value) / 1000000000.0;
    if (!isfinite(total)) {
        g_free(clean_currency);
        return FALSE;
    }
    *currency = clean_currency;
    *value = total;
    return TRUE;
}

CodexBarProvider *codexbar_fireworks_parse_summary_bytes(
    const char *json, size_t length, gint64 now_ms, GError **error) {
    json_object *root = parse_json_document(json, length);
    json_object *line_items = NULL;
    gboolean has_line_items = root && json_object_object_get_ex(root, "lineItems", &line_items) &&
                              !json_object_is_type(line_items, json_type_null);
    if (!root || !json_object_is_type(root, json_type_object) ||
        (has_line_items && !json_object_is_type(line_items, json_type_array))) {
        if (root) json_object_put(root);
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Fireworks billing response is malformed");
        return NULL;
    }

    char *currency = NULL;
    double total = 0.0;
    for (size_t index = 0; has_line_items && index < json_object_array_length(line_items); index++) {
        json_object *item = json_object_array_get_idx(line_items, index);
        if (!item || !json_object_is_type(item, json_type_object)) {
            json_object_put(root);
            g_free(currency);
            g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Fireworks billing response is malformed");
            return NULL;
        }
        char *item_currency = NULL;
        double item_value = 0.0;
        if (!money_value(item, &item_currency, &item_value)) continue;
        if (!currency) currency = g_strdup(item_currency);
        if (g_str_equal(currency, item_currency)) total += item_value;
        g_free(item_currency);
        if (!isfinite(total)) {
            json_object_put(root);
            g_free(currency);
            g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Fireworks billing response is malformed");
            return NULL;
        }
    }

    CodexBarProvider *provider = codexbar_provider_new();
    provider->provider = g_strdup("fireworks");
    provider->source = g_strdup("api");
    provider->has_updated_at = TRUE;
    provider->updated_at_ms = now_ms;
    if (currency) {
        provider->provider_cost = g_new0(CodexBarProviderCost, 1);
        provider->provider_cost->used = total;
        provider->provider_cost->limit = 0.0;
        provider->provider_cost->currency = currency;
        provider->provider_cost->period = g_strdup("Last 30 days");
        provider->provider_cost->has_updated_at = TRUE;
        provider->provider_cost->updated_at_ms = now_ms;
    }
    json_object_put(root);
    return provider;
}

CodexBarProvider *codexbar_fireworks_parse_summary(const char *json, gint64 now_ms, GError **error) {
    return codexbar_fireworks_parse_summary_bytes(json, json ? strlen(json) : 0, now_ms, error);
}

static char *summary_url(const char *slug, gint64 now_ms, GError **error) {
    if (!codexbar_fireworks_account_slug_is_valid(slug)) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "Invalid Fireworks account slug.");
        return NULL;
    }
    GDateTime *end = g_date_time_new_from_unix_utc(now_ms / 1000);
    if (!end) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "Invalid Fireworks billing time.");
        return NULL;
    }
    GDateTime *start = g_date_time_add_days(end, -30);
    char *start_text = g_date_time_format(start, "%Y-%m-%dT%H:%M:%SZ");
    char *end_text = g_date_time_format(end, "%Y-%m-%dT%H:%M:%SZ");
    char *url = g_strdup_printf("https://api.fireworks.ai/v1/accounts/%s/billing/summary?startTime=%s&endTime=%s",
                                slug,
                                start_text,
                                end_text);
    g_free(end_text);
    g_free(start_text);
    g_date_time_unref(start);
    g_date_time_unref(end);
    return url;
}

static char *accounts_url(const char *page_token) {
    if (!page_token) return g_strdup("https://api.fireworks.ai/v1/accounts");
    char *escaped = g_uri_escape_string(page_token, NULL, FALSE);
    char *url = g_strdup_printf("https://api.fireworks.ai/v1/accounts?pageToken=%s", escaped);
    g_free(escaped);
    return url;
}

static CodexBarHttpResponse *fireworks_send_get(const char *url,
                                                const char *authorization,
                                                CodexBarFireworksTransport transport,
                                                GCancellable *cancellable,
                                                GError **error) {
    const CodexBarHttpRequestHeader headers[] = {
        {"Authorization", authorization},
        {"Accept", "application/json"},
    };
    const CodexBarHttpRequest request = {
        .url = url,
        .method = "GET",
        .headers = headers,
        .header_count = G_N_ELEMENTS(headers),
        .timeout_seconds = 15,
        .maximum_response_bytes = FIREWORKS_MAXIMUM_RESPONSE_BYTES,
        .protocol_policy = CODEXBAR_HTTP_HTTPS_ONLY,
        .redirect_policy = CODEXBAR_HTTP_REDIRECT_DENY,
        .cancellable = cancellable,
    };
    GError *request_error = NULL;
    CodexBarHttpResponse *response = transport(&request, &request_error);
    if (cancellable && g_cancellable_is_cancelled(cancellable)) {
        codexbar_http_response_free(response);
        g_clear_error(&request_error);
        g_cancellable_set_error_if_cancelled(cancellable, error);
        return NULL;
    }
    if (response) {
        g_clear_error(&request_error);
        return response;
    }
    if (request_error && request_error->domain == G_IO_ERROR && request_error->code == G_IO_ERROR_CANCELLED) {
        g_propagate_error(error, request_error);
    } else {
        g_clear_error(&request_error);
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Fireworks network request failed.");
    }
    return NULL;
}

static void fireworks_set_http_error(const char *surface, long status, GError **error) {
    if (status == 401 || status == 403) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED, "Fireworks rejected the API key.");
    } else if (status == 429) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_BUSY, "Fireworks rate limit exceeded.");
    } else {
        g_set_error(error,
                    G_IO_ERROR,
                    G_IO_ERROR_FAILED,
                    "Fireworks %s API returned HTTP %ld.",
                    surface,
                    status);
    }
}

static char *json_clean_string_member(json_object *object, const char *key, gboolean *malformed) {
    json_object *value = NULL;
    if (!json_object_object_get_ex(object, key, &value) || json_object_is_type(value, json_type_null)) return NULL;
    if (!json_object_is_type(value, json_type_string)) {
        *malformed = TRUE;
        return NULL;
    }
    const char *text = json_object_get_string(value);
    size_t length = (size_t)json_object_get_string_len(value);
    if (memchr(text, '\0', length)) {
        *malformed = TRUE;
        return NULL;
    }
    return clean_value(text);
}

static char *account_slug_from_object(json_object *object, gboolean *malformed) {
    static const char *const keys[] = {"accountId", "id", "name"};
    for (size_t index = 0; index < G_N_ELEMENTS(keys); index++) {
        char *candidate = json_clean_string_member(object, keys[index], malformed);
        if (*malformed) return NULL;
        if (!candidate) continue;
        char *slash = strrchr(candidate, '/');
        if (slash) memmove(candidate, slash + 1, strlen(slash + 1) + 1);
        if (codexbar_fireworks_account_slug_is_valid(candidate)) return candidate;
        g_free(candidate);
    }
    return NULL;
}

static gint compare_string_pointers(gconstpointer left, gconstpointer right) {
    return g_strcmp0(*(char *const *)left, *(char *const *)right);
}

static GPtrArray *fireworks_list_account_slugs(const char *authorization,
                                               CodexBarFireworksTransport transport,
                                               GCancellable *cancellable,
                                               GError **error) {
    GHashTable *unique = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    GHashTable *seen_tokens = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    char *page_token = NULL;
    for (guint page = 0; page < FIREWORKS_MAXIMUM_ACCOUNT_PAGES; page++) {
        char *url = accounts_url(page_token);
        CodexBarHttpResponse *response = fireworks_send_get(url, authorization, transport, cancellable, error);
        g_free(url);
        if (!response) goto failed;
        if (response->status != 200) {
            fireworks_set_http_error("accounts", response->status, error);
            codexbar_http_response_free(response);
            goto failed;
        }

        json_object *root = parse_json_document(response->body, response->body_length);
        codexbar_http_response_free(response);
        if (!root || !json_object_is_type(root, json_type_object)) {
            if (root) json_object_put(root);
            g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                                "Fireworks accounts response is malformed.");
            goto failed;
        }
        json_object *accounts = NULL;
        if (json_object_object_get_ex(root, "accounts", &accounts) &&
            !json_object_is_type(accounts, json_type_null)) {
            if (!json_object_is_type(accounts, json_type_array)) {
                json_object_put(root);
                g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                                    "Fireworks accounts response is malformed.");
                goto failed;
            }
            for (size_t index = 0; index < json_object_array_length(accounts); index++) {
                json_object *account = json_object_array_get_idx(accounts, index);
                if (!account || !json_object_is_type(account, json_type_object)) {
                    json_object_put(root);
                    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                                        "Fireworks accounts response is malformed.");
                    goto failed;
                }
                gboolean malformed = FALSE;
                char *slug = account_slug_from_object(account, &malformed);
                if (malformed) {
                    json_object_put(root);
                    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                                        "Fireworks accounts response is malformed.");
                    goto failed;
                }
                if (slug) {
                    if (g_hash_table_contains(unique, slug)) {
                        g_free(slug);
                    } else {
                        g_hash_table_add(unique, slug);
                    }
                }
            }
        }

        gboolean malformed = FALSE;
        char *next_token = json_clean_string_member(root, "nextPageToken", &malformed);
        json_object_put(root);
        if (malformed) {
            g_free(next_token);
            g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                                "Fireworks accounts response is malformed.");
            goto failed;
        }
        g_free(page_token);
        page_token = next_token;
        if (!page_token) {
            GPtrArray *slugs = g_ptr_array_new_with_free_func(g_free);
            GHashTableIter iterator;
            gpointer key = NULL;
            g_hash_table_iter_init(&iterator, unique);
            while (g_hash_table_iter_next(&iterator, &key, NULL)) g_ptr_array_add(slugs, g_strdup(key));
            g_ptr_array_sort(slugs, compare_string_pointers);
            g_hash_table_unref(seen_tokens);
            g_hash_table_unref(unique);
            return slugs;
        }
        if (g_hash_table_contains(seen_tokens, page_token)) {
            g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                                "Fireworks accounts pagination token repeated.");
            goto failed;
        }
        g_hash_table_add(seen_tokens, g_strdup(page_token));
    }
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                        "Fireworks accounts pagination exceeded 100 pages.");

failed:
    g_free(page_token);
    g_hash_table_unref(seen_tokens);
    g_hash_table_unref(unique);
    return NULL;
}

static char *single_discovered_slug(GPtrArray *slugs, GError **error) {
    if (slugs->len == 0) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                            "No Fireworks accounts are visible to this API key.");
        return NULL;
    }
    if (slugs->len == 1) return g_strdup(g_ptr_array_index(slugs, 0));
    GString *joined = g_string_new(NULL);
    for (guint index = 0; index < slugs->len; index++) {
        if (index > 0) g_string_append(joined, ", ");
        g_string_append(joined, g_ptr_array_index(slugs, index));
    }
    g_set_error(error,
                G_IO_ERROR,
                G_IO_ERROR_FAILED,
                "This Fireworks API key can access multiple accounts: %s. Set accountSlug or "
                "FIREWORKS_ACCOUNT_SLUG.",
                joined->str);
    g_string_free(joined, TRUE);
    return NULL;
}

static void set_account_not_found_error(const char *slug, GError **error) {
    g_set_error(error,
                G_IO_ERROR,
                G_IO_ERROR_NOT_FOUND,
                "Fireworks account slug '%s' was not found for this API key. Leave accountSlug blank to "
                "auto-discover it.",
                slug);
}

static CodexBarProvider *fireworks_fetch_summary(const char *slug,
                                                 const char *authorization,
                                                 CodexBarFireworksTransport transport,
                                                 GCancellable *cancellable,
                                                 gint64 now_ms,
                                                 long *status,
                                                 GError **error) {
    char *url = summary_url(slug, now_ms, error);
    if (!url) return NULL;
    CodexBarHttpResponse *response = fireworks_send_get(url, authorization, transport, cancellable, error);
    g_free(url);
    if (!response) return NULL;
    *status = response->status;
    if (response->status != 200) {
        if (response->status != 404) fireworks_set_http_error("billing", response->status, error);
        codexbar_http_response_free(response);
        return NULL;
    }
    CodexBarProvider *provider = codexbar_fireworks_parse_summary_bytes(
        response->body, response->body_length, now_ms, error);
    codexbar_http_response_free(response);
    return provider;
}

static void fireworks_attach_account(CodexBarProvider *provider, const char *slug, gboolean discovered) {
    provider->identity = g_new0(CodexBarProviderIdentity, 1);
    provider->identity->account_id = g_strdup(slug);
    provider->identity->login_method = g_strdup(discovered ? "API key (discovered)" : "API key");
    provider->usage_extensions = json_object_new_object();
    json_object_object_add(provider->usage_extensions, "dataConfidence", json_object_new_string("exact"));
    json_object_object_add(provider->usage_extensions, "fireworksAccountSlug", json_object_new_string(slug));
    json_object_object_add(provider->usage_extensions,
                           "fireworksAccountSlugWasDiscovered",
                           json_object_new_boolean(discovered));
}

CodexBarProvider *codexbar_fireworks_fetch_with_transport_and_cancellable(const CodexBarProviderConfig *config,
                                                                          CodexBarFireworksTransport transport,
                                                                          GCancellable *cancellable,
                                                                          gint64 now_ms,
                                                                          GError **error) {
    g_return_val_if_fail(transport != NULL, NULL);
    if (cancellable && g_cancellable_set_error_if_cancelled(cancellable, error)) return NULL;
    char *api_key = resolve_api_key(config);
    if (!api_key) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "Missing Fireworks API key.");
        return NULL;
    }
    char *authorization = g_strdup_printf("Bearer %s", api_key);
    g_free(api_key);
    char *slug = resolve_account_slug(config);
    gboolean discovered = FALSE;
    if (!slug) {
        GPtrArray *slugs = fireworks_list_account_slugs(authorization, transport, cancellable, error);
        if (!slugs) {
            g_free(authorization);
            return NULL;
        }
        slug = single_discovered_slug(slugs, error);
        g_ptr_array_unref(slugs);
        if (!slug) {
            g_free(authorization);
            return NULL;
        }
        discovered = TRUE;
    }

    long status = 0;
    CodexBarProvider *provider = fireworks_fetch_summary(
        slug, authorization, transport, cancellable, now_ms, &status, error);
    if (!provider && status == 404 && discovered) {
        fireworks_set_http_error("billing", status, error);
    } else if (!provider && status == 404) {
        GPtrArray *slugs = fireworks_list_account_slugs(authorization, transport, cancellable, error);
        if (!slugs) goto finished;
        if (slugs->len == 1) {
            char *replacement = g_strdup(g_ptr_array_index(slugs, 0));
            g_ptr_array_unref(slugs);
            discovered = !g_str_equal(replacement, slug);
            g_free(slug);
            slug = replacement;
            status = 0;
            provider = fireworks_fetch_summary(
                slug, authorization, transport, cancellable, now_ms, &status, error);
            if (!provider && status == 404) fireworks_set_http_error("billing", status, error);
        } else if (slugs->len == 0) {
            g_ptr_array_unref(slugs);
            set_account_not_found_error(slug, error);
        } else {
            char *unused = single_discovered_slug(slugs, error);
            g_free(unused);
            g_ptr_array_unref(slugs);
        }
    } else if (provider && !provider->provider_cost && !discovered) {
        GPtrArray *slugs = fireworks_list_account_slugs(authorization, transport, cancellable, error);
        if (!slugs) {
            codexbar_provider_free(provider);
            provider = NULL;
            goto finished;
        }
        gboolean found = FALSE;
        for (guint index = 0; index < slugs->len; index++) {
            if (g_str_equal(slug, g_ptr_array_index(slugs, index))) {
                found = TRUE;
                break;
            }
        }
        g_ptr_array_unref(slugs);
        if (!found) {
            codexbar_provider_free(provider);
            provider = NULL;
            set_account_not_found_error(slug, error);
        }
    }
    if (provider) fireworks_attach_account(provider, slug, discovered);

finished:
    g_free(slug);
    g_free(authorization);
    return provider;
}

CodexBarProvider *codexbar_fireworks_fetch_with_cancellable(const CodexBarProviderConfig *config,
                                                            GCancellable *cancellable,
                                                            GError **error) {
    return codexbar_fireworks_fetch_with_transport_and_cancellable(
        config, codexbar_http_send, cancellable, g_get_real_time() / 1000, error);
}

CodexBarProvider *codexbar_fireworks_fetch(const CodexBarProviderConfig *config, GError **error) {
    return codexbar_fireworks_fetch_with_cancellable(config, NULL, error);
}
