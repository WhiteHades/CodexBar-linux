#include "http.h"

#include <curl/curl.h>

static GQuark http_error_quark(void) {
    return g_quark_from_static_string("codexbar-http-error");
}

static size_t append_body(char *data, size_t size, size_t count, void *user_data) {
    GByteArray *body = user_data;
    size_t bytes = size * count;
    g_byte_array_append(body, (const guint8 *)data, bytes);
    return bytes;
}

CodexBarHttpResponse *codexbar_http_get(
    const char *url, const char *auth_header, const char *auth_value, GError **error) {
    CURL *curl = curl_easy_init();
    if (!curl) {
        g_set_error_literal(error, http_error_quark(), 1, "Could not initialize libcurl");
        return NULL;
    }
    GByteArray *body = g_byte_array_new();
    struct curl_slist *headers = NULL;
    char *authorization = g_strdup_printf("%s: %s", auth_header, auth_value);
    headers = curl_slist_append(headers, authorization);
    headers = curl_slist_append(headers, "Accept: application/json");
    headers = curl_slist_append(headers, "X-Title: CodexBar");

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, append_body);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, body);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "https");
    CURLcode result = curl_easy_perform(curl);
    if (result != CURLE_OK) {
        g_set_error(error, http_error_quark(), (int)result, "HTTP request failed: %s", curl_easy_strerror(result));
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
        g_byte_array_unref(body);
        g_free(authorization);
        return NULL;
    }

    CodexBarHttpResponse *response = g_new0(CodexBarHttpResponse, 1);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response->status);
    g_byte_array_append(body, (const guint8 *)"", 1);
    response->body = (char *)g_byte_array_free(body, FALSE);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    g_free(authorization);
    return response;
}

void codexbar_http_response_free(CodexBarHttpResponse *response) {
    if (response) {
        g_free(response->body);
        g_free(response);
    }
}
