#include "desktop.h"

#include "backend.h"
#include "model.h"
#include "render.h"

#include <gio/gio.h>
#include <glib-unix.h>
#include <json-c/json.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define CODEXBAR_STATUS_ITEM_PATH "/StatusNotifierItem"
#define CODEXBAR_REFRESH_SECONDS 60U
#define CODEXBAR_ICON_SIZE 32

typedef enum {
    CODEXBAR_TERMINAL_NOT_FOUND,
    CODEXBAR_TERMINAL_LAUNCHED,
    CODEXBAR_TERMINAL_FAILED,
} CodexBarTerminalLaunchResult;

typedef struct {
    const char *program;
    const char *const *arguments;
    size_t argument_count;
} CodexBarTerminalCommand;

typedef struct {
    gint references;
    GMainLoop *loop;
    GDBusConnection *connection;
    GDBusNodeInfo *introspection;
    guint freedesktop_object;
    guint kde_object;
    guint freedesktop_watcher;
    guint kde_watcher;
    guint refresh_timer;
    char *service_name;
    char *program;
    char *tooltip;
    int percentage;
    char *status;
    gboolean refreshing;
    gboolean stopping;
} CodexBarStatusItem;

#define STATUS_ITEM_MEMBERS                                                                                         \
    "<property name='Category' type='s' access='read'/>"                                                           \
    "<property name='Id' type='s' access='read'/>"                                                                 \
    "<property name='Title' type='s' access='read'/>"                                                              \
    "<property name='Status' type='s' access='read'/>"                                                             \
    "<property name='WindowId' type='u' access='read'/>"                                                           \
    "<property name='IconName' type='s' access='read'/>"                                                           \
    "<property name='IconPixmap' type='a(iiay)' access='read'/>"                                                   \
    "<property name='IconThemePath' type='s' access='read'/>"                                                      \
    "<property name='OverlayIconName' type='s' access='read'/>"                                                    \
    "<property name='OverlayIconPixmap' type='a(iiay)' access='read'/>"                                            \
    "<property name='AttentionIconName' type='s' access='read'/>"                                                  \
    "<property name='AttentionIconPixmap' type='a(iiay)' access='read'/>"                                          \
    "<property name='AttentionMovieName' type='s' access='read'/>"                                                 \
    "<property name='ToolTip' type='(sa(iiay)ss)' access='read'/>"                                                 \
    "<property name='ItemIsMenu' type='b' access='read'/>"                                                         \
    "<property name='Menu' type='o' access='read'/>"                                                               \
    "<method name='ContextMenu'><arg name='x' type='i' direction='in'/><arg name='y' type='i' direction='in'/>"    \
    "</method>"                                                                                                    \
    "<method name='Activate'><arg name='x' type='i' direction='in'/><arg name='y' type='i' direction='in'/>"       \
    "</method>"                                                                                                    \
    "<method name='SecondaryActivate'><arg name='x' type='i' direction='in'/>"                                     \
    "<arg name='y' type='i' direction='in'/></method>"                                                             \
    "<method name='Scroll'><arg name='delta' type='i' direction='in'/>"                                            \
    "<arg name='orientation' type='s' direction='in'/></method>"                                                   \
    "<signal name='NewTitle'/>"                                                                                    \
    "<signal name='NewIcon'/>"                                                                                     \
    "<signal name='NewAttentionIcon'/>"                                                                            \
    "<signal name='NewOverlayIcon'/>"                                                                              \
    "<signal name='NewToolTip'/>"                                                                                  \
    "<signal name='NewStatus'><arg name='status' type='s'/></signal>"

static const char status_item_xml[] =
    "<node>"
    "<interface name='org.freedesktop.StatusNotifierItem'>" STATUS_ITEM_MEMBERS "</interface>"
    "<interface name='org.kde.StatusNotifierItem'>" STATUS_ITEM_MEMBERS "</interface>"
    "</node>";

#undef STATUS_ITEM_MEMBERS

static char *resolve_executable(const char *program) {
    char *executable = g_file_read_link("/proc/self/exe", NULL);
    if (executable) return executable;
    if (strchr(program, G_DIR_SEPARATOR)) return g_canonicalize_filename(program, NULL);
    return g_find_program_in_path(program);
}

static CodexBarTerminalLaunchResult try_terminal(
    const CodexBarTerminalCommand *command, const char *executable, GError **error) {
    char *terminal = g_find_program_in_path(command->program);
    if (!terminal) return CODEXBAR_TERMINAL_NOT_FOUND;

    char **arguments = g_new0(char *, command->argument_count + 4);
    arguments[0] = terminal;
    for (size_t index = 0; index < command->argument_count; index++) {
        arguments[index + 1] = g_strdup(command->arguments[index]);
    }
    arguments[command->argument_count + 1] = g_strdup(executable);
    arguments[command->argument_count + 2] = g_strdup("tui");
    gboolean launched = g_spawn_async(NULL,
                                      arguments,
                                      NULL,
                                      G_SPAWN_STDOUT_TO_DEV_NULL | G_SPAWN_STDERR_TO_DEV_NULL,
                                      NULL,
                                      NULL,
                                      NULL,
                                      error);
    g_strfreev(arguments);
    return launched ? CODEXBAR_TERMINAL_LAUNCHED : CODEXBAR_TERMINAL_FAILED;
}

gboolean codexbar_desktop_launch_tui(const char *program, GError **error) {
    static const char *const xdg_arguments[] = {
        "--app-id=com.steipete.codexbar",
        "--title=CodexBar",
        "-e",
    };
    static const char *const ghostty_arguments[] = {
        "--class=com.steipete.codexbar",
        "--title=CodexBar",
        "-e",
    };
    static const char *const alacritty_arguments[] = {
        "--class",
        "com.steipete.codexbar",
        "--title",
        "CodexBar",
        "-e",
    };
    static const char *const kitty_arguments[] = {
        "--class",
        "com.steipete.codexbar",
        "--title",
        "CodexBar",
    };
    static const char *const foot_arguments[] = {
        "--app-id=com.steipete.codexbar",
        "--title=CodexBar",
    };
    static const char *const wezterm_arguments[] = {
        "start",
        "--class",
        "com.steipete.codexbar",
        "--",
    };
    static const char *const gnome_terminal_arguments[] = {
        "--title=CodexBar",
        "--",
    };
    static const char *const konsole_arguments[] = {
        "--name",
        "com.steipete.codexbar",
        "-p",
        "tabtitle=CodexBar",
        "-e",
    };
    static const char *const xterm_arguments[] = {
        "-class",
        "CodexBar",
        "-title",
        "CodexBar",
        "-e",
    };
    static const CodexBarTerminalCommand terminals[] = {
        {"xdg-terminal-exec", xdg_arguments, G_N_ELEMENTS(xdg_arguments)},
        {"ghostty", ghostty_arguments, G_N_ELEMENTS(ghostty_arguments)},
        {"alacritty", alacritty_arguments, G_N_ELEMENTS(alacritty_arguments)},
        {"kitty", kitty_arguments, G_N_ELEMENTS(kitty_arguments)},
        {"foot", foot_arguments, G_N_ELEMENTS(foot_arguments)},
        {"wezterm", wezterm_arguments, G_N_ELEMENTS(wezterm_arguments)},
        {"gnome-terminal", gnome_terminal_arguments, G_N_ELEMENTS(gnome_terminal_arguments)},
        {"konsole", konsole_arguments, G_N_ELEMENTS(konsole_arguments)},
        {"xterm", xterm_arguments, G_N_ELEMENTS(xterm_arguments)},
    };

    char *executable = resolve_executable(program);
    if (!executable) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "cannot resolve the codexbar-linux executable");
        return FALSE;
    }
    for (size_t index = 0; index < G_N_ELEMENTS(terminals); index++) {
        CodexBarTerminalLaunchResult result = try_terminal(&terminals[index], executable, error);
        if (result == CODEXBAR_TERMINAL_LAUNCHED) {
            g_free(executable);
            return TRUE;
        }
        if (result == CODEXBAR_TERMINAL_FAILED) {
            g_free(executable);
            return FALSE;
        }
    }
    g_free(executable);
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "no supported terminal emulator was found");
    return FALSE;
}

static CodexBarStatusItem *status_item_ref(CodexBarStatusItem *item) {
    g_atomic_int_inc(&item->references);
    return item;
}

static void status_item_free(CodexBarStatusItem *item) {
    g_clear_pointer(&item->loop, g_main_loop_unref);
    g_clear_object(&item->connection);
    g_clear_pointer(&item->introspection, g_dbus_node_info_unref);
    g_free(item->service_name);
    g_free(item->program);
    g_free(item->tooltip);
    g_free(item->status);
    g_free(item);
}

static void status_item_unref(CodexBarStatusItem *item) {
    if (g_atomic_int_dec_and_test(&item->references)) status_item_free(item);
}

static GVariant *empty_pixmaps(void) {
    GVariantBuilder builder;
    g_variant_builder_init(&builder, G_VARIANT_TYPE("a(iiay)"));
    return g_variant_builder_end(&builder);
}

static const guint8 digit_rows[10][5] = {
    {7, 5, 5, 5, 7},
    {2, 6, 2, 2, 7},
    {7, 1, 7, 4, 7},
    {7, 1, 7, 1, 7},
    {5, 5, 7, 1, 1},
    {7, 4, 7, 1, 7},
    {7, 4, 7, 5, 7},
    {7, 1, 1, 1, 1},
    {7, 5, 7, 5, 7},
    {7, 5, 7, 1, 7},
};

static void set_pixel(guint8 *pixels, int x, int y, guint8 red, guint8 green, guint8 blue, guint8 alpha) {
    if (x < 0 || x >= CODEXBAR_ICON_SIZE || y < 0 || y >= CODEXBAR_ICON_SIZE) return;
    size_t offset = ((size_t)y * CODEXBAR_ICON_SIZE + (size_t)x) * 4;
    pixels[offset] = alpha;
    pixels[offset + 1] = red;
    pixels[offset + 2] = green;
    pixels[offset + 3] = blue;
}

static GVariant *icon_pixmap(int percentage) {
    guint8 pixels[CODEXBAR_ICON_SIZE * CODEXBAR_ICON_SIZE * 4] = {0};
    guint8 red = percentage >= 90 ? 243 : percentage >= 70 ? 249 : 137;
    guint8 green = percentage >= 90 ? 139 : percentage >= 70 ? 226 : 180;
    guint8 blue = percentage >= 90 ? 168 : percentage >= 70 ? 175 : 250;
    for (int y = 0; y < CODEXBAR_ICON_SIZE; y++) {
        for (int x = 0; x < CODEXBAR_ICON_SIZE; x++) {
            int dx = x * 2 - (CODEXBAR_ICON_SIZE - 1);
            int dy = y * 2 - (CODEXBAR_ICON_SIZE - 1);
            int radius = dx * dx + dy * dy;
            if (radius >= 650 && radius <= 930) set_pixel(pixels, x, y, red, green, blue, 255);
        }
    }

    char text[4];
    g_snprintf(text, sizeof(text), "%d", CLAMP(percentage, 0, 100));
    int length = (int)strlen(text);
    int scale = 2;
    int width = length * 3 * scale + (length - 1) * scale;
    int start_x = (CODEXBAR_ICON_SIZE - width) / 2;
    int start_y = (CODEXBAR_ICON_SIZE - 5 * scale) / 2;
    for (int character = 0; character < length; character++) {
        int digit = text[character] - '0';
        int origin_x = start_x + character * 4 * scale;
        for (int row = 0; row < 5; row++) {
            for (int column = 0; column < 3; column++) {
                if ((digit_rows[digit][row] & (1U << (2 - column))) == 0) continue;
                for (int py = 0; py < scale; py++) {
                    for (int px = 0; px < scale; px++) {
                        set_pixel(pixels, origin_x + column * scale + px, start_y + row * scale + py, 255, 255, 255, 255);
                    }
                }
            }
        }
    }

    GVariantBuilder builder;
    g_variant_builder_init(&builder, G_VARIANT_TYPE("a(iiay)"));
    GVariant *bytes = g_variant_new_fixed_array(G_VARIANT_TYPE_BYTE, pixels, sizeof(pixels), sizeof(guint8));
    g_variant_builder_add(&builder, "(ii@ay)", CODEXBAR_ICON_SIZE, CODEXBAR_ICON_SIZE, bytes);
    return g_variant_builder_end(&builder);
}

static GVariant *status_item_property(GDBusConnection *connection,
                                      const char *sender,
                                      const char *object_path,
                                      const char *interface_name,
                                      const char *property_name,
                                      GError **error,
                                      gpointer user_data) {
    (void)connection;
    (void)sender;
    (void)object_path;
    (void)interface_name;
    (void)error;
    CodexBarStatusItem *item = user_data;
    if (g_str_equal(property_name, "Category")) return g_variant_new_string("ApplicationStatus");
    if (g_str_equal(property_name, "Id")) return g_variant_new_string("CodexBar");
    if (g_str_equal(property_name, "Title")) {
        char *title = g_strdup_printf("CodexBar %d%%", item->percentage);
        GVariant *value = g_variant_new_take_string(title);
        return value;
    }
    if (g_str_equal(property_name, "Status")) return g_variant_new_string(item->status);
    if (g_str_equal(property_name, "WindowId")) return g_variant_new_uint32(0);
    if (g_str_equal(property_name, "IconName") || g_str_equal(property_name, "IconThemePath") ||
        g_str_equal(property_name, "OverlayIconName") || g_str_equal(property_name, "AttentionIconName") ||
        g_str_equal(property_name, "AttentionMovieName")) {
        return g_variant_new_string("");
    }
    if (g_str_equal(property_name, "IconPixmap")) return icon_pixmap(item->percentage);
    if (g_str_equal(property_name, "OverlayIconPixmap") || g_str_equal(property_name, "AttentionIconPixmap")) {
        return empty_pixmaps();
    }
    if (g_str_equal(property_name, "ToolTip")) {
        char *title = g_strdup_printf("CodexBar %d%%", item->percentage);
        GVariant *value = g_variant_new("(s@a(iiay)ss)", "", empty_pixmaps(), title, item->tooltip);
        g_free(title);
        return value;
    }
    if (g_str_equal(property_name, "ItemIsMenu")) return g_variant_new_boolean(FALSE);
    if (g_str_equal(property_name, "Menu")) return g_variant_new_object_path("/");
    return NULL;
}

static void emit_update(CodexBarStatusItem *item) {
    static const char *const interfaces[] = {
        "org.freedesktop.StatusNotifierItem",
        "org.kde.StatusNotifierItem",
    };
    for (size_t index = 0; index < G_N_ELEMENTS(interfaces); index++) {
        g_dbus_connection_emit_signal(
            item->connection, NULL, CODEXBAR_STATUS_ITEM_PATH, interfaces[index], "NewTitle", NULL, NULL);
        g_dbus_connection_emit_signal(
            item->connection, NULL, CODEXBAR_STATUS_ITEM_PATH, interfaces[index], "NewIcon", NULL, NULL);
        g_dbus_connection_emit_signal(
            item->connection, NULL, CODEXBAR_STATUS_ITEM_PATH, interfaces[index], "NewToolTip", NULL, NULL);
        g_dbus_connection_emit_signal(item->connection,
                                      NULL,
                                      CODEXBAR_STATUS_ITEM_PATH,
                                      interfaces[index],
                                      "NewStatus",
                                      g_variant_new("(s)", item->status),
                                      NULL);
    }
}

static void set_error_state(CodexBarStatusItem *item, const char *message) {
    g_free(item->tooltip);
    item->tooltip = g_strdup_printf("CodexBar\n%s", message ? message : "usage refresh failed");
    item->percentage = 0;
    g_free(item->status);
    item->status = g_strdup("NeedsAttention");
    emit_update(item);
}

static void set_snapshot_state(CodexBarStatusItem *item, const CodexBarSnapshot *snapshot) {
    char *rendered = codexbar_render_waybar(snapshot);
    json_object *root = json_tokener_parse(rendered);
    json_object *tooltip = NULL;
    json_object *percentage = NULL;
    json_object *class_name = NULL;
    gboolean valid = root && json_object_get_type(root) == json_type_object &&
                     json_object_object_get_ex(root, "tooltip", &tooltip) &&
                     json_object_get_type(tooltip) == json_type_string &&
                     json_object_object_get_ex(root, "percentage", &percentage) &&
                     json_object_get_type(percentage) == json_type_int;
    if (!valid) {
        if (root) json_object_put(root);
        g_free(rendered);
        set_error_state(item, "usage output is invalid");
        return;
    }
    g_free(item->tooltip);
    item->tooltip = g_strdup(json_object_get_string(tooltip));
    item->percentage = CLAMP(json_object_get_int(percentage), 0, 100);
    gboolean has_class = json_object_object_get_ex(root, "class", &class_name) &&
                         json_object_get_type(class_name) == json_type_string;
    const char *class_text = has_class ? json_object_get_string(class_name) : "";
    g_free(item->status);
    item->status = g_strdup(g_str_equal(class_text, "critical") || g_str_equal(class_text, "error")
                                ? "NeedsAttention"
                                : "Active");
    json_object_put(root);
    g_free(rendered);
    emit_update(item);
}

static void refresh_worker(GTask *task, gpointer source_object, gpointer task_data, GCancellable *cancellable) {
    (void)source_object;
    (void)task_data;
    (void)cancellable;
    GError *error = NULL;
    CodexBarSnapshot *snapshot = codexbar_backend_fetch(&error);
    if (snapshot) {
        g_task_return_pointer(task, snapshot, (GDestroyNotify)codexbar_snapshot_free);
    } else {
        g_task_return_error(task, error);
    }
}

static void refresh_complete(GObject *source_object, GAsyncResult *result, gpointer user_data) {
    (void)source_object;
    CodexBarStatusItem *item = user_data;
    GError *error = NULL;
    CodexBarSnapshot *snapshot = g_task_propagate_pointer(G_TASK(result), &error);
    item->refreshing = FALSE;
    if (!item->stopping) {
        if (snapshot) {
            set_snapshot_state(item, snapshot);
        } else {
            set_error_state(item, error ? error->message : "usage refresh failed");
        }
    }
    if (snapshot) codexbar_snapshot_free(snapshot);
    g_clear_error(&error);
    status_item_unref(item);
}

static gboolean start_refresh(gpointer user_data) {
    CodexBarStatusItem *item = user_data;
    if (item->stopping || item->refreshing) return G_SOURCE_CONTINUE;
    item->refreshing = TRUE;
    GTask *task = g_task_new(NULL, NULL, refresh_complete, status_item_ref(item));
    g_task_run_in_thread(task, refresh_worker);
    g_object_unref(task);
    return G_SOURCE_CONTINUE;
}

static void status_item_method(GDBusConnection *connection,
                               const char *sender,
                               const char *object_path,
                               const char *interface_name,
                               const char *method_name,
                               GVariant *parameters,
                               GDBusMethodInvocation *invocation,
                               gpointer user_data) {
    (void)connection;
    (void)sender;
    (void)object_path;
    (void)interface_name;
    (void)parameters;
    CodexBarStatusItem *item = user_data;
    if (g_str_equal(method_name, "Activate")) {
        GError *error = NULL;
        if (!codexbar_desktop_launch_tui(item->program, &error)) {
            g_dbus_method_invocation_return_gerror(invocation, error);
            g_error_free(error);
            return;
        }
    } else if (g_str_equal(method_name, "ContextMenu") || g_str_equal(method_name, "SecondaryActivate")) {
        start_refresh(item);
    }
    g_dbus_method_invocation_return_value(invocation, NULL);
}

static const GDBusInterfaceVTable status_item_vtable = {
    .method_call = status_item_method,
    .get_property = status_item_property,
};

static void watcher_appeared(
    GDBusConnection *connection, const char *name, const char *name_owner, gpointer user_data) {
    (void)name_owner;
    CodexBarStatusItem *item = user_data;
    const char *interface_name = g_str_equal(name, "org.kde.StatusNotifierWatcher")
                                     ? "org.kde.StatusNotifierWatcher"
                                     : "org.freedesktop.StatusNotifierWatcher";
    g_dbus_connection_call(connection,
                           name,
                           "/StatusNotifierWatcher",
                           interface_name,
                           "RegisterStatusNotifierItem",
                           g_variant_new("(s)", item->service_name),
                           NULL,
                           G_DBUS_CALL_FLAGS_NONE,
                           -1,
                           NULL,
                           NULL,
                           NULL);
}

static gboolean request_bus_name(CodexBarStatusItem *item, GError **error) {
    GVariant *reply = g_dbus_connection_call_sync(item->connection,
                                                  "org.freedesktop.DBus",
                                                  "/org/freedesktop/DBus",
                                                  "org.freedesktop.DBus",
                                                  "RequestName",
                                                  g_variant_new("(su)", item->service_name, 4U),
                                                  G_VARIANT_TYPE("(u)"),
                                                  G_DBUS_CALL_FLAGS_NONE,
                                                  -1,
                                                  NULL,
                                                  error);
    if (!reply) return FALSE;
    guint32 result = 0;
    g_variant_get(reply, "(u)", &result);
    g_variant_unref(reply);
    if (result == 1U || result == 4U) return TRUE;
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_EXISTS, "CodexBar status item name is already in use");
    return FALSE;
}

static gboolean register_status_item(CodexBarStatusItem *item, GError **error) {
    item->introspection = g_dbus_node_info_new_for_xml(status_item_xml, error);
    if (!item->introspection) return FALSE;
    GDBusInterfaceInfo *freedesktop =
        g_dbus_node_info_lookup_interface(item->introspection, "org.freedesktop.StatusNotifierItem");
    GDBusInterfaceInfo *kde = g_dbus_node_info_lookup_interface(item->introspection, "org.kde.StatusNotifierItem");
    item->freedesktop_object = g_dbus_connection_register_object(
        item->connection, CODEXBAR_STATUS_ITEM_PATH, freedesktop, &status_item_vtable, item, NULL, error);
    if (item->freedesktop_object == 0) return FALSE;
    item->kde_object = g_dbus_connection_register_object(
        item->connection, CODEXBAR_STATUS_ITEM_PATH, kde, &status_item_vtable, item, NULL, error);
    if (item->kde_object == 0) return FALSE;
    item->freedesktop_watcher = g_bus_watch_name_on_connection(item->connection,
                                                               "org.freedesktop.StatusNotifierWatcher",
                                                               G_BUS_NAME_WATCHER_FLAGS_NONE,
                                                               watcher_appeared,
                                                               NULL,
                                                               item,
                                                               NULL);
    item->kde_watcher = g_bus_watch_name_on_connection(item->connection,
                                                       "org.kde.StatusNotifierWatcher",
                                                       G_BUS_NAME_WATCHER_FLAGS_NONE,
                                                       watcher_appeared,
                                                       NULL,
                                                       item,
                                                       NULL);
    return TRUE;
}

static gboolean stop_status_item(gpointer user_data) {
    CodexBarStatusItem *item = user_data;
    item->stopping = TRUE;
    g_main_loop_quit(item->loop);
    return G_SOURCE_REMOVE;
}

static void status_item_shutdown(CodexBarStatusItem *item) {
    item->stopping = TRUE;
    if (item->refresh_timer) g_source_remove(item->refresh_timer);
    if (item->freedesktop_watcher) g_bus_unwatch_name(item->freedesktop_watcher);
    if (item->kde_watcher) g_bus_unwatch_name(item->kde_watcher);
    if (item->freedesktop_object) {
        g_dbus_connection_unregister_object(item->connection, item->freedesktop_object);
    }
    if (item->kde_object) g_dbus_connection_unregister_object(item->connection, item->kde_object);
}

int codexbar_status_item_run(const char *program) {
    CodexBarStatusItem *item = g_new0(CodexBarStatusItem, 1);
    item->references = 1;
    item->program = g_strdup(program);
    item->service_name =
        g_strdup_printf("org.freedesktop.StatusNotifierItem-%" G_GINT64_FORMAT "-1", (gint64)getpid());
    item->tooltip = g_strdup("CodexBar\nLoading usage...");
    item->status = g_strdup("Active");
    item->loop = g_main_loop_new(NULL, FALSE);
    GError *error = NULL;
    item->connection = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &error);
    if (!item->connection || !request_bus_name(item, &error) || !register_status_item(item, &error)) {
        fprintf(stderr, "Cannot start the CodexBar status item: %s\n", error ? error->message : "unknown error");
        g_clear_error(&error);
        status_item_shutdown(item);
        status_item_unref(item);
        return 1;
    }
    g_unix_signal_add(SIGINT, stop_status_item, item);
    g_unix_signal_add(SIGTERM, stop_status_item, item);
    start_refresh(item);
    item->refresh_timer = g_timeout_add_seconds(CODEXBAR_REFRESH_SECONDS, start_refresh, item);
    g_main_loop_run(item->loop);
    status_item_shutdown(item);
    status_item_unref(item);
    return 0;
}
