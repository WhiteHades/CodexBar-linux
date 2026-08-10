#include <gio/gio.h>
#include <glib-unix.h>
#include <stdio.h>

static const char xml[] =
    "<node><interface name='org.kde.StatusNotifierWatcher'>"
    "<method name='RegisterStatusNotifierItem'><arg type='s' direction='in'/></method>"
    "</interface></node>";

typedef struct {
    GMainLoop *loop;
    const char *output;
    GDBusNodeInfo *introspection;
    guint object_id;
} Watcher;

static void method_call(GDBusConnection *connection,
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
    (void)method_name;
    Watcher *watcher = user_data;
    const char *service = NULL;
    g_variant_get(parameters, "(&s)", &service);
    GError *error = NULL;
    if (!g_file_set_contents(watcher->output, service, -1, &error)) {
        g_dbus_method_invocation_return_gerror(invocation, error);
        g_error_free(error);
        return;
    }
    g_dbus_method_invocation_return_value(invocation, NULL);
}

static const GDBusInterfaceVTable vtable = {.method_call = method_call};

static void bus_acquired(GDBusConnection *connection, const char *name, gpointer user_data) {
    (void)name;
    Watcher *watcher = user_data;
    GError *error = NULL;
    watcher->object_id = g_dbus_connection_register_object(connection,
                                                            "/StatusNotifierWatcher",
                                                            watcher->introspection->interfaces[0],
                                                            &vtable,
                                                            watcher,
                                                            NULL,
                                                            &error);
    if (watcher->object_id == 0) g_error("could not export test watcher: %s", error->message);
}

static gboolean stop(gpointer user_data) {
    g_main_loop_quit(user_data);
    return G_SOURCE_REMOVE;
}

int main(int argc, char **argv) {
    if (argc != 2) return 2;
    Watcher watcher = {.loop = g_main_loop_new(NULL, FALSE), .output = argv[1]};
    GError *error = NULL;
    watcher.introspection = g_dbus_node_info_new_for_xml(xml, &error);
    if (!watcher.introspection) g_error("invalid watcher introspection: %s", error->message);
    guint owner = g_bus_own_name(G_BUS_TYPE_SESSION,
                                 "org.kde.StatusNotifierWatcher",
                                 G_BUS_NAME_OWNER_FLAGS_NONE,
                                 bus_acquired,
                                 NULL,
                                 NULL,
                                 &watcher,
                                 NULL);
    g_unix_signal_add(SIGTERM, stop, watcher.loop);
    g_unix_signal_add(SIGINT, stop, watcher.loop);
    g_main_loop_run(watcher.loop);
    g_bus_unown_name(owner);
    g_dbus_node_info_unref(watcher.introspection);
    g_main_loop_unref(watcher.loop);
    return 0;
}
