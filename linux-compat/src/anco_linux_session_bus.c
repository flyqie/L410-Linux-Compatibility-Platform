#include "anco_linux_session_bus.h"
#include <gio/gio.h>
#include <gio/gunixinputstream.h>
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static GSubprocess *owned_process;
static GSubprocess *tray_process;
static gboolean closing;
static GSourceFunc lost_callback;
static gpointer lost_data;
static gboolean exit_registered;

static void session_child_setup(gpointer data)
{
    (void)data;
    /* enter forwards TERM to the application's process group. The bus is a
     * session dependency and must survive until application watches drain. */
    if (setpgid(0, 0)) _exit(127);
}

static void child_exited(GObject *object, GAsyncResult *result, gpointer data)
{
    (void)data;
    GError *error = NULL;
    const gboolean waited = g_subprocess_wait_finish(G_SUBPROCESS(object), result, &error);
    if (owned_process != G_SUBPROCESS(object) || closing) {
        g_clear_error(&error); return;
    }
    printf("LINUX_SESSION_BUS_EXIT waited=%d status=%d unexpected=1 error=%s\n", waited,
        waited ? g_subprocess_get_status(owned_process) : -1, error ? error->message : "none");
    g_clear_error(&error);
    g_clear_object(&owned_process);
    g_unsetenv("DBUS_SESSION_BUS_ADDRESS");
    if (lost_callback) lost_callback(lost_data);
}

typedef struct { GMainLoop *loop; gboolean waited; GError *error; } StopWait;
static void stop_wait_done(GObject *object, GAsyncResult *result, gpointer data)
{
    StopWait *state = data;
    state->waited = g_subprocess_wait_finish(G_SUBPROCESS(object), result, &state->error);
    g_main_loop_quit(state->loop);
}
static gboolean stop_deadline(gpointer data)
{
    g_subprocess_force_exit(G_SUBPROCESS(data));
    return G_SOURCE_REMOVE;
}

static void stop_owned_process(GSubprocess **slot, const char *label)
{
    if (!*slot) return;
    GMainContext *context = g_main_context_new();
    StopWait state = {.loop = g_main_loop_new(context, FALSE)};
    GSource *deadline = g_timeout_source_new(3000);
    g_source_set_callback(deadline, stop_deadline, *slot, NULL);
    g_source_attach(deadline, context);
    g_main_context_push_thread_default(context);
    g_subprocess_wait_async(*slot, NULL, stop_wait_done, &state);
    g_subprocess_send_signal(*slot, SIGTERM);
    g_main_loop_run(state.loop);
    g_main_context_pop_thread_default(context);
    g_source_destroy(deadline); g_source_unref(deadline);
    printf("%s waited=%d status=%d error=%s\n", label, state.waited,
        state.waited ? g_subprocess_get_status(*slot) : -1,
        state.error ? state.error->message : "none");
    g_clear_error(&state.error);
    g_main_loop_unref(state.loop); g_main_context_unref(context);
    g_clear_object(slot);
}

void anco_linux_session_bus_stop(void)
{
    closing = TRUE;
    stop_owned_process(&tray_process, "LINUX_SNI_BRIDGE_STOP");
    if (owned_process) {
        /* GLib alone owns reaping. Its worker serializes safe signal sending
         * with process exit; never mix child-watch and manual waitpid/kill.
         * This private context dispatches only this wait and shutdown bound,
         * not already-retired Binder/Wayland callbacks. */
        GMainContext *context = g_main_context_new();
        StopWait state = {.loop = g_main_loop_new(context, FALSE)};
        GSource *deadline = g_timeout_source_new(3000);
        g_source_set_callback(deadline, stop_deadline, owned_process, NULL);
        g_source_attach(deadline, context);
        g_main_context_push_thread_default(context);
        g_subprocess_wait_async(owned_process, NULL, stop_wait_done, &state);
        g_subprocess_send_signal(owned_process, SIGTERM);
        g_main_loop_run(state.loop);
        g_main_context_pop_thread_default(context);
        g_source_destroy(deadline); g_source_unref(deadline);
        printf("LINUX_SESSION_BUS_STOP waited=%d status=%d error=%s\n", state.waited,
            state.waited ? g_subprocess_get_status(owned_process) : -1,
            state.error ? state.error->message : "none");
        g_clear_error(&state.error);
        g_main_loop_unref(state.loop); g_main_context_unref(context);
        g_clear_object(&owned_process);
    }
    lost_callback = NULL;
    lost_data = NULL;
    g_unsetenv("DBUS_SESSION_BUS_ADDRESS");
}

static void tray_exited(GObject *object, GAsyncResult *result, gpointer data)
{
    (void)data;
    GError *error = NULL;
    const gboolean waited = g_subprocess_wait_finish(G_SUBPROCESS(object), result, &error);
    if (tray_process == G_SUBPROCESS(object)) {
        printf("LINUX_SNI_BRIDGE_EXIT waited=%d status=%d stopping=%d error=%s\n",
            waited, waited ? g_subprocess_get_status(tray_process) : -1, closing,
            error ? error->message : "none");
        g_clear_object(&tray_process);
    }
    g_clear_error(&error);
}

int anco_linux_session_bus_start(const char *directory, const char *tray_directory,
                                 GSourceFunc lost, gpointer data)
{
    if (owned_process || tray_process || !directory || directory[0] != '/' ||
        !tray_directory || tray_directory[0] != '/' ||
        strpbrk(directory, ",;\n\r") || strpbrk(tray_directory, ",;\n\r") ||
        getuid() != 1000 || getgid() != 1000)
        return -EINVAL;
    if (!exit_registered) {
        if (atexit(anco_linux_session_bus_stop)) return -ENOMEM;
        exit_registered = TRUE;
    }
    char *expected = g_strdup_printf("unix:path=%s/bus", directory);
    char *option = g_strdup_printf("--address=%s", expected);
    char *argv[] = {"/usr/bin/dbus-daemon", "--session", "--nofork",
                    "--print-address=1", option, NULL};
    GError *error = NULL;
    GSubprocessLauncher *launcher = g_subprocess_launcher_new(G_SUBPROCESS_FLAGS_STDOUT_PIPE);
    g_subprocess_launcher_set_child_setup(launcher, session_child_setup, NULL, NULL);
    owned_process = g_subprocess_launcher_spawnv(launcher, (const gchar *const *)argv, &error);
    g_object_unref(launcher);
    g_free(option);
    if (!owned_process) {
        fprintf(stderr, "LINUX_SESSION_BUS_START_ERROR %s\n", error ? error->message : "spawn");
        g_clear_error(&error); g_free(expected); return -ENOEXEC;
    }
    closing = FALSE;
    GInputStream *stream = g_subprocess_get_stdout_pipe(owned_process);
    const int output = g_unix_input_stream_get_fd(G_UNIX_INPUT_STREAM(stream));
    char address[512]; size_t length = 0;
    const gint64 deadline = g_get_monotonic_time() + 5000000;
    gboolean complete = FALSE;
    while (length + 1 < sizeof(address)) {
        const gint64 remaining = deadline - g_get_monotonic_time();
        if (remaining <= 0) break;
        struct pollfd fd = {.fd = output, .events = POLLIN};
        int ready = poll(&fd, 1, (int)((remaining + 999) / 1000));
        if (ready < 0 && errno == EINTR) continue;
        if (ready <= 0) break;
        char byte; ssize_t count = read(output, &byte, 1);
        if (count < 0 && errno == EINTR) continue;
        if (count != 1) break;
        if (byte == '\n') { complete = TRUE; break; }
        address[length++] = byte;
    }
    g_input_stream_close(stream, NULL, NULL); address[length] = 0;
    const size_t expected_length = strlen(expected);
    const gboolean matched = complete && length >= expected_length &&
        !strncmp(address, expected, expected_length) &&
        (address[expected_length] == 0 || address[expected_length] == ',');
    g_free(expected);
    if (!matched) { anco_linux_session_bus_stop(); return -EPROTO; }
    GDBusConnection *connection = g_dbus_connection_new_for_address_sync(address,
        G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT |
        G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION, NULL, NULL, &error);
    GVariant *reply = connection ? g_dbus_connection_call_sync(connection,
        "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus",
        "ListNames", NULL, G_VARIANT_TYPE("(as)"), G_DBUS_CALL_FLAGS_NONE,
        2000, NULL, &error) : NULL;
    const gboolean authenticated = reply != NULL;
    if (reply) g_variant_unref(reply);
    if (connection) {
        g_dbus_connection_close_sync(connection, NULL, NULL);
        g_object_unref(connection);
    }
    if (!authenticated) {
        fprintf(stderr, "LINUX_SESSION_BUS_AUTH_ERROR %s\n", error ? error->message : "no reply");
        g_clear_error(&error); anco_linux_session_bus_stop(); return -ECONNREFUSED;
    }
    g_clear_error(&error);
    g_setenv("DBUS_SESSION_BUS_ADDRESS", address, TRUE);
    char *tray_argv[] = {"/usr/bin/python3", "/opt/l410/anco-runtime/sni_bridge.py",
                         (char *)tray_directory, NULL};
    launcher = g_subprocess_launcher_new(G_SUBPROCESS_FLAGS_NONE);
    g_subprocess_launcher_set_child_setup(launcher, session_child_setup, NULL, NULL);
    tray_process = g_subprocess_launcher_spawnv(launcher,
        (const gchar *const *)tray_argv, &error);
    g_object_unref(launcher);
    if (!tray_process) {
        fprintf(stderr, "LINUX_SNI_BRIDGE_START_ERROR %s\n",
            error ? error->message : "spawn");
        g_clear_error(&error); anco_linux_session_bus_stop(); return -ENOEXEC;
    }
    g_subprocess_wait_async(tray_process, NULL, tray_exited, NULL);
    printf("LINUX_SNI_BRIDGE_START pid=%s state=%s\n",
        g_subprocess_get_identifier(tray_process), tray_directory);
    lost_callback = lost; lost_data = data;
    g_subprocess_wait_async(owned_process, NULL, child_exited, NULL);
    const char *identifier = g_subprocess_get_identifier(owned_process);
    printf("LINUX_SESSION_BUS_READY pid=%s uid=%ld authenticated=1 private=1\n",
        identifier ? identifier : "retired", (long)getuid());
    return 0;
}
