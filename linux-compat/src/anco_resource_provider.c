#define _GNU_SOURCE
#include "anco_resource_agent.h"
#include "gbinder_ipc.h"

#include <errno.h>
#include <fcntl.h>
#include <glib-unix.h>
#include <glib.h>
#include <grp.h>
#include <signal.h>
#include <stdio.h>
#include <sys/types.h>
#include <unistd.h>

static gboolean stop_provider(gpointer data)
{
    g_main_loop_quit(data);
    return G_SOURCE_REMOVE;
}

int main(int argc, char **argv)
{
    if (argc != 2 || argv[1][0] != '/') {
        fprintf(stderr, "usage: %s ICON_DIRECTORY\n", argv[0]);
        return 64;
    }
    setvbuf(stdout, NULL, _IOLBF, 0);
    const int icon_fd = open(argv[1], O_RDONLY | O_DIRECTORY);
    if (icon_fd < 0) {
        perror("resource provider icon directory");
        return 65;
    }
    char icon_directory[64];
    if (snprintf(icon_directory, sizeof(icon_directory),
                 "/proc/self/fd/%d", icon_fd) <= 0) {
        close(icon_fd);
        return 65;
    }
    /* Keep the original guest application uid while retaining only the
     * resource-service group. The read-only directory descriptor preserves
     * access to the ShellAssistant-owned icon directory without widening any
     * application-data directory permissions or copying icons elsewhere. */
    if (getuid() != 0 || setgroups(0, NULL) || setgid(5557) || setuid(1000)) {
        perror("resource provider identity");
        close(icon_fd);
        return 65;
    }
    GBinderIpc *ipc = gbinder_ipc_new("/dev/binder", "aidl2");
    if (!ipc) {
        close(icon_fd);
        return 66;
    }
    AncoResourceAgent *agent = anco_resource_agent_new(ipc, icon_directory);
    if (!agent) {
        gbinder_ipc_unref(ipc);
        close(icon_fd);
        return 67;
    }
    const int registered = anco_resource_agent_register(agent);
    printf("RESOURCE_PROVIDER_STANDALONE_REGISTER id=66894 result=%d original_interface=1 uid=%lu gid=%lu\n",
        registered, (unsigned long)getuid(), (unsigned long)getgid());
    if (registered) {
        anco_resource_agent_free(agent);
        gbinder_ipc_unref(ipc);
        close(icon_fd);
        return 68;
    }
    GMainLoop *loop = g_main_loop_new(NULL, FALSE);
    const guint term = g_unix_signal_add(SIGTERM, stop_provider, loop);
    const guint interrupt = g_unix_signal_add(SIGINT, stop_provider, loop);
    g_main_loop_run(loop);
    g_source_remove(term);
    g_source_remove(interrupt);
    const int retired = anco_resource_agent_free(agent);
    printf("RESOURCE_PROVIDER_STANDALONE_RETIRE id=66894 result=%d\n", retired);
    g_main_loop_unref(loop);
    gbinder_ipc_unref(ipc);
    close(icon_fd);
    return retired ? 69 : 0;
}
