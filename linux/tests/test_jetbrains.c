#include "jetbrains.h"

#include <gio/gio.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <math.h>
#include <string.h>
#include <sys/stat.h>
#include <utime.h>

static const char *fixture_directory;

static char *load_fixture(const char *name, gsize *length) {
    char *path = g_build_filename(fixture_directory, name, NULL);
    char *contents = NULL;
    GError *error = NULL;
    g_assert_true(g_file_get_contents(path, &contents, length, &error));
    g_assert_no_error(error);
    g_free(path);
    return contents;
}

static gint64 timestamp_ms(const char *iso8601) {
    GDateTime *time = g_date_time_new_from_iso8601(iso8601, NULL);
    g_assert_nonnull(time);
    gint64 result = g_date_time_to_unix(time) * 1000 + g_date_time_get_microsecond(time) / 1000;
    g_date_time_unref(time);
    return result;
}

static CodexBarProvider *parse_fixture(const char *name, const char *ide, gint64 now_ms, GError **error) {
    gsize length = 0;
    char *xml = load_fixture(name, &length);
    CodexBarProvider *provider = codexbar_jetbrains_parse_xml(xml, length, ide, now_ms, error);
    g_free(xml);
    return provider;
}

static CodexBarQuotaWindow *primary_window(CodexBarProvider *provider) {
    CodexBarQuotaWindow *window = codexbar_provider_quota_window(provider, 0);
    g_assert_nonnull(window);
    return window;
}

static void test_full_quota(void) {
    GError *error = NULL;
    gint64 now_ms = timestamp_ms("2030-01-15T12:00:00Z");
    CodexBarProvider *provider = parse_fixture("quota-full.xml", "IntelliJ IDEA 2025.3", now_ms, &error);
    g_assert_no_error(error);
    g_assert_nonnull(provider);
    g_assert_cmpstr(provider->provider, ==, "jetbrains");
    g_assert_cmpstr(provider->source, ==, "local");
    g_assert_true(provider->has_updated_at);
    g_assert_cmpint(provider->updated_at_ms, ==, now_ms);
    g_assert_nonnull(provider->identity);
    g_assert_cmpstr(provider->identity->organization, ==, "IntelliJ IDEA 2025.3");
    g_assert_cmpstr(provider->identity->login_method, ==, "Available");
    g_assert_cmpuint(provider->quota_windows->len, ==, 1);
    CodexBarQuotaWindow *window = primary_window(provider);
    g_assert_cmpstr(window->id, ==, "primary");
    g_assert_cmpstr(window->title, ==, "Current");
    g_assert_true(window->usage_known);
    g_assert_cmpfloat_with_epsilon(window->used_percent, 25.0, 0.000001);
    g_assert_true(window->has_resets_at);
    g_assert_cmpint(window->resets_at_ms, ==, timestamp_ms("2030-01-16T14:00:54.939Z"));
    g_assert_cmpstr(window->reset_description, ==, "Resets in 1d 2h");
    codexbar_provider_free(provider);
}

static void test_supported_xml_variants(void) {
    struct {
        const char *fixture;
        double expected_percent;
        const char *expected_type;
    } cases[] = {
        {"quota-only.xml", 5.0, "free"},
        {"reversed-attributes.xml", 20.0, "paid"},
        {"single-quotes.xml", 10.0, "single"},
        {"invalid-refill.xml", 20.0, "valid"},
        {"empty-quota.xml", 0.0, NULL},
    };
    for (guint index = 0; index < G_N_ELEMENTS(cases); index++) {
        GError *error = NULL;
        CodexBarProvider *provider = parse_fixture(cases[index].fixture, NULL, 0, &error);
        g_assert_no_error(error);
        g_assert_nonnull(provider);
        g_assert_cmpfloat_with_epsilon(
            primary_window(provider)->used_percent, cases[index].expected_percent, 0.000001);
        g_assert_cmpstr(provider->identity->login_method, ==, cases[index].expected_type);
        g_assert_false(primary_window(provider)->has_resets_at);
        codexbar_provider_free(provider);
    }
}

static void test_invalid_quota_inputs(void) {
    const char *missing[] = {"missing-quota.xml", "wrong-component.xml", "empty-value.xml"};
    for (guint index = 0; index < G_N_ELEMENTS(missing); index++) {
        GError *error = NULL;
        CodexBarProvider *provider = parse_fixture(missing[index], NULL, 0, &error);
        g_assert_null(provider);
        g_assert_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND);
        g_clear_error(&error);
    }
    GError *error = NULL;
    CodexBarProvider *provider = parse_fixture("invalid-quota.xml", NULL, 0, &error);
    g_assert_null(provider);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
    g_clear_error(&error);

    const char malformed[] = "<application><component>";
    provider = codexbar_jetbrains_parse_xml(malformed, sizeof(malformed) - 1, NULL, 0, &error);
    g_assert_null(provider);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
    g_clear_error(&error);

}

static void test_percentage_clamping_and_reset_boundaries(void) {
    const char over_limit[] =
        "<application><component name='AIAssistantQuotaManager2'>"
        "<option name='quotaInfo' value='{&quot;current&quot;:&quot;150&quot;,&quot;maximum&quot;:&quot;100&quot;}'/>"
        "</component></application>";
    GError *error = NULL;
    CodexBarProvider *provider =
        codexbar_jetbrains_parse_xml(over_limit, sizeof(over_limit) - 1, NULL, 0, &error);
    g_assert_no_error(error);
    g_assert_cmpfloat(primary_window(provider)->used_percent, ==, 100.0);
    codexbar_provider_free(provider);

    const char zero_limit[] =
        "<application><component name='AIAssistantQuotaManager2'>"
        "<option name='quotaInfo' value='{&quot;current&quot;:&quot;20&quot;,&quot;maximum&quot;:&quot;0&quot;}'/>"
        "</component></application>";
    provider = codexbar_jetbrains_parse_xml(zero_limit, sizeof(zero_limit) - 1, NULL, 0, &error);
    g_assert_no_error(error);
    g_assert_cmpfloat(primary_window(provider)->used_percent, ==, 0.0);
    codexbar_provider_free(provider);
}

static void test_reset_boundaries(void) {
    const struct {
        const char *next_refill;
        const char *expected;
    } cases[] = {
        {"2030-01-15T11:59:00Z", "Expired"},
        {"2030-01-15T12:00:00Z", "Expired"},
        {"2030-01-15T12:00:30Z", "Resets in 0m"},
        {"2030-01-16T12:00:00Z", "Resets in 24h 0m"},
        {"2030-01-17T13:00:00Z", "Resets in 2d 1h"},
    };
    gint64 now_ms = timestamp_ms("2030-01-15T12:00:00Z");
    for (guint index = 0; index < G_N_ELEMENTS(cases); index++) {
        char *xml = g_strdup_printf(
            "<application><component name='AIAssistantQuotaManager2'>"
            "<option name='quotaInfo' value='{&quot;current&quot;:&quot;0&quot;,&quot;maximum&quot;:&quot;100&quot;}'/>"
            "<option name='nextRefill' value='{&quot;next&quot;:&quot;%s&quot;}'/>"
            "</component></application>",
            cases[index].next_refill);
        GError *error = NULL;
        CodexBarProvider *provider = codexbar_jetbrains_parse_xml(xml, strlen(xml), NULL, now_ms, &error);
        g_assert_no_error(error);
        g_assert_cmpstr(primary_window(provider)->reset_description, ==, cases[index].expected);
        codexbar_provider_free(provider);
        g_free(xml);
    }
}

static void test_strict_json(void) {
    const char xml[] =
        "<application><component name='AIAssistantQuotaManager2'>"
        "<option name='quotaInfo' value='{&quot;current&quot;:&quot;1&quot;,&quot;maximum&quot;:&quot;2&quot;} trailing'/>"
        "</component></application>";
    GError *error = NULL;
    CodexBarProvider *provider = codexbar_jetbrains_parse_xml(xml, sizeof(xml) - 1, NULL, 0, &error);
    g_assert_null(provider);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
    g_clear_error(&error);
}

static char *write_quota(
    const char *home, const char *root, const char *ide, const char *fixture, time_t modification_time) {
    char *directory = g_build_filename(home, root, ide, "options", NULL);
    g_assert_cmpint(g_mkdir_with_parents(directory, 0700), ==, 0);
    char *path = g_build_filename(directory, "AIAssistantQuotaManager2.xml", NULL);
    gsize length = 0;
    char *contents = load_fixture(fixture, &length);
    GError *error = NULL;
    g_assert_true(g_file_set_contents(path, contents, (gssize)length, &error));
    g_assert_no_error(error);
    struct utimbuf times = {.actime = modification_time, .modtime = modification_time};
    g_assert_cmpint(g_utime(path, &times), ==, 0);
    g_free(contents);
    g_free(directory);
    return path;
}

static void remove_quota_tree(const char *home, const char *quota_path) {
    g_assert_cmpint(g_remove(quota_path), ==, 0);
    char *directory = g_path_get_dirname(quota_path);
    while (!g_str_equal(directory, home)) {
        g_rmdir(directory);
        char *parent = g_path_get_dirname(directory);
        g_free(directory);
        directory = parent;
    }
    g_free(directory);
}

static void test_discovery_roots(void) {
    struct {
        const char *root;
        const char *ide;
        const char *organization;
    } cases[] = {
        {".config/JetBrains", "intellijidea2025.1", "IntelliJ IDEA 2025.1"},
        {".local/share/JetBrains", "PyCharm2025.2", "PyCharm 2025.2"},
        {".config/Google", "AndroidStudio2025.3", "Android Studio 2025.3"},
    };
    for (guint index = 0; index < G_N_ELEMENTS(cases); index++) {
        GError *error = NULL;
        char *home = g_dir_make_tmp("codexbar-jetbrains-XXXXXX", &error);
        g_assert_no_error(error);
        char *path = write_quota(home, cases[index].root, cases[index].ide, "quota-full.xml", 100 + index);
        CodexBarProvider *provider = codexbar_jetbrains_fetch_from_home(home, 0, &error);
        g_assert_no_error(error);
        g_assert_nonnull(provider);
        g_assert_cmpstr(provider->identity->organization, ==, cases[index].organization);
        codexbar_provider_free(provider);
        remove_quota_tree(home, path);
        g_assert_cmpint(g_rmdir(home), ==, 0);
        g_free(path);
        g_free(home);
    }
}

static void test_discovery_uses_latest_quota_file(void) {
    GError *error = NULL;
    char *home = g_dir_make_tmp("codexbar-jetbrains-XXXXXX", &error);
    g_assert_no_error(error);
    char *older = write_quota(home, ".config/JetBrains", "WebStorm2024.1", "quota-full.xml", 100);
    char *newer = write_quota(home, ".config/JetBrains", "GoLand2025.2", "quota-only.xml", 200);
    CodexBarProvider *provider = codexbar_jetbrains_fetch_from_home(home, 0, &error);
    g_assert_no_error(error);
    g_assert_cmpstr(provider->identity->organization, ==, "GoLand 2025.2");
    g_assert_cmpfloat(primary_window(provider)->used_percent, ==, 5.0);
    codexbar_provider_free(provider);
    remove_quota_tree(home, older);
    remove_quota_tree(home, newer);
    g_assert_cmpint(g_rmdir(home), ==, 0);
    g_free(older);
    g_free(newer);
    g_free(home);
}

static void test_discovery_equal_mtime_uses_swift_order(void) {
    GError *error = NULL;
    char *home = g_dir_make_tmp("codexbar-jetbrains-XXXXXX", &error);
    g_assert_no_error(error);
    char *older = write_quota(home, ".config/JetBrains", "IntelliJIdea2025.1", "quota-only.xml", 100);
    char *newer = write_quota(home, ".config/JetBrains", "IntelliJIdea2025.2", "quota-full.xml", 100);
    CodexBarProvider *provider = codexbar_jetbrains_fetch_from_home(home, 0, &error);
    g_assert_no_error(error);
    g_assert_cmpstr(provider->identity->organization, ==, "IntelliJ IDEA 2025.2");
    codexbar_provider_free(provider);
    remove_quota_tree(home, older);
    remove_quota_tree(home, newer);
    g_assert_cmpint(g_rmdir(home), ==, 0);
    g_free(older);
    g_free(newer);
    g_free(home);
}

static void test_discovery_rejects_fifo(void) {
    GError *error = NULL;
    char *home = g_dir_make_tmp("codexbar-jetbrains-XXXXXX", &error);
    g_assert_no_error(error);
    char *directory = g_build_filename(home, ".config/JetBrains", "IntelliJIdea2025.2", "options", NULL);
    g_assert_cmpint(g_mkdir_with_parents(directory, 0700), ==, 0);
    char *path = g_build_filename(directory, "AIAssistantQuotaManager2.xml", NULL);
    g_assert_cmpint(mkfifo(path, 0600), ==, 0);
    CodexBarProvider *provider = codexbar_jetbrains_fetch_from_home(home, 0, &error);
    g_assert_null(provider);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
    g_clear_error(&error);
    remove_quota_tree(home, path);
    g_assert_cmpint(g_rmdir(home), ==, 0);
    g_free(path);
    g_free(directory);
    g_free(home);
}

static void test_discovery_rejects_oversized_file(void) {
    GError *error = NULL;
    char *home = g_dir_make_tmp("codexbar-jetbrains-XXXXXX", &error);
    g_assert_no_error(error);
    char *path = write_quota(home, ".config/JetBrains", "IntelliJIdea2025.2", "quota-full.xml", 100);
    gsize oversized_length = 4U * 1024U * 1024U + 1U;
    char *oversized = g_malloc0(oversized_length);
    g_assert_true(g_file_set_contents(path, oversized, (gssize)oversized_length, &error));
    g_assert_no_error(error);
    g_free(oversized);
    CodexBarProvider *provider = codexbar_jetbrains_fetch_from_home(home, 0, &error);
    g_assert_null(provider);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
    g_clear_error(&error);
    remove_quota_tree(home, path);
    g_assert_cmpint(g_rmdir(home), ==, 0);
    g_free(path);
    g_free(home);
}

static void test_discovery_follows_symlink(void) {
    GError *error = NULL;
    char *home = g_dir_make_tmp("codexbar-jetbrains-XXXXXX", &error);
    g_assert_no_error(error);
    char *directory = g_build_filename(home, ".config/JetBrains", "IntelliJIdea2025.2", "options", NULL);
    g_assert_cmpint(g_mkdir_with_parents(directory, 0700), ==, 0);
    char *target = g_build_filename(home, "quota.xml", NULL);
    char *contents = load_fixture("quota-full.xml", NULL);
    g_assert_true(g_file_set_contents(target, contents, -1, &error));
    g_assert_no_error(error);
    g_free(contents);
    char *path = g_build_filename(directory, "AIAssistantQuotaManager2.xml", NULL);
    GFile *link = g_file_new_for_path(path);
    g_assert_true(g_file_make_symbolic_link(link, target, NULL, &error));
    g_assert_no_error(error);
    g_object_unref(link);
    CodexBarProvider *provider = codexbar_jetbrains_fetch_from_home(home, 0, &error);
    g_assert_no_error(error);
    g_assert_nonnull(provider);
    g_assert_cmpfloat(primary_window(provider)->used_percent, ==, 25.0);
    codexbar_provider_free(provider);
    remove_quota_tree(home, path);
    g_assert_cmpint(g_remove(target), ==, 0);
    g_assert_cmpint(g_rmdir(home), ==, 0);
    g_free(path);
    g_free(target);
    g_free(directory);
    g_free(home);
}

static void test_discovery_reports_missing_ide(void) {
    GError *error = NULL;
    char *home = g_dir_make_tmp("codexbar-jetbrains-XXXXXX", &error);
    g_assert_no_error(error);
    CodexBarProvider *provider = codexbar_jetbrains_fetch_from_home(home, 0, &error);
    g_assert_null(provider);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND);
    g_clear_error(&error);
    g_assert_cmpint(g_rmdir(home), ==, 0);
    g_free(home);
}

int main(int argc, char **argv) {
    g_assert_cmpint(argc, ==, 2);
    fixture_directory = argv[1];
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/jetbrains/full-quota", test_full_quota);
    g_test_add_func("/jetbrains/supported-xml-variants", test_supported_xml_variants);
    g_test_add_func("/jetbrains/invalid-quota-inputs", test_invalid_quota_inputs);
    g_test_add_func("/jetbrains/percentage-clamping", test_percentage_clamping_and_reset_boundaries);
    g_test_add_func("/jetbrains/reset-boundaries", test_reset_boundaries);
    g_test_add_func("/jetbrains/strict-json", test_strict_json);
    g_test_add_func("/jetbrains/discovery-roots", test_discovery_roots);
    g_test_add_func("/jetbrains/discovery-latest", test_discovery_uses_latest_quota_file);
    g_test_add_func("/jetbrains/discovery-equal-mtime", test_discovery_equal_mtime_uses_swift_order);
    g_test_add_func("/jetbrains/discovery-fifo", test_discovery_rejects_fifo);
    g_test_add_func("/jetbrains/discovery-oversized", test_discovery_rejects_oversized_file);
    g_test_add_func("/jetbrains/discovery-symlink", test_discovery_follows_symlink);
    g_test_add_func("/jetbrains/discovery-missing", test_discovery_reports_missing_ide);
    return g_test_run();
}
