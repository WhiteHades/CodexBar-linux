#include "model.h"
#include "render.h"

#include <glib.h>
#include <json-c/json.h>

static char *fixture_path;

static void discard_reference_payloads(CodexBarSnapshot *snapshot) {
    for (guint index = 0; index < snapshot->providers->len; index++) {
        CodexBarProvider *provider = g_ptr_array_index(snapshot->providers, index);
        if (provider->raw) {
            json_object_put(provider->raw);
            provider->raw = NULL;
        }
    }
}

static void test_fixture_contract(void) {
    GError *error = NULL;
    char *contents = NULL;
    g_assert_true(g_file_get_contents(fixture_path, &contents, NULL, &error));
    g_assert_no_error(error);

    json_object *manifest = json_tokener_parse(contents);
    g_free(contents);
    g_assert_nonnull(manifest);
    g_assert_true(json_object_is_type(manifest, json_type_object));
    json_object *schema_version = NULL;
    json_object *cases = NULL;
    g_assert_true(json_object_object_get_ex(manifest, "schemaVersion", &schema_version));
    g_assert_cmpint(json_object_get_int(schema_version), ==, 1);
    g_assert_true(json_object_object_get_ex(manifest, "cases", &cases));
    g_assert_true(json_object_is_type(cases, json_type_array));

    GHashTable *names = g_hash_table_new(g_str_hash, g_str_equal);
    for (size_t index = 0; index < json_object_array_length(cases); index++) {
        json_object *test_case = json_object_array_get_idx(cases, index);
        json_object *name_value = NULL;
        json_object *payloads = NULL;
        g_assert_true(json_object_object_get_ex(test_case, "name", &name_value));
        g_assert_true(json_object_is_type(name_value, json_type_string));
        const char *name = json_object_get_string(name_value);
        g_assert_true(name[0] != '\0');
        g_assert_false(g_hash_table_contains(names, name));
        g_hash_table_add(names, (gpointer)name);
        g_assert_true(json_object_object_get_ex(test_case, "payloads", &payloads));
        g_assert_true(json_object_is_type(payloads, json_type_array));

        const char *payload_json = json_object_to_json_string_ext(payloads, JSON_C_TO_STRING_PLAIN);
        CodexBarSnapshot *snapshot = codexbar_snapshot_parse(payload_json, &error);
        g_assert_no_error(error);
        g_assert_nonnull(snapshot);
        discard_reference_payloads(snapshot);
        char *actual_json = codexbar_render_usage_json(snapshot, FALSE);
        json_object *actual = json_tokener_parse(actual_json);
        g_test_message("%s actual: %s", name, actual_json);
        g_test_message(
            "%s expected: %s", name, json_object_to_json_string_ext(payloads, JSON_C_TO_STRING_PLAIN));
        g_assert_true(json_object_equal(actual, payloads));

        CodexBarSnapshot *round_trip = codexbar_snapshot_parse(actual_json, &error);
        g_assert_no_error(error);
        g_assert_nonnull(round_trip);
        discard_reference_payloads(round_trip);
        char *second_json = codexbar_render_usage_json(round_trip, FALSE);
        json_object *second = json_tokener_parse(second_json);
        g_assert_true(json_object_equal(second, actual));
        json_object_put(second);
        g_free(second_json);
        codexbar_snapshot_free(round_trip);
        json_object_put(actual);
        g_free(actual_json);
        codexbar_snapshot_free(snapshot);
    }
    g_hash_table_unref(names);
    json_object_put(manifest);
}

int main(int argc, char **argv) {
    g_assert_cmpint(argc, ==, 2);
    fixture_path = g_strdup(argv[1]);
    argc = 1;
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/serialization/native-fixtures", test_fixture_contract);
    int result = g_test_run();
    g_free(fixture_path);
    return result;
}
