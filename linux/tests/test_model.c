#include "model.h"
#include "render.h"

#include <glib.h>
#include <json-c/json.h>

static const char *fixture =
    "[{"
    "\"provider\":\"codex\","
    "\"account\":\"dev@example.com\","
    "\"source\":\"oauth\","
    "\"usage\":{"
    "\"primary\":{\"usedPercent\":28,\"resetDescription\":\"Resets in 2h\"},"
    "\"secondary\":{\"usedPercent\":71.4,\"resetDescription\":\"Resets Friday\"},"
    "\"tertiary\":null},"
    "\"credits\":{\"remaining\":12.5}"
    "},{"
    "\"provider\":\"claude\","
    "\"source\":\"cli\","
    "\"usage\":{\"primary\":{\"usedPercent\":91}},"
    "\"error\":null"
    "}]";

static void test_parse_snapshot(void) {
    GError *error = NULL;
    CodexBarSnapshot *snapshot = codexbar_snapshot_parse(fixture, &error);
    g_assert_no_error(error);
    g_assert_nonnull(snapshot);
    g_assert_cmpuint(snapshot->providers->len, ==, 2);

    CodexBarProvider *codex = g_ptr_array_index(snapshot->providers, 0);
    g_assert_cmpstr(codex->provider, ==, "codex");
    g_assert_cmpstr(codex->account, ==, "dev@example.com");
    g_assert_true(codex->primary.available);
    g_assert_cmpfloat(codex->primary.used_percent, ==, 28.0);
    g_assert_true(codex->has_credits);
    g_assert_cmpfloat(codex->credits_remaining, ==, 12.5);
    g_assert_cmpfloat(codexbar_snapshot_highest_used(snapshot), ==, 91.0);
    codexbar_snapshot_free(snapshot);
}

static void test_rejects_non_array(void) {
    GError *error = NULL;
    CodexBarSnapshot *snapshot = codexbar_snapshot_parse("{}", &error);
    g_assert_null(snapshot);
    g_assert_error(error, g_quark_from_static_string("codexbar-model-error"), 2);
    g_clear_error(&error);
}

static void test_waybar_rendering(void) {
    GError *error = NULL;
    CodexBarSnapshot *snapshot = codexbar_snapshot_parse(fixture, &error);
    g_assert_no_error(error);

    char *rendered = codexbar_render_waybar(snapshot);
    json_object *object = json_tokener_parse(rendered);
    g_assert_nonnull(object);

    json_object *class_name = NULL;
    json_object *percentage = NULL;
    json_object *tooltip = NULL;
    g_assert_true(json_object_object_get_ex(object, "class", &class_name));
    g_assert_true(json_object_object_get_ex(object, "percentage", &percentage));
    g_assert_true(json_object_object_get_ex(object, "tooltip", &tooltip));
    g_assert_cmpstr(json_object_get_string(class_name), ==, "critical");
    g_assert_cmpint(json_object_get_int(percentage), ==, 91);
    g_assert_nonnull(strstr(json_object_get_string(tooltip), "codex // dev@example.com"));
    g_assert_nonnull(strstr(json_object_get_string(tooltip), "claude"));

    json_object_put(object);
    g_free(rendered);
    codexbar_snapshot_free(snapshot);
}

int main(int argc, char **argv) {
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/model/parse-snapshot", test_parse_snapshot);
    g_test_add_func("/model/reject-non-array", test_rejects_non_array);
    g_test_add_func("/render/waybar", test_waybar_rendering);
    return g_test_run();
}
