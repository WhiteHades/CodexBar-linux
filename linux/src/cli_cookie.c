#include "cli_cookie.h"

#include "provider_registry.h"

#include <glib.h>
#include <stdio.h>

static int fail(const char *message) {
    fprintf(stderr, "%s\n", message);
    return 1;
}

int codexbar_cli_cookie_run(int argc, char **argv) {
    if (argc < 1 || !g_str_equal(argv[0], "refresh")) {
        return fail("Usage: codexbar-linux cookie refresh (--provider <name>|--all).");
    }
    const char *provider_name = NULL;
    gboolean all = FALSE;
    for (int index = 1; index < argc; index++) {
        if (g_str_equal(argv[index], "--provider")) {
            if (++index >= argc || provider_name) return fail("Missing or duplicate value for --provider.");
            provider_name = argv[index];
        } else if (g_str_equal(argv[index], "--all")) {
            if (all) return fail("Duplicate --all argument.");
            all = TRUE;
        } else if (g_str_equal(argv[index], "--json") || g_str_equal(argv[index], "--json-only") ||
                   g_str_equal(argv[index], "--pretty") || g_str_equal(argv[index], "--allow-keychain-prompt")) {
            continue;
        } else if (g_str_equal(argv[index], "--format")) {
            if (++index >= argc || (!g_str_equal(argv[index], "text") && !g_str_equal(argv[index], "json"))) {
                return fail("Invalid format. Use text or json.");
            }
        } else {
            return fail("Unknown cookie refresh argument.");
        }
    }
    if ((provider_name != NULL) == all) return fail("Specify exactly one of --provider <name> or --all.");
    if (provider_name) {
        char *lowercase = g_ascii_strdown(provider_name, -1);
        const CodexBarProviderDescriptor *provider = codexbar_provider_registry_find(lowercase);
        g_free(lowercase);
        if (!provider) return fail("Unknown provider.");
    }
    return fail("Cookie refresh is only supported on macOS.");
}
