/* Native ArkUI control host.
 *
 * A Linux process cannot link the HarmonyOS ArkUI toolkit: it is a different
 * libc and a different ABI. What it can do is ask for controls. This module is
 * the receiving end of that request: it owns the window's XComponent container
 * and turns a small widget protocol into real ArkUI components through the node
 * API. The controls are ordinary ArkUI controls in an ordinary HarmonyOS
 * window; the Linux side never draws anything, it only describes its widget
 * tree and receives events.
 *
 * Every window is a session. A session is created when the page mounts its
 * container and asks native code to start it; it gets its own root node, its
 * own listening socket (SOCKET_PREFIX + "-" + index) and its own command queue,
 * so several Linux applications can each have their own window at the same
 * time. Commands are queued by the session's socket thread and applied on a
 * thread that carries the ArkUI container scope (the page's tick call).
 *
 * Wire protocol, one command per line, text only:
 *
 *   hello <name>                  identify the client
 *   clear                         drop the current tree
 *   node <id> <type> <parent>     type: col row text button input, parent 0=root
 *   set <id> <attr> <value>       attr: text label ph w h font pad bg fg
 *   attach                        make the built tree visible in the window
 *   ping                          liveness check
 *   quit                          detach and close
 *
 *   ready session <n>             host -> client, container is usable
 *   ok <what> [args]              host -> client
 *   err <what> <code>             host -> client
 *   click <id>                    host -> client
 *   change <id> <text>            host -> client
 */
#define _GNU_SOURCE
#include <node_api.h>
#include <napi/native_api.h>
#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <signal.h>
#include <sys/un.h>
#include <unistd.h>
#include <fcntl.h>
#include "native_interface_xcomponent.h"
typedef struct OH_PixelmapNative OH_PixelmapNative;
#include "native_node.h"
#include "ui_input_event.h"
#include "native_interface.h"

#define MARK_PATH "/data/storage/el2/base/files/ui-host.log"
#define MARK_PATH_STATE "/data/storage/el2/base/files/ui-sessions"
#define SOCKET_PREFIX "l410-native-ui"
#define MANAGER_SOCKET "l410-manager"
#define MAX_SESSIONS 4
#define MAX_NODES 1024
#define QUEUE_DEPTH 4096
#define HOST_LINE_MAX 8192

static void host_mark(const char *text) {
    int fd = open(MARK_PATH, O_WRONLY | O_APPEND | O_CREAT, 0644);
    if (fd < 0) return;
    write(fd, text, strlen(text));
    write(fd, "\n", 1);
    close(fd);
}

static void markf(const char *format, ...) {
    char line[320];
    va_list args;
    va_start(args, format);
    vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    host_mark(line);
}

/* Small numeric parsers: the cross toolchain's glibc headers redirect strtol and
 * friends to __isoc23_* symbols, which the device's libc does not have, and an
 * unresolved symbol here faults at the call site. */
static int parse_int(const char *text) {
    int sign = 1, value = 0;
    if (!text) return 0;
    while (*text == ' ') text++;
    if (*text == '-') { sign = -1; text++; }
    else if (*text == '+') text++;
    while (*text >= '0' && *text <= '9') value = value * 10 + (*text++ - '0');
    return sign * value;
}

static unsigned int parse_uint(const char *text) {
    unsigned int value = 0;
    if (!text) return 0;
    while (*text == ' ') text++;
    if (text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
        text += 2;
        for (;; text++) {
            if (*text >= '0' && *text <= '9') value = value * 16 + (unsigned)(*text - '0');
            else if (*text >= 'a' && *text <= 'f') value = value * 16 + 10u + (unsigned)(*text - 'a');
            else if (*text >= 'A' && *text <= 'F') value = value * 16 + 10u + (unsigned)(*text - 'A');
            else break;
        }
        return value;
    }
    while (*text >= '0' && *text <= '9') value = value * 10 + (unsigned)(*text++ - '0');
    return value;
}

static float parse_float(const char *text) {
    float sign = 1.0f, value = 0.0f;
    if (!text) return 0.0f;
    while (*text == ' ') text++;
    if (*text == '-') { sign = -1.0f; text++; }
    else if (*text == '+') text++;
    while (*text >= '0' && *text <= '9') value = value * 10.0f + (float)(*text++ - '0');
    if (*text == '.') {
        text++;
        float scale = 0.1f;
        while (*text >= '0' && *text <= '9') {
            value += (float)(*text++ - '0') * scale;
            scale *= 0.1f;
        }
    }
    return sign * value;
}

/* ------------------------------------------------------------------ sessions */

struct session {
    int index;
    int used;          /* window live: a container is bound and a root attached */
    int reserved;      /* a page has taken this number but has not started yet */
    int attached;
    OH_NativeXComponent *component;
    ArkUI_NodeHandle root;
    ArkUI_NodeHandle nodes[MAX_NODES];
    int client_fd;
    pthread_mutex_t client_lock;
    pthread_mutex_t queue_lock;
    char *queue[QUEUE_DEPTH];
    int queue_head, queue_tail;
    char socket_name[64];
    char client_name[128];
    struct node_box {
        int x, y, w, h, kind;
    } boxes[MAX_NODES];
    ArkUI_ContextHandle context;
    pthread_t driver;
    int driver_running;
    int drive_stop;
    int drive_reported;
    int heartbeats;
    float tap_x, tap_y;
    int tap_have;
};

/* The library is loaded once per module scope, and a page may end up calling
 * the tick of a different load than the one that owns the socket and the node
 * tree. Keeping the session state in a shared mapping makes both loads see the
 * same sessions, so whichever tick runs can apply the queued commands. */
static int instance_marker;   /* one per loaded copy of this library */

struct state_header {
    int owner_pid;
    int reserved;
};

static struct session *shared_sessions = NULL;

static struct session *host_sessions(void) {
    if (!shared_sessions) {
        const size_t size = sizeof(struct state_header) + sizeof(struct session) * MAX_SESSIONS;
        int fd = open("/data/storage/el2/base/files/l410-host-state", O_RDWR | O_CREAT, 0666);
        if (fd >= 0) {
            if (ftruncate(fd, (off_t)size) == 0) {
                void *base = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
                if (base != MAP_FAILED)
                    shared_sessions = (struct session *)((char *)base + sizeof(struct state_header));
            }
            close(fd);
        }
        if (!shared_sessions) {
            markf("HOST_STATE_FALLBACK private");
            static struct session private_sessions[MAX_SESSIONS];
            shared_sessions = private_sessions;
        } else {
            /* The mapping outlives a run; only the process that created it may
             * start from a clean slate, otherwise a second load in the same
             * process would wipe the first one's sessions. */
            struct state_header *header = (struct state_header *)shared_sessions;
            if (header->owner_pid != (int)getpid()) {
                memset(shared_sessions, 0, size);
                header->owner_pid = (int)getpid();
                markf("HOST_STATE_RESET pid=%d", (int)getpid());
            }
            int first = 0;
            for (int i = 0; i < MAX_SESSIONS; i++) if (shared_sessions[i].used) first++;
            markf("HOST_STATE_SHARED pid=%d base=%p live=%d marker=%p", (int)getpid(), (void *)shared_sessions,
                  first, (void *)&instance_marker);
        }
    }
    return shared_sessions;
}

#define sessions host_sessions()
static ArkUI_NativeNodeAPI_1 *node_api;
static OH_NativeXComponent *first_component;
static char first_component_id[128];

static void write_state_file(void);

static void set_number(ArkUI_NodeHandle node, ArkUI_NodeAttributeType attribute, float value) {
    ArkUI_NumberValue number[] = {{.f32 = value}};
    ArkUI_AttributeItem item = {number, 1, NULL, NULL};
    node_api->setAttribute(node, attribute, &item);
}

static void set_bool(ArkUI_NodeHandle node, ArkUI_NodeAttributeType attribute, int32_t value) {
    ArkUI_NumberValue number[] = {{.i32 = value}};
    ArkUI_AttributeItem item = {number, 1, NULL, NULL};
    node_api->setAttribute(node, attribute, &item);
}

static void set_color(ArkUI_NodeHandle node, ArkUI_NodeAttributeType attribute, uint32_t argb) {
    ArkUI_NumberValue number[] = {{.u32 = argb}};
    ArkUI_AttributeItem item = {number, 1, NULL, NULL};
    node_api->setAttribute(node, attribute, &item);
}

static void set_text(ArkUI_NodeHandle node, ArkUI_NodeAttributeType attribute, const char *value) {
    ArkUI_AttributeItem item = {NULL, 0, value, 0};
    node_api->setAttribute(node, attribute, &item);
}

/* ---------------------------------------------------------------- client io */

static void client_send(struct session *session, const char *format, ...) {
    /* A control message to a client that has gone away raises SIGPIPE, which
     * would take the whole application down with it. */
    signal(SIGPIPE, SIG_IGN);
    char line[HOST_LINE_MAX];
    va_list args;
    va_start(args, format);
    int length = vsnprintf(line, sizeof(line) - 2, format, args);
    va_end(args);
    if (length < 0) return;
    if (length > (int)sizeof(line) - 2) length = (int)sizeof(line) - 2;
    line[length++] = '\n';
    pthread_mutex_lock(&session->client_lock);
    if (session->client_fd >= 0) {
        ssize_t written = write(session->client_fd, line, (size_t)length);
        (void)written;
    }
    pthread_mutex_unlock(&session->client_lock);
}

/* ------------------------------------------------------------- node lookups */

static ArkUI_NodeHandle node_of(struct session *session, int id) {
    if (id < 0 || id >= MAX_NODES) return NULL;
    return session->nodes[id];
}

/* Values travel on one protocol line, so a client may escape newlines as \n and
 * backslashes as \\ (control text such as a document body needs that). */
static void unescape_in_place(char *value) {
    char *read = value, *write = value;
    while (*read) {
        if (*read == '\\' && read[1] == 'n') { *write++ = '\n'; read += 2; }
        else if (*read == '\\' && read[1] == 'r') { *write++ = '\r'; read += 2; }
        else if (*read == '\\' && read[1] == '\\') { *write++ = '\\'; read += 2; }
        else *write++ = *read++;
    }
    *write = 0;
}

static int32_t node_type_from(const char *type) {
    if (!strcmp(type, "col")) return ARKUI_NODE_COLUMN;
    if (!strcmp(type, "row")) return ARKUI_NODE_ROW;
    if (!strcmp(type, "text")) return ARKUI_NODE_TEXT;
    if (!strcmp(type, "button")) return ARKUI_NODE_BUTTON;
    if (!strcmp(type, "input")) return ARKUI_NODE_TEXT_INPUT;
    if (!strcmp(type, "area")) return ARKUI_NODE_TEXT_AREA;
    if (!strcmp(type, "stack")) return ARKUI_NODE_STACK;
    return -1;
}

/* Absolute placement inside a stack: what a mirrored application window needs,
 * because a toolkit lays its widgets out by coordinates, not by flow. */
static int32_t set_position(ArkUI_NodeHandle node, float x, float y) {
    ArkUI_NumberValue value[] = {{.f32 = x}, {.f32 = y}};
    ArkUI_AttributeItem item = {value, 2, NULL, NULL};
    return node_api->setAttribute(node, NODE_POSITION, &item);
}

/* Change events carry no readable string on this build, so the text of an input
 * or a text area is read back from the control itself. */
static const char *node_text_of(ArkUI_NodeHandle node) {
    const ArkUI_AttributeItem *item = node_api->getAttribute(node, NODE_TEXT_INPUT_TEXT);
    if (!item || !item->string) item = node_api->getAttribute(node, NODE_TEXT_AREA_TEXT);
    return (item && item->string) ? item->string : "";
}

static void on_node_event(ArkUI_NodeEvent *event) {
    int32_t kind = OH_ArkUI_NodeEvent_GetEventType(event);
    ArkUI_NodeHandle node = OH_ArkUI_NodeEvent_GetNodeHandle(event);
    if (!node) return;
    for (int s = 0; s < MAX_SESSIONS; s++) {
        if (!sessions[s].used) continue;
        if (node == sessions[s].root) {
            markf("HOST_EVENT root type=%d", kind);
            return;
        }
        for (int i = 0; i < MAX_NODES; i++) {
            if (sessions[s].nodes[i] != node) continue;
            if (kind == NODE_TOUCH_EVENT) {
                ArkUI_UIInputEvent *input = OH_ArkUI_NodeEvent_GetInputEvent(event);
                markf("HOST_TOUCH session=%d id=%d action=%d x=%.0f y=%.0f", sessions[s].index, i,
                      input ? OH_ArkUI_UIInputEvent_GetAction(input) : -1,
                      input ? OH_ArkUI_PointerEvent_GetX(input) : -1.0f,
                      input ? OH_ArkUI_PointerEvent_GetY(input) : -1.0f);
            } else if (kind == NODE_ON_CLICK) {
                markf("HOST_EVENT click session=%d id=%d", sessions[s].index, i);
                client_send(&sessions[s], "click %d", i);
            } else if (kind == NODE_TEXT_INPUT_ON_CHANGE || kind == NODE_TEXT_AREA_ON_CHANGE) {
                /* The change event itself carries no readable string on this
                 * build (OH_ArkUI_NodeEvent_GetStringValue answers 106108), so
                 * read the control's text back instead. */
                const char *value = node_text_of(node);
                markf("HOST_EVENT change session=%d id=%d len=%d", sessions[s].index, i, (int)strlen(value));
                client_send(&sessions[s], "change %d %s", i, value);
            }
            return;
        }
    }
}

/* ----------------------------------------------------------- command apply */

static void drop_tree(struct session *session) {
    markf("HOST_DROP_TREE session=%d", session->index);
    if (session->root) {
        if (session->attached && session->component) {
            OH_NativeXComponent_DetachNativeRootNode(session->component, session->root);
        }
        node_api->disposeNode(session->root);
        session->root = NULL;
        session->attached = 0;
    }
    for (int i = 0; i < MAX_NODES; i++) session->nodes[i] = NULL;
}

/* The page's JavaScript timer is suspended by the platform as soon as its
 * window is not the active one, which leaves queued commands unapplied. The
 * queue is therefore drained by a native driver too: a thread posts a task to
 * the UI thread and the task applies a few commands, with or without a timer. */
static void apply_line(struct session *session, char *line);

static void drain_on_ui_thread(void *raw) {
    struct session *session = (struct session *)raw;
    int applied = 0;
    for (int step = 0; step < 4; step++) {
        char *line = NULL;
        pthread_mutex_lock(&session->queue_lock);
        if (session->queue_head != session->queue_tail) {
            line = session->queue[session->queue_head];
            session->queue[session->queue_head] = NULL;
            session->queue_head = (session->queue_head + 1) % QUEUE_DEPTH;
        }
        pthread_mutex_unlock(&session->queue_lock);
        if (!line) break;
        markf("HOST_APPLY s%d %.70s", session->index, line);
        apply_line(session, line);
        free(line);
        applied++;
    }
    if (applied) markf("HOST_DRIVE s%d applied=%d", session->index, applied);
}

static void ui_heartbeat(void *raw) {
    struct session *session = (struct session *)raw;
    session->heartbeats++;
    markf("HOST_UI_ALIVE s%d beat=%d", session->index, session->heartbeats);
}

/* ------------------------------------------------------------- tap harness --
 * Device level input injection is inert on this hardware, so the same code path
 * a real touch would take can be driven from a file instead: the driver thread
 * watches a small file and applies a hit test plus the resulting click or
 * focus, exactly like l410UiHostTouch does for a finger.
 */
static int hit_test(struct session *session, float x, float y);

#define TAP_FILE "/data/storage/el2/base/files/ui-tap.txt"

static void apply_tap(struct session *session, const char *text, float fx, float fy) {
    float x = fx, y = fy;
    if (text && *text) {
        /* This shared object is built without libc startup, and the formatted
         * calls (atoi/snprintf/strchr) faulted when this ran on the framework's
         * UI thread. Everything here is plain pointer arithmetic instead. */
        long value = 0;
        const char *cursor = text;
        int digits_only = 1;
        while (*cursor) {
            if (*cursor >= '0' && *cursor <= '9') value = value * 10 + (*cursor - '0');
            else digits_only = 0;
            cursor++;
        }
        if (digits_only) {
            /* Buttons take a click, everything else takes focus: the kind is a
             * plain array read, safe here now that no libc call is involved. */
            const int kind = (value > 0 && value < MAX_NODES) ? session->boxes[value].kind : 0;
            const char *verb = (kind == ARKUI_NODE_BUTTON) ? "click " : "focus ";
            char message[24];
            int length = 0;
            while (*verb) message[length++] = *verb++;
            char reversed[12];
            int count = 0;
            if (value == 0) reversed[count++] = '0';
            while (value > 0) { reversed[count++] = (char)('0' + (value % 10)); value /= 10; }
            while (count > 0) message[length++] = reversed[--count];
            message[length++] = '\n';
            const int fd = session ? session->client_fd : -1;
            if (fd >= 0) {
                const ssize_t wrote = write(fd, message, (size_t)length);
                (void)wrote;
            }
            return;
        }
    }
    const int hit = hit_test(session, x, y);
    markf("HOST_TAP x=%d y=%d id=%d", (int)x, (int)y, hit);
    if (hit > 0) {
        const int kind = session->boxes[hit].kind;
        client_send(session, kind == ARKUI_NODE_BUTTON ? "click %d" : "focus %d", hit);
    }
}

/* A tap asked for by the page (which relays it from the management service):
 * same hit test and same click/focus as a real touch. */
static void apply_tap_on_ui(void *raw);

static napi_value host_tap(napi_env env, napi_callback_info info) {
    size_t argc = 2;
    napi_value argv[2] = {NULL, NULL};
    napi_get_cb_info(env, info, &argc, argv, NULL, NULL);
    double x = 0.0, y = 0.0;
    if (argc >= 1 && argv[0]) napi_get_value_double(env, argv[0], &x);
    if (argc >= 2 && argv[1]) napi_get_value_double(env, argv[1], &y);
    markf("HOST_TAP_API_ENTER x=%d y=%d", (int)x, (int)y);
    for (int i = 0; i < MAX_SESSIONS; i++) {
        struct session *session = &sessions[i];
        if (!session->attached) continue;
        /* Hand the tap to the UI thread instead of applying it here: the tree may
         * only be touched there, and this is the mechanism the rest of the host
         * already uses for work that belongs to that thread. */
        session->tap_x = (float)x;
        session->tap_y = (float)y;
        session->tap_have = 1;
        if (session->context) OH_ArkUI_PostUITask(session->context, session, apply_tap_on_ui);
        else session->tap_have = 0;
        break;
    }
    return NULL;
}

static void poll_tap_file(struct session *session) {
    static int announced = 0;
    if (!announced) {
        /* Prove the harness is reached at all, and that the file can be created
         * inside the application's own sandbox. */
        const int created = open(TAP_FILE, O_WRONLY | O_CREAT | O_APPEND, 0666);
        const int create_errno = errno;
        if (created >= 0) close(created);
        markf("HOST_TAP_READY create_errno=%d", create_errno);
        announced = 1;
    }
    int fd = open(TAP_FILE, O_RDONLY);
    if (fd < 0) {
        static int reported = 0;
        if (reported < 3) { reported++; markf("HOST_TAP_OPEN_FAIL errno=%d", errno); }
        return;
    }
    char text[64];
    const ssize_t got = read(fd, text, sizeof(text) - 1);
    close(fd);
    /* Only consume the file once it actually carried a command: deleting the
     * empty file the harness itself creates made every later read fail. */
    if (got <= 0) return;
    unlink(TAP_FILE);
    text[got] = 0;
    char *newline = strchr(text, '\n');
    if (newline) *newline = 0;
    apply_tap(session, text, 0.0f, 0.0f);
}

/* Ask the management service whether anyone queued a tap. The page is not
 * involved: this process may talk to the service directly, and a tap is applied
 * through exactly the same hit test a real touch uses. */
static void apply_tap_on_ui(void *raw);

static void apply_tap_on_ui(void *raw) {
    struct session *session = (struct session *)raw;
    if (!session || !session->tap_have) return;
    session->tap_have = 0;
    const float x = session->tap_x;
    const float y = session->tap_y;
    if (y < 0.0f) {
        /* A negative y means "x is a control id". Everything below is plain
         * pointer arithmetic: this object is built without libc startup and the
         * formatted calls faulted when entered from the framework's UI thread. */
        long value = (long)x;
        /* A tap asked for by id is meant as a press: the client turns it into a
         * button click, or into a focus change for anything else. */
        const int kind = 0;
        const char *verb = "click ";
        char message[24];
        int length = 0;
        while (*verb) message[length++] = *verb++;
        char reversed[12];
        int count = 0;
        if (value == 0) reversed[count++] = '0';
        while (value > 0) { reversed[count++] = (char)('0' + (value % 10)); value /= 10; }
        while (count > 0) message[length++] = reversed[--count];
        message[length++] = '\n';
        /* client_send() is the path every control command already uses, and those
         * reach the client (its replies prove it), so the click goes that way. */
        int delivered = 0;
        for (int i = 0; i < MAX_SESSIONS; i++) {
            if (!sessions[i].attached || sessions[i].client_fd < 0) continue;
            client_send(&sessions[i], "click %d", (int)x);
            delivered++;
            markf("HOST_TAP_SENT id=%d session=%d client=%s fd=%d",
                  (int)x, i, sessions[i].client_name, sessions[i].client_fd);
        }
        if (!delivered) markf("HOST_TAP_SENT id=%d nobody=%d", (int)x, delivered);
    } else {
        const int hit = hit_test(session, x, y);
        if (hit > 0) {
            const int kind = session->boxes[hit].kind;
            client_send(session, kind == ARKUI_NODE_BUTTON ? "click %d" : "focus %d", hit);
        }
    }
}

static void poll_manager_tap(struct session *session) {
    static int calls = 0;
    calls++;
    const int loud = (calls < 200);
    if (!session->attached) {
        if (loud) markf("HOST_TAP_TRY n=%d skipped unattached", calls);
        return;
    }
    const int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0);
    if (fd < 0) { if (loud) markf("HOST_TAP_TRY n=%d socket errno=%d", calls, errno); return; }
    struct timeval timeout;
    timeout.tv_sec = 0;
    timeout.tv_usec = 200000;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    struct sockaddr_un address;
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    static const char name[] = "\0l410-manager";
    const size_t name_length = sizeof(name) - 1;
    memcpy(address.sun_path, name, name_length);
    if (connect(fd, (struct sockaddr *)&address, sizeof(sa_family_t) + name_length) != 0) {
        const int connect_errno = errno;
        close(fd);
        if (loud) markf("HOST_TAP_TRY n=%d connect errno=%d", calls, connect_errno);
        return;
    }
    const char request[] = "{\"path\":\"/ui/tap-pending\",\"method\":\"POST\",\"body\":{}}\n";
    const ssize_t sent = write(fd, request, sizeof(request) - 1);
    if (sent <= 0) { close(fd); if (loud) markf("HOST_TAP_TRY n=%d write=%d errno=%d", calls, (int)sent, errno); return; }
    usleep(20000);
    char line[1024];
    const ssize_t got = read(fd, line, sizeof(line) - 1);
    close(fd);
    if (got <= 0) { if (loud) markf("HOST_TAP_TRY n=%d read=%d errno=%d", calls, (int)got, errno); return; }
    line[got] = 0;
    if (loud) markf("HOST_TAP_TRY n=%d reply=%.48s", calls, line);
    if (strstr(line, "\"tap\": null") || strstr(line, "\"tap\":null")) return;
    const char *x_key = strstr(line, "\"x\"");
    const char *y_key = strstr(line, "\"y\"");
    if (!x_key || !y_key) { markf("HOST_TAP_KEYS_MISSING %.32s", line); return; }
    while (*x_key && (*x_key < '0' || *x_key > '9') && *x_key != '-') x_key++;
    while (*y_key && (*y_key < '0' || *y_key > '9') && *y_key != '-') y_key++;
    const float x = (float)strtod(x_key, NULL);
    const float y = (float)strtod(y_key, NULL);
    markf("HOST_TAP_PARSED x=%d y=%d", (int)x, (int)y);
    session->tap_x = x;
    session->tap_y = y;
    session->tap_have = 1;
    if (session->context) OH_ArkUI_PostUITask(session->context, session, apply_tap_on_ui);
    else session->tap_have = 0;
}

static void *tap_thread_main(void *unused) {
    (void)unused;
    markf("HOST_TAP_THREAD_START");
    for (;;) {
        sleep(3600);
    }
    return NULL;
}

static void start_tap_thread(void) {
    static int started = 0;
    if (started) return;
    started = 1;
    pthread_t thread;
    if (pthread_create(&thread, NULL, tap_thread_main, NULL) == 0) pthread_detach(thread);
}

/* The same channel as a tap, for text: a native input control hands the typed
 * text over, and it is delivered to the Linux side as a "change" event. */
static void poll_manager_type(struct session *session) {
    if (!session->attached) return;
    const int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0);
    if (fd < 0) return;
    struct timeval timeout;
    timeout.tv_sec = 0;
    timeout.tv_usec = 200000;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    struct sockaddr_un address;
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    static const char name[] = "\0l410-manager";
    const size_t name_length = sizeof(name) - 1;
    memcpy(address.sun_path, name, name_length);
    if (connect(fd, (struct sockaddr *)&address, sizeof(sa_family_t) + name_length) != 0) {
        close(fd);
        return;
    }
    const char request[] = "{\"path\":\"/ui/type-pending\",\"method\":\"POST\",\"body\":{}}\n";
    if (write(fd, request, sizeof(request) - 1) <= 0) { close(fd); return; }
    usleep(20000);
    char line[2048];
    const ssize_t got = read(fd, line, sizeof(line) - 1);
    close(fd);
    if (got <= 0) return;
    line[got] = 0;
    if (strstr(line, "\"change\": null") || strstr(line, "\"change\":null")) return;
    const char *id_key = strstr(line, "\"id\"");
    const char *text_key = strstr(line, "\"text\"");
    if (!id_key || !text_key) return;
    while (*id_key && (*id_key < '0' || *id_key > '9')) id_key++;
    int wanted = 0;
    while (*id_key >= '0' && *id_key <= '9') { wanted = wanted * 10 + (*id_key - '0'); id_key++; }
    text_key += 6;
    while (*text_key == ' ' || *text_key == ':' || *text_key == '"') text_key++;
    char text[512];
    int length = 0;
    while (*text_key && *text_key != '"' && length < (int)sizeof(text) - 1) text[length++] = *text_key++;
    text[length] = 0;
    if (!length) return;
    /* Hand built for the same reason as the tap message: this object is linked
     * without libc startup, and a formatted call here faults the driver thread. */
    char message[600];
    int used = 0;
    const char *verb = "change ";
    while (*verb && used < (int)sizeof(message) - 1) message[used++] = *verb++;
    char digits[12];
    int count = 0;
    int value = wanted;
    if (value == 0) digits[count++] = '0';
    while (value > 0) { digits[count++] = (char)('0' + (value % 10)); value /= 10; }
    while (count > 0 && used < (int)sizeof(message) - 1) message[used++] = digits[--count];
    if (used < (int)sizeof(message) - 1) message[used++] = ' ';
    for (int i = 0; i < length && used < (int)sizeof(message) - 2; i++) message[used++] = text[i];
    message[used++] = '\n';
    int delivered = 0;
    for (int i = 0; i < MAX_SESSIONS; i++) {
        if (!sessions[i].attached || sessions[i].client_fd < 0) continue;
        const ssize_t wrote = write(sessions[i].client_fd, message, (size_t)used);
        if (wrote > 0) delivered++;
    }
    markf("HOST_TYPE_SENT id=%d chars=%d delivered=%d", wanted, length, delivered);
}

static void *driver_thread(void *raw) {
    struct session *session = (struct session *)raw;
    markf("HOST_DRIVER_START s%d", session->index);
    int rounds = 0;
    while (!session->drive_stop) {
        usleep(100000);
        if (!session->context) continue;
        rounds++;
        /* Every half second also ask the UI thread to report that it is alive:
         * this is what tells a suspended UI thread apart from an empty queue. */
        if (rounds % 25 == 0) OH_ArkUI_PostUITask(session->context, session, ui_heartbeat);
        if (rounds % 100 == 0) markf("HOST_DRIVER_ALIVE s%d rounds=%d", session->index, rounds);
        poll_tap_file(session);
        if (rounds % 10 == 0) poll_manager_tap(session);
        if (rounds % 10 == 3) poll_manager_type(session);
        int32_t rc = OH_ArkUI_PostUITask(session->context, session, drain_on_ui_thread);
        if (rc != 0 && session->drive_reported < 3) {
            session->drive_reported++;
            markf("HOST_DRIVE_POST_FAIL s%d rc=%d", session->index, rc);
        }
    }
    markf("HOST_DRIVER_STOP s%d", session->index);
    return NULL;
}

/* The attached control tree does not take part in the framework's own hit
 * testing, so a touch that the page hands over is matched against the stored
 * boxes here and turned into a click for the Linux side. */
static int hit_test(struct session *session, float x, float y);

static int hit_test(struct session *session, float x, float y) {
    for (int i = MAX_NODES - 1; i > 0; i--) {
        const struct node_box *box = &session->boxes[i];
        if (!session->nodes[i] || box->w <= 0 || box->h <= 0) continue;
        if (box->kind != ARKUI_NODE_BUTTON && box->kind != ARKUI_NODE_TEXT_INPUT &&
            box->kind != ARKUI_NODE_TEXT_AREA)
            continue;
        if (x >= box->x && x <= box->x + box->w && y >= box->y && y <= box->y + box->h) return i;
    }
    return -1;
}

/* The component itself is asked for touches: a node type XComponent swallows
 * pointer input before the page ever sees it, so the only place left to learn
 * about a press is the native component callback. */
static void on_xc_touch(OH_NativeXComponent *component, void *window) {
    OH_NativeXComponent_TouchEvent event;
    memset(&event, 0, sizeof(event));
    if (OH_NativeXComponent_GetTouchEvent(component, window, &event) != 0) return;
    char id[64] = {0};
    uint64_t size = sizeof(id) - 1;
    if (OH_NativeXComponent_GetXComponentId(component, id, &size) != 0) id[0] = 0;

    for (int s = 0; s < MAX_SESSIONS; s++) {
        if (!sessions[s].used) continue;
        if (strcmp(sessions[s].socket_name, id) != 0) continue;   /* not ours */
        markf("HOST_XC_TOUCH s%d type=%d x=%.0f y=%.0f", s + 1, (int)event.type, event.x, event.y);
        if (event.type == OH_NATIVEXCOMPONENT_DOWN) {
            int hit = hit_test(&sessions[s], event.x, event.y);
            markf("HOST_XC_HIT s%d hit=%d", s + 1, hit);
            if (hit > 0 && sessions[s].boxes[hit].kind == ARKUI_NODE_BUTTON)
                client_send(&sessions[s], "click %d", hit);
        }
        break;
    }
}

static void apply_line(struct session *session, char *line) {
    char *save = NULL;
    char *command = strtok_r(line, " \t", &save);
    if (!command) return;

    if (!strcmp(command, "hello")) {
        const char *name = strtok_r(NULL, " \t", &save);
        markf("HOST_HELLO session=%d %s", session->index, name ? name : "(unnamed)");
        /* A window belongs to one application: when another client takes it, the
         * previous tree goes away, otherwise two applications would share one
         * window and their controls would be mixed together. */
        if (session->root && session->client_name[0]) {
            int live_nodes = 0;
            for (int i = 0; i < MAX_NODES; i++) if (session->nodes[i]) live_nodes++;
            markf("HOST_TAKEOVER session=%d from=%s nodes_live=%d", session->index, session->client_name,
                  live_nodes);
            if (session->attached && session->component) {
                OH_NativeXComponent_DetachNativeRootNode(session->component, session->root);
            }
            node_api->disposeNode(session->root);
            session->root = node_api->createNode(ARKUI_NODE_COLUMN);
            set_text(session->root, NODE_ID, session->socket_name);
            set_number(session->root, NODE_WIDTH, 1000);
            set_number(session->root, NODE_HEIGHT, 640);
            set_number(session->root, NODE_PADDING, 8);
            set_color(session->root, NODE_BACKGROUND_COLOR, 0xFFF7F8FA);
            session->attached = 0;
            if (session->component) {
                OH_NativeXComponent_AttachNativeRootNode(session->component, session->root);
                session->attached = OH_ArkUI_NodeUtils_GetParentInPageTree(session->root) != NULL;
            }
            for (int i = 0; i < MAX_NODES; i++) session->nodes[i] = NULL;
        }
        snprintf(session->client_name, sizeof(session->client_name), "%s", name ? name : "");
        session->used = 1;
        session->reserved = 0;
        write_state_file();
        client_send(session, "ok hello");
        return;
    }
    if (!strcmp(command, "ping")) { client_send(session, "ok ping"); return; }
    if (!strcmp(command, "quit")) {
        drop_tree(session);
        client_send(session, "ok quit");
        return;
    }
    if (!strcmp(command, "clear")) {
        drop_tree(session);
        client_send(session, "ok clear");
        return;
    }
    if (!strcmp(command, "node")) {
        char *id_text = strtok_r(NULL, " \t", &save);
        char *type_text = strtok_r(NULL, " \t", &save);
        char *parent_text = strtok_r(NULL, " \t", &save);
        if (!id_text || !type_text || !parent_text) { client_send(session, "err node 1"); markf("HOST_ERR_NODE 1 session=%d", session->index); return; }
        int id = parse_int(id_text);
        int parent = parse_int(parent_text);
        int32_t type = node_type_from(type_text);
        if (id <= 0 || id >= MAX_NODES || type < 0) { client_send(session, "err node 2"); markf("HOST_ERR_NODE 2 session=%d", session->index); return; }
        if (session->nodes[id]) { client_send(session, "err node 3"); markf("HOST_ERR_NODE 3 session=%d", session->index); return; }
        ArkUI_NodeHandle handle = node_api->createNode((ArkUI_NodeType)type);
        if (!handle) { client_send(session, "err node 4"); markf("HOST_ERR_NODE 4 session=%d", session->index); return; }
        session->nodes[id] = handle;
        session->boxes[id].kind = type;
        ArkUI_NodeHandle parent_handle = (parent == 0) ? session->root : node_of(session, parent);
        if (parent_handle) node_api->addChild(parent_handle, handle);
        /* No empty strings here: setting an empty text on a freshly created
         * control is what the framework chokes on. Content arrives later, and
         * only when there is something to say. */
        /* Every control reports its clicks: the Linux side has to hear about a
         * press wherever it lands, and it also makes it visible when a click
         * reaches a container instead of the control inside it. */
        node_api->registerNodeEventReceiver(on_node_event);
        node_api->registerNodeEvent(handle, NODE_ON_CLICK, id, NULL);
        if (type == ARKUI_NODE_TEXT_INPUT || type == ARKUI_NODE_TEXT_AREA) {
            set_bool(handle, NODE_FOCUSABLE, 1);
            node_api->registerNodeEventReceiver(on_node_event);
            node_api->registerNodeEvent(handle, type == ARKUI_NODE_TEXT_AREA ? NODE_TEXT_AREA_ON_CHANGE
                                                                             : NODE_TEXT_INPUT_ON_CHANGE,
                                        id, NULL);
        }
        markf("HOST_NODE session=%d id=%d type=%s parent=%d handle=%p",
              session->index, id, type_text, parent, (void *)handle);
        client_send(session, "ok node %d", id);
        return;
    }
    if (!strcmp(command, "set")) {
        char *id_text = strtok_r(NULL, " \t", &save);
        char *attr = strtok_r(NULL, " \t", &save);
        char *value = strtok_r(NULL, "", &save);          /* rest of the line */
        while (value && *value == ' ') value++;
        if (!id_text || !attr) { client_send(session, "err set 1"); return; }
        ArkUI_NodeHandle handle = node_of(session, parse_int(id_text));
        if (!handle) { client_send(session, "err set 2"); return; }
        if (!value) value = (char *)"";
        unescape_in_place(value);
        /* Setting an empty string on a control is what the framework chokes on,
         * so an empty label or text simply leaves the control as it is. */
        if ((!strcmp(attr, "text") || !strcmp(attr, "label") || !strcmp(attr, "ph")) && value[0] == 0) {
            client_send(session, "ok set");
            return;
        }
        int node_id = parse_int(id_text);
        if (!strcmp(attr, "pos")) {
            const char *space = strchr(value, ' ');
            session->boxes[node_id].x = (int)parse_float(value);
            session->boxes[node_id].y = (int)(space ? parse_float(space + 1) : 0.0f);
        } else if (!strcmp(attr, "w")) {
            session->boxes[node_id].w = (int)parse_float(value);
        } else if (!strcmp(attr, "h")) {
            session->boxes[node_id].h = (int)parse_float(value);
        }
        if (!strcmp(attr, "text")) {
            /* The attribute belongs to the node type: sending a text area's
             * attribute to a plain text node takes the framework down. */
            const int kind = session->boxes[node_id].kind;
            if (kind == ARKUI_NODE_TEXT_AREA) set_text(handle, NODE_TEXT_AREA_TEXT, value);
            else set_text(handle, NODE_TEXT_CONTENT, value);
        }
        else if (!strcmp(attr, "label")) set_text(handle, NODE_BUTTON_LABEL, value);
        else if (!strcmp(attr, "ph")) set_text(handle, NODE_TEXT_INPUT_PLACEHOLDER, value);
        else if (!strcmp(attr, "w")) set_number(handle, NODE_WIDTH, parse_float(value));
        else if (!strcmp(attr, "h")) set_number(handle, NODE_HEIGHT, parse_float(value));
        else if (!strcmp(attr, "font")) set_number(handle, NODE_FONT_SIZE, parse_float(value));
        else if (!strcmp(attr, "pad")) set_number(handle, NODE_PADDING, parse_float(value));
        else if (!strcmp(attr, "pos")) {
            const char *space = strchr(value, ' ');
            float x = parse_float(value);
            float y = space ? parse_float(space + 1) : 0.0f;
            int32_t rc = set_position(handle, x, y);
            markf("HOST_POS id=%s x=%.0f y=%.0f rc=%d", id_text, x, y, rc);
        }
        else if (!strcmp(attr, "bg")) set_color(handle, NODE_BACKGROUND_COLOR, parse_uint(value));
        else if (!strcmp(attr, "fg")) set_color(handle, NODE_FONT_COLOR, parse_uint(value));
        else { client_send(session, "err set 3"); return; }
        client_send(session, "ok set %s", attr);
        return;
    }
    if (!strcmp(command, "attach")) {
        if (!session->root) { client_send(session, "err attach 1"); return; }
        if (!session->attached) {
            start_tap_thread();
            if (session->component) {
                int32_t rc = OH_NativeXComponent_AttachNativeRootNode(session->component, session->root);
                ArkUI_NodeHandle parent = OH_ArkUI_NodeUtils_GetParentInPageTree(session->root);
                session->attached = (parent != NULL);
                markf("HOST_ATTACH session=%d rc=%d parent=%p", session->index, rc, (void *)parent);
            } else {
                markf("HOST_ATTACH session=%d no-container", session->index);
            }
        }
        client_send(session, "ok attach %d", session->attached);
        return;
    }
    client_send(session, "err unknown 0");
}

/* The page calls this on the UI thread; nodes may only be touched there, and
 * only from a call that carries the ArkUI container scope, which a libuv
 * callback does not have (touching a node from one crashes inside createNode). */
static napi_value tick(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value argv[1] = {NULL};
    napi_get_cb_info(env, info, &argc, argv, NULL, NULL);
    int index = 1;
    if (argc >= 1 && argv[0]) {
        int32_t value = 0;
        if (napi_get_value_int32(env, argv[0], &value) == napi_ok && value > 0) index = value;
    }
    int applied = 0;
    static int tick_calls = 0;
    tick_calls++;
    /* Dense observation while the mirror is being built: what does this tick
     * see, and what does it manage to apply? */
    if (tick_calls <= 400 || tick_calls % 100 == 0) {
        if (index >= 1 && index <= MAX_SESSIONS) {
            struct session *probe = &sessions[index - 1];
            markf("HOST_TICK_BEFORE n=%d s%d used=%d fd=%d head=%d tail=%d", tick_calls, index, probe->used,
                  probe->client_fd, probe->queue_head, probe->queue_tail);
        }
    }
    if (tick_calls % 50 == 1) {
        for (int i = 0; i < MAX_SESSIONS; i++) {
            struct session *s = &sessions[i];
            int live = 0;
            for (int k = 0; k < MAX_NODES; k++) if (s->nodes[k]) live++;
            markf("HOST_TICK_STATE pid=%d marker=%p s%d used=%d head=%d tail=%d fd=%d attached=%d nodes=%d",
                  (int)getpid(), (void *)&instance_marker, i + 1, s->used,
                  s->queue_head, s->queue_tail, s->client_fd, s->attached, live);
        }
    }
    if (index >= 1 && index <= MAX_SESSIONS && sessions[index - 1].used) {
        struct session *session = &sessions[index - 1];
        /* A few commands per tick: applying a whole tree's worth of node API
         * calls in one go blocks the UI thread (the page's timer stops and the
         * mirror never finishes). Spreading them keeps the interface alive. */
        const int budget = 1;
        for (int step = 0; step < budget; step++) {
            char *line = NULL;
            pthread_mutex_lock(&session->queue_lock);
            if (session->queue_head != session->queue_tail) {
                line = session->queue[session->queue_head];
                session->queue[session->queue_head] = NULL;
                session->queue_head = (session->queue_head + 1) % QUEUE_DEPTH;
            }
            pthread_mutex_unlock(&session->queue_lock);
            if (!line) break;
            markf("HOST_APPLY s%d %.70s", session->index, line);
            apply_line(session, line);
            free(line);
            applied++;
        }
    }
    if (applied > 0 || (tick_calls <= 400 && tick_calls % 100 == 0)) {
        if (index >= 1 && index <= MAX_SESSIONS) {
            struct session *probe = &sessions[index - 1];
            markf("HOST_TICK_AFTER n=%d applied=%d head=%d tail=%d", tick_calls, applied, probe->queue_head,
                  probe->queue_tail);
        }
    }
    napi_value result = NULL;
    napi_create_int32(env, applied, &result);
    return result;
}

/* ------------------------------------------------------------ socket thread */

struct listener_arg {
    int index;
};

static void *socket_thread(void *raw) {
    struct listener_arg *arg = (struct listener_arg *)raw;
    int index = arg->index;
    free(arg);
    struct session *session = &sessions[index - 1];

    int server = socket(AF_UNIX, SOCK_STREAM, 0);
    if (server < 0) { markf("HOST_SOCKET_FAIL session=%d errno=%d", index, errno); return NULL; }
    struct sockaddr_un address;
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    address.sun_path[0] = '\0';                       /* abstract namespace */
    snprintf(address.sun_path + 1, sizeof(address.sun_path) - 1, "%s", session->socket_name);
    socklen_t length = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + 1 + strlen(session->socket_name));
    if (bind(server, (struct sockaddr *)&address, length) < 0) {
        markf("HOST_BIND_FAIL session=%d errno=%d", index, errno);
        close(server);
        return NULL;
    }
    if (listen(server, 4) < 0) { markf("HOST_LISTEN_FAIL session=%d errno=%d", index, errno); close(server); return NULL; }
    markf("HOST_LISTENING pid=%d session=%d %s base=%p marker=%p", (int)getpid(), index,
              session->socket_name, (void *)session, (void *)&instance_marker);

    for (;;) {
        int client = accept(server, NULL, NULL);
        if (client < 0) {
            if (errno == EINTR) continue;
            markf("HOST_ACCEPT_FAIL session=%d errno=%d", index, errno);
            break;
        }
        pthread_mutex_lock(&session->client_lock);
        if (session->client_fd >= 0) close(session->client_fd);
        session->client_fd = client;
        pthread_mutex_unlock(&session->client_lock);
        markf("HOST_CLIENT_CONNECTED session=%d", index);
        client_send(session, "ready session %d", index);

        char buffer[HOST_LINE_MAX * 4];
        size_t used = 0;
        for (;;) {
            ssize_t got = read(client, buffer + used, sizeof(buffer) - used - 1);
            if (got <= 0) break;
            used += (size_t)got;
            buffer[used] = 0;
            char *start = buffer;
            char *newline = NULL;
            while ((newline = strchr(start, '\n')) != NULL) {
                *newline = 0;
                size_t span = strlen(start);
                if (span && start[span - 1] == '\r') start[span - 1] = 0;
                char *copy = strdup(start);
                if (copy) {
                    markf("HOST_QUEUED session=%d %.60s", index, copy);
                    pthread_mutex_lock(&session->queue_lock);
                    int next = (session->queue_tail + 1) % QUEUE_DEPTH;
        if (next == session->queue_head) {
            markf("HOST_QUEUE_FULL session=%d", session->index);
            free(copy);
            return;
        }
                    if (next != session->queue_head) {
                        session->queue[session->queue_tail] = copy;
                        session->queue_tail = next;
                    } else {
                        free(copy);
                    }
                    pthread_mutex_unlock(&session->queue_lock);
                }
                start = newline + 1;
            }
            size_t remaining = strlen(start);
            memmove(buffer, start, remaining);
            used = remaining;
            if (used >= sizeof(buffer) - 2) used = 0;   /* oversized line: drop */
        }
        markf("HOST_CLIENT_CLOSED session=%d queue_head=%d queue_tail=%d", index, session->queue_head, session->queue_tail);
        pthread_mutex_lock(&session->client_lock);
        if (session->client_fd == client) session->client_fd = -1;
        pthread_mutex_unlock(&session->client_lock);
        close(client);
    }
    close(server);
    return NULL;
}

/* ------------------------------------------------------------------ napi api */

/* What the platform needs to know about the running applications: one line per
 * live window, "<session> <client name>". Written where the launcher can read it
 * (the application's own files directory), so repeated launches can be turned
 * into "this application already has a window". */
static void write_state_file(void) {
    char text[1024];
    size_t used = 0;
    text[0] = 0;
    for (int i = 0; i < MAX_SESSIONS; i++) {
        if (!sessions[i].used || !sessions[i].client_name[0]) continue;
        int written = snprintf(text + used, sizeof(text) - used, "%d %s\n",
                               sessions[i].index, sessions[i].client_name);
        if (written < 0 || (size_t)written >= sizeof(text) - used) break;
        used += (size_t)written;
    }
    int fd = open(MARK_PATH_STATE, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return;
    if (used) {
        ssize_t ignored = write(fd, text, used);
        (void)ignored;
    }
    close(fd);
}

/* Reserve a window number for a page that is about to create its container. */
static napi_value alloc_session(napi_env env, napi_callback_info info) {
    (void)info;
    int chosen = 0;
    for (int i = 0; i < MAX_SESSIONS; i++) {
        if (!sessions[i].used && !sessions[i].reserved) {
            sessions[i].reserved = 1;
            chosen = i + 1;
            break;
        }
    }
    markf("HOST_ALLOC %d", chosen);
    napi_value value = NULL;
    napi_create_int32(env, chosen, &value);
    return value;
}

/* The window is gone: tell the application and let it exit. The listening
 * socket stays open so this number can be handed out again. */
static napi_value stop_session(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value argv[1] = {NULL};
    napi_get_cb_info(env, info, &argc, argv, NULL, NULL);
    int index = 1;
    if (argc >= 1 && argv[0]) {
        int32_t value = 0;
        if (napi_get_value_int32(env, argv[0], &value) == napi_ok && value > 0) index = value;
    }
    if (index >= 1 && index <= MAX_SESSIONS) {
        struct session *session = &sessions[index - 1];
        markf("HOST_STOP session=%d client=%s", index, session->client_name);
        client_send(session, "close");
        pthread_mutex_lock(&session->client_lock);
        if (session->client_fd >= 0) {
            close(session->client_fd);
            session->client_fd = -1;
        }
        pthread_mutex_unlock(&session->client_lock);
        drop_tree(session);
        session->used = 0;
        session->reserved = 0;
        session->client_name[0] = 0;
        write_state_file();
    }
    napi_value value = NULL;
    napi_create_int32(env, index, &value);
    return value;
}

/* Called by the page once a node type component is mounted, which is when the
 * container callbacks exist and a root node can be attached. Arguments are the
 * session index the page gave its container and that container's id. */
static napi_value start(napi_env env, napi_callback_info info) {
    size_t argc = 2;
    napi_value argv[2] = {NULL, NULL};
    napi_get_cb_info(env, info, &argc, argv, NULL, NULL);
    int index = 1;
    if (argc >= 1 && argv[0]) {
        int32_t value = 0;
        if (napi_get_value_int32(env, argv[0], &value) == napi_ok && value > 0) index = value;
    }
    if (index < 1) index = 1;
    if (index > MAX_SESSIONS) index = MAX_SESSIONS;
    char *id_text = NULL;
    if (argc >= 2 && argv[1]) {
        size_t length = 0;
        if (napi_get_value_string_utf8(env, argv[1], NULL, 0, &length) == napi_ok && length) {
            id_text = calloc(1, length + 1);
            if (id_text) napi_get_value_string_utf8(env, argv[1], id_text, length + 1, &length);
        }
    }

    char result[160];
    if (!node_api) {
        void *api = OH_ArkUI_QueryModuleInterfaceByName(ARKUI_NATIVE_NODE, "ArkUI_NativeNodeAPI_1");
        node_api = (ArkUI_NativeNodeAPI_1 *)api;
    }
    if (!node_api) {
        host_mark("HOST_START_NO_NODE_API");
        napi_value value = NULL;
        napi_create_string_utf8(env, "no-api", NAPI_AUTO_LENGTH, &value);
        free(id_text);
        return value;
    }

    struct session *session = &sessions[index - 1];
    if (!session->used) {
        session->index = index;
        session->used = 1;
        session->client_fd = -1;
        pthread_mutex_init(&session->client_lock, NULL);
        pthread_mutex_init(&session->queue_lock, NULL);
        snprintf(session->socket_name, sizeof(session->socket_name), "%s-%d", SOCKET_PREFIX, index);
        if (!session->component) session->component = first_component;

        ArkUI_NodeHandle container = NULL;
        if (!session->component && id_text) {
            /* Another window in the same process: find its container by id. */
            int32_t found = OH_ArkUI_NodeUtils_GetAttachedNodeHandleById(id_text, &container);
            markf("HOST_CONTAINER session=%d byid=%d node=%p", index, found, (void *)container);
            if (container) {
                node_api->registerNodeEventReceiver(on_node_event);
                node_api->registerNodeEvent(container, NODE_TOUCH_EVENT, 0, NULL);
            }
        }

        session->root = node_api->createNode(ARKUI_NODE_COLUMN);
        node_api->registerNodeEventReceiver(on_node_event);
        node_api->registerNodeEvent(session->root, NODE_ON_CLICK, 0, NULL);
        node_api->registerNodeEvent(session->root, NODE_TOUCH_EVENT, 0, NULL);
        session->context = OH_ArkUI_GetContextByNode(session->root);
        markf("HOST_CONTEXT s%d %p", index, (void *)session->context);
        if (!session->driver_running && session->context) {
            session->driver_running = 1;
            session->drive_stop = 0;
            if (pthread_create(&session->driver, NULL, driver_thread, session) == 0) {
                pthread_detach(session->driver);
            } else {
                session->driver_running = 0;
            }
        }
        set_text(session->root, NODE_ID, id_text ? id_text : "l410UiHostRoot");
        set_number(session->root, NODE_WIDTH, 1000);
        set_number(session->root, NODE_HEIGHT, 620);
        set_number(session->root, NODE_PADDING, 16);
        set_color(session->root, NODE_BACKGROUND_COLOR, 0xFFF7F8FA);
        node_api->registerNodeEventReceiver(on_node_event);

        if (session->component) {
            int32_t rc = OH_NativeXComponent_AttachNativeRootNode(session->component, session->root);
            ArkUI_NodeHandle parent = OH_ArkUI_NodeUtils_GetParentInPageTree(session->root);
            session->attached = (parent != NULL);
            markf("HOST_START session=%d rc=%d parent=%p id=%s",
                  index, rc, (void *)parent, first_component_id);
        } else if (container) {
            int32_t rc = node_api->addChild(container, session->root);
            ArkUI_NodeHandle parent = OH_ArkUI_NodeUtils_GetParentInPageTree(session->root);
            session->attached = (parent != NULL);
            markf("HOST_START session=%d addchild=%d parent=%p", index, rc, (void *)parent);
        } else {
            markf("HOST_START session=%d no-container", index);
        }

        struct listener_arg *arg = calloc(1, sizeof(*arg));
        if (arg) {
            arg->index = index;
            pthread_t thread;
            if (pthread_create(&thread, NULL, socket_thread, arg) == 0) pthread_detach(thread);
        }
        snprintf(result, sizeof(result), "session=%d attached=%d socket=%s",
                 index, session->attached, session->socket_name);
    } else {
        snprintf(result, sizeof(result), "session=%d exists", index);
    }
    free(id_text);
    napi_value value = NULL;
    napi_create_string_utf8(env, result, NAPI_AUTO_LENGTH, &value);
    return value;
}

/* One management request.  Socket I/O must never run on the ArkUI thread: a
 * slow status scan or a broker restart used to freeze the whole manager
 * window.  Keep the existing wire format and global function name, but return
 * a Promise whose work runs on the N-API worker pool. */
struct manager_async_context {
    napi_deferred deferred;
    napi_async_work work;
    char request[HOST_LINE_MAX];
    char answer[HOST_LINE_MAX * 4];
    char error[256];
};

static void manager_exchange(struct manager_async_context *context) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        snprintf(context->error, sizeof(context->error), "manager socket: %s", strerror(errno));
        return;
    }
    struct timeval timeout = {3, 0};
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    struct sockaddr_un address;
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    address.sun_path[0] = '\0';
    snprintf(address.sun_path + 1, sizeof(address.sun_path) - 1, "%s", MANAGER_SOCKET);
    socklen_t address_length = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + 1 + strlen(MANAGER_SOCKET));
    if (connect(fd, (struct sockaddr *)&address, address_length) != 0) {
        snprintf(context->error, sizeof(context->error), "manager connect: %s", strerror(errno));
        close(fd);
        return;
    }
    const char *cursor = context->request;
    size_t remaining = strlen(context->request);
    while (remaining) {
        ssize_t written = write(fd, cursor, remaining);
        if (written < 0 && errno == EINTR) continue;
        if (written <= 0) {
            snprintf(context->error, sizeof(context->error), "manager write: %s", strerror(errno));
            close(fd);
            return;
        }
        cursor += written;
        remaining -= (size_t)written;
    }
    size_t used = 0;
    while (used < sizeof(context->answer) - 1) {
        ssize_t got = read(fd, context->answer + used, sizeof(context->answer) - 1 - used);
        if (got < 0 && errno == EINTR) continue;
        if (got <= 0) break;
        used += (size_t)got;
        context->answer[used] = 0;
        if (memchr(context->answer, '\n', used)) break;
    }
    close(fd);
    if (!used) snprintf(context->error, sizeof(context->error), "manager returned no response");
}

static void manager_execute(napi_env env, void *data) {
    (void)env;
    manager_exchange((struct manager_async_context *)data);
}

static void manager_complete(napi_env env, napi_status status, void *data) {
    struct manager_async_context *context = data;
    napi_value value = NULL;
    if (status != napi_ok && !context->error[0]) {
        snprintf(context->error, sizeof(context->error), "manager request cancelled");
    }
    if (context->error[0]) {
        host_mark("HOST_MANAGER_ASYNC_COMPLETE_ERROR");
        napi_value message = NULL;
        napi_value error = NULL;
        napi_status string_status = napi_create_string_utf8(
            env, context->error, NAPI_AUTO_LENGTH, &message);
        napi_status error_status = napi_create_error(env, NULL, message, &error);
        napi_status settle_status = napi_reject_deferred(env, context->deferred, error);
        if (string_status != napi_ok || error_status != napi_ok || settle_status != napi_ok) {
            markf("HOST_MANAGER_ASYNC_REJECT string=%d error=%d settle=%d",
                  string_status, error_status, settle_status);
        }
    } else {
        napi_status string_status = napi_create_string_utf8(
            env, context->answer, NAPI_AUTO_LENGTH, &value);
        napi_status settle_status = napi_resolve_deferred(env, context->deferred, value);
        if (string_status != napi_ok || settle_status != napi_ok) {
            markf("HOST_MANAGER_ASYNC_RESOLVE_FAILED string=%d settle=%d bytes=%zu",
                  string_status, settle_status, strlen(context->answer));
        }
    }
    napi_delete_async_work(env, context->work);
    free(context);
}

static napi_value manager_call(napi_env env, napi_callback_info info) {
    size_t argc = 2;
    napi_value argv[2] = {NULL, NULL};
    napi_get_cb_info(env, info, &argc, argv, NULL, NULL);
    char path[160] = {0};
    char *body = NULL;
    if (argc >= 1 && argv[0]) {
        size_t length = 0;
        if (napi_get_value_string_utf8(env, argv[0], NULL, 0, &length) == napi_ok && length) {
            napi_get_value_string_utf8(env, argv[0], path, sizeof(path), &length);
        }
    }
    if (argc >= 2 && argv[1]) {
        size_t length = 0;
        if (napi_get_value_string_utf8(env, argv[1], NULL, 0, &length) == napi_ok && length) {
            body = calloc(1, length + 1);
            if (body) napi_get_value_string_utf8(env, argv[1], body, length + 1, &length);
        }
    }
    struct manager_async_context *context = calloc(1, sizeof(*context));
    if (!context) {
        free(body);
        napi_throw_error(env, NULL, "manager request allocation failed");
        return NULL;
    }
    snprintf(context->request, sizeof(context->request),
             "{\"path\":\"%s\",\"method\":\"%s\",\"body\":%s}\n",
             path, body ? "POST" : "GET", body ? body : "null");
    free(body);
    napi_value promise = NULL;
    napi_value resource = NULL;
    /* A desktop entry has no UI of its own.  Deliver its one short local
     * launch packet before returning to ArkTS so EntryAbility can terminate
     * immediately.  Keeping this on the generic async queue leaves a blank
     * start window visible until that queue is scheduled (15 seconds on the
     * L410 when the optional QoS node is absent).  The broker's launch route
     * only validates metadata and queues work; the Linux window remains owned
     * by the resident AnCo runtime. */
    if (strcmp(path, "/anco/launch") == 0) {
        napi_value value = NULL;
        napi_value error_message = NULL;
        napi_value error = NULL;
        host_mark("HOST_MANAGER_LAUNCH_INLINE_BEGIN");
        manager_exchange(context);
        if (context->error[0])
            markf("HOST_MANAGER_LAUNCH_INLINE_ERROR %.220s", context->error);
        else
            markf("HOST_MANAGER_LAUNCH_INLINE_REPLY %.220s", context->answer);
        if (napi_create_promise(env, &context->deferred, &promise) != napi_ok) {
            host_mark("HOST_MANAGER_LAUNCH_INLINE_PROMISE_ERROR");
            free(context);
            napi_throw_error(env, NULL, "manager launch promise creation failed");
            return NULL;
        }
        if (context->error[0]) {
            napi_create_string_utf8(env, context->error, NAPI_AUTO_LENGTH,
                                    &error_message);
            napi_create_error(env, NULL, error_message, &error);
            napi_reject_deferred(env, context->deferred, error);
        } else {
            napi_create_string_utf8(env, context->answer, NAPI_AUTO_LENGTH,
                                    &value);
            napi_resolve_deferred(env, context->deferred, value);
        }
        host_mark("HOST_MANAGER_LAUNCH_INLINE_SETTLED");
        free(context);
        return promise;
    }
    if (napi_create_promise(env, &context->deferred, &promise) != napi_ok ||
        napi_create_string_utf8(env, "l410ManagerCall", NAPI_AUTO_LENGTH, &resource) != napi_ok ||
        napi_create_async_work(env, NULL, resource, manager_execute, manager_complete,
                               context, &context->work) != napi_ok ||
        napi_queue_async_work_with_qos(env, context->work,
                                       napi_qos_user_initiated) != napi_ok) {
        free(context);
        napi_throw_error(env, NULL, "manager async work creation failed");
        return NULL;
    }
    return promise;
}

/* Register a function into the page's global object only if it is not there
 * yet. The library can be loaded more than once per process (the framework
 * loads it for the node component, and the page may import it as well); the
 * first load is the one that owns the socket and the control tree, so its
 * functions are the ones the page must keep calling. */
static void set_global_once(napi_env env, napi_value global, const char *name, napi_value fn) {
    bool present = false;
    napi_value existing = NULL;
    if (napi_get_named_property(env, global, name, &existing) == napi_ok && existing) {
        napi_typeof(env, existing, NULL);
        napi_has_named_property(env, global, name, &present);
    }
    if (present) {
        markf("HOST_GLOBAL_KEPT %s marker=%p", name, (void *)&instance_marker);
        return;
    }
    napi_set_named_property(env, global, name, fn);
    markf("HOST_GLOBAL_SET %s marker=%p", name, (void *)&instance_marker);
}

static napi_value touch(napi_env env, napi_callback_info info) {
    size_t argc = 4;
    napi_value argv[4] = {NULL, NULL, NULL, NULL};
    napi_get_cb_info(env, info, &argc, argv, NULL, NULL);
    int32_t index = 1, action = 0;
    double x = 0, y = 0;
    if (argc >= 1 && argv[0]) napi_get_value_int32(env, argv[0], &index);
    if (argc >= 2 && argv[1]) napi_get_value_double(env, argv[1], &x);
    if (argc >= 3 && argv[2]) napi_get_value_double(env, argv[2], &y);
    if (argc >= 4 && argv[3]) napi_get_value_int32(env, argv[3], &action);

    int hit = -1;
    if (index >= 1 && index <= MAX_SESSIONS) {
        struct session *session = &sessions[index - 1];
        hit = hit_test(session, (float)x, (float)y);
        markf("HOST_TOUCH_PAGE s%d action=%d x=%.0f y=%.0f hit=%d", index, action, x, y, hit);
        if (hit > 0 && action == 0) {   /* press */
            const int kind = session->boxes[hit].kind;
            client_send(session, kind == ARKUI_NODE_BUTTON ? "click %d" : "focus %d", hit);
        }
    }
    napi_value result = NULL;
    napi_create_int32(env, hit, &result);
    return result;
}

/* A page side note that goes to its own file: the main host log is truncated on
 * start up, so anything the page wants to report about input has to live
 * somewhere else. */
static napi_value note(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value argv[1] = {NULL};
    napi_get_cb_info(env, info, &argc, argv, NULL, NULL);
    char text[512] = {0};
    size_t length = 0;
    if (argc >= 1 && argv[0]) napi_get_value_string_utf8(env, argv[0], text, sizeof(text) - 1, &length);
    FILE *file = fopen("/data/storage/el2/base/files/ui-touch.log", "a");
    if (file) {
        fprintf(file, "%s\n", text);
        fclose(file);
    }
    napi_value result = NULL;
    napi_create_int32(env, (int)length, &result);
    return result;
}

/* ------------------------------------------------------------------ audio --
 * Playback PCM arrives from the container on a fifo inside the application's
 * own files directory; the page pulls it and hands it to the HarmonyOS audio
 * renderer. No server, no HTTP: the data path is a plain pipe plus a call.
 */
#define AUDIO_FIFO "/data/storage/el2/base/files/audio-playback.fifo"

static int audio_fd = -1;
static long long audio_served = 0;

static int audio_open(void) {
    if (audio_fd >= 0) return audio_fd;
    if (mkfifo(AUDIO_FIFO, 0666) != 0 && errno != EEXIST) {
        markf("HOST_AUDIO_FIFO_FAIL errno=%d", errno);
        return -1;
    }
    int fd = open(AUDIO_FIFO, O_RDONLY | O_NONBLOCK);
    if (fd < 0) {
        markf("HOST_AUDIO_OPEN_FAIL errno=%d", errno);
        return -1;
    }
    audio_fd = fd;
    markf("HOST_AUDIO_OPEN ok");
    return fd;
}

static napi_value audio_pull(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value argv[1] = {NULL};
    napi_get_cb_info(env, info, &argc, argv, NULL, NULL);
    int32_t want = 8192;
    if (argc >= 1 && argv[0]) napi_get_value_int32(env, argv[0], &want);
    if (want < 256 || want > 65536) want = 8192;

    int fd = audio_open();
    size_t filled = 0;
    void *raw = NULL;
    napi_value buffer = NULL;
    if (fd >= 0 && napi_create_arraybuffer(env, (size_t)want, &raw, &buffer) == napi_ok && raw) {
        ssize_t got = read(fd, raw, (size_t)want);
        if (got > 0) {
            filled = (size_t)got;
            audio_served += got;
            if (audio_served < 300000 || (audio_served % 200000) < (long long)got)
                markf("HOST_AUDIO_PULL %d bytes total=%lld", (int)got, audio_served);
        }
    }
    if (!buffer) napi_create_arraybuffer(env, 0, NULL, &buffer);
    return buffer;
}

/* Captured PCM goes the other way: the page reads it from the HarmonyOS
 * capturer and hands it here, and it lands on a fifo for the relay to carry
 * into the container. */
#define AUDIO_CAPTURE_FIFO "/data/storage/el2/base/files/audio-capture.fifo"

static int audio_capture_fd = -1;
static long long audio_captured = 0;

static napi_value audio_push(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value argv[1] = {NULL};
    napi_get_cb_info(env, info, &argc, argv, NULL, NULL);
    if (argc < 1 || !argv[0]) return NULL;

    void *data = NULL;
    size_t length = 0;
    if (napi_get_arraybuffer_info(env, argv[0], &data, &length) != napi_ok || !data || !length) return NULL;

    if (audio_capture_fd < 0) {
        if (mkfifo(AUDIO_CAPTURE_FIFO, 0666) != 0 && errno != EEXIST) {
            markf("HOST_AUDIO_CAP_FIFO_FAIL errno=%d", errno);
            return NULL;
        }
        audio_capture_fd = open(AUDIO_CAPTURE_FIFO, O_WRONLY | O_NONBLOCK);
        if (audio_capture_fd < 0) {
            markf("HOST_AUDIO_CAP_OPEN_FAIL errno=%d", errno);
            return NULL;
        }
        markf("HOST_AUDIO_CAP_OPEN ok");
    }
    ssize_t written = write(audio_capture_fd, data, length);
    if (written > 0) {
        audio_captured += written;
        if (audio_captured < 200000 || (audio_captured % 100000) < (long long)written)
            markf("HOST_AUDIO_PUSH %d bytes total=%lld", (int)written, audio_captured);
    }
    return NULL;
}

/* --------------------------------------------------------------- clipboard --
 * Two small files carry text between the page (which owns the HarmonyOS
 * pasteboard) and the service (which relays it to the container).
 */
#define CLIP_OUT "/data/storage/el2/base/files/clipboard-out.txt"
#define CLIP_IN  "/data/storage/el2/base/files/clipboard-in.txt"

static void write_text_file(const char *path, const char *text, size_t length) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) {
        markf("HOST_CLIP_WRITE_FAIL errno=%d", errno);
        return;
    }
    if (length) write(fd, text, length);
    close(fd);
}

static napi_value clipboard_push(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value argv[1] = {NULL};
    napi_get_cb_info(env, info, &argc, argv, NULL, NULL);
    if (argc < 1 || !argv[0]) return NULL;
    size_t length = 0;
    if (napi_get_value_string_utf8(env, argv[0], NULL, 0, &length) != napi_ok) return NULL;
    char *text = (char *)malloc(length + 1);
    if (!text) return NULL;
    if (napi_get_value_string_utf8(env, argv[0], text, length + 1, &length) != napi_ok) {
        free(text);
        return NULL;
    }
    write_text_file(CLIP_OUT, text, length);
    markf("HOST_CLIP_PUSH %d bytes", (int)length);
    free(text);
    return NULL;
}

static napi_value clipboard_pull(napi_env env, napi_callback_info info) {
    napi_value result = NULL;
    char buffer[4096];
    ssize_t got = -1;
    int fd = open(CLIP_IN, O_RDONLY);
    if (fd >= 0) {
        got = read(fd, buffer, sizeof(buffer) - 1);
        close(fd);
        unlink(CLIP_IN);
    }
    if (got < 0) got = 0;
    buffer[got] = 0;
    napi_create_string_utf8(env, buffer, (size_t)got, &result);
    return result;
}

/* --------------------------------------------------------------- sensors --
 * The page reads the device sensors through the HarmonyOS sensor service and
 * hands the latest values here; the management service carries the file into
 * the container so applications can read them as plain data.
 */
#define SENSOR_FILE "/data/storage/el2/base/files/sensors.json"

static napi_value sensor_push(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value argv[1] = {NULL};
    napi_get_cb_info(env, info, &argc, argv, NULL, NULL);
    if (argc < 1 || !argv[0]) return NULL;
    size_t length = 0;
    if (napi_get_value_string_utf8(env, argv[0], NULL, 0, &length) != napi_ok || !length) return NULL;
    if (length > 1024) length = 1024;
    char *text = (char *)malloc(length + 1);
    if (!text) return NULL;
    if (napi_get_value_string_utf8(env, argv[0], text, length + 1, &length) != napi_ok) {
        free(text);
        return NULL;
    }
    int fd = open(SENSOR_FILE, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd >= 0) {
        const ssize_t wrote = write(fd, text, length);
        (void)wrote;
        close(fd);
    }
    free(text);
    return NULL;
}

/* The Huawei StatusBar kit is exposed only through ArkTS `import lazy` on this
 * image.  Keep the factory supplied by the module AbilityStage, exactly like
 * Qt OHOS keeps its OhosExportModules factory map, and invoke it only when the
 * entry ability needs the real vendor object. */
static napi_env status_bar_factory_env = NULL;
static napi_ref status_bar_factory_ref = NULL;

static napi_value setup_status_bar_factories(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value argv[1] = {NULL};
    napi_value factory = NULL;
    napi_value result = NULL;
    napi_valuetype type = napi_undefined;
    napi_get_cb_info(env, info, &argc, argv, NULL, NULL);
    if (argc < 1 || !argv[0] ||
        napi_get_named_property(env, argv[0],
            "@kit.StatusBarExtensionKit.statusBarManager", &factory) != napi_ok ||
        !factory || napi_typeof(env, factory, &type) != napi_ok || type != napi_function) {
        napi_get_boolean(env, false, &result);
        return result;
    }
    if (status_bar_factory_ref && status_bar_factory_env == env)
        napi_delete_reference(env, status_bar_factory_ref);
    status_bar_factory_ref = NULL;
    status_bar_factory_env = env;
    if (napi_create_reference(env, factory, 1, &status_bar_factory_ref) != napi_ok)
        status_bar_factory_ref = NULL;
    napi_get_boolean(env, status_bar_factory_ref != NULL, &result);
    return result;
}

static napi_value load_status_bar_manager(napi_env env, napi_callback_info info) {
    (void)info;
    napi_value result = NULL;
    napi_value factory = NULL;
    napi_value receiver = NULL;
    napi_status status = napi_invalid_arg;
    if (status_bar_factory_ref && status_bar_factory_env == env &&
        napi_get_reference_value(env, status_bar_factory_ref, &factory) == napi_ok && factory &&
        napi_get_undefined(env, &receiver) == napi_ok)
        status = napi_call_function(env, receiver, factory, 0, NULL, &result);
    if (status != napi_ok || !result) {
        bool pending = false;
        napi_value ignored = NULL;
        if (napi_is_exception_pending(env, &pending) == napi_ok && pending)
            napi_get_and_clear_last_exception(env, &ignored);
        char message[96];
        snprintf(message, sizeof(message), "status bar ArkTS factory unavailable: %d", (int)status);
        napi_throw_error(env, NULL, message);
        napi_get_undefined(env, &result);
    }
    return result;
}

static napi_value init(napi_env env, napi_value exports) {
    napi_value fn = NULL;
    napi_value object = NULL;
    napi_value global = NULL;
    void *unwrapped = NULL;

    napi_create_function(env, "start", NAPI_AUTO_LENGTH, start, NULL, &fn);
    napi_set_named_property(env, exports, "start", fn);
    if (napi_create_function(env, "l410UiHostStart", NAPI_AUTO_LENGTH, start, NULL, &fn) == napi_ok &&
        napi_get_global(env, &global) == napi_ok && global) {
        set_global_once(env, global, "l410UiHostStart", fn);
    }
    if (napi_create_function(env, "l410UiHostTick", NAPI_AUTO_LENGTH, tick, NULL, &fn) == napi_ok &&
        napi_get_global(env, &global) == napi_ok && global) {
        set_global_once(env, global, "l410UiHostTick", fn);
    }
    if (napi_create_function(env, "l410UiHostNote", NAPI_AUTO_LENGTH, note, NULL, &fn) == napi_ok &&
        napi_get_global(env, &global) == napi_ok && global) {
        set_global_once(env, global, "l410UiHostNote", fn);
    }
    if (napi_create_function(env, "l410UiHostTap", NAPI_AUTO_LENGTH, host_tap, NULL, &fn) == napi_ok &&
        napi_get_global(env, &global) == napi_ok && global) {
        set_global_once(env, global, "l410UiHostTap", fn);
    }
    if (napi_create_function(env, "l410UiHostSensorPush", NAPI_AUTO_LENGTH, sensor_push, NULL, &fn) == napi_ok &&
        napi_get_global(env, &global) == napi_ok && global) {
        set_global_once(env, global, "l410UiHostSensorPush", fn);
    }
    if (napi_create_function(env, "l410UiHostClipboardPush", NAPI_AUTO_LENGTH, clipboard_push, NULL, &fn) == napi_ok &&
        napi_get_global(env, &global) == napi_ok && global) {
        set_global_once(env, global, "l410UiHostClipboardPush", fn);
    }
    if (napi_create_function(env, "l410UiHostClipboardPull", NAPI_AUTO_LENGTH, clipboard_pull, NULL, &fn) == napi_ok &&
        napi_get_global(env, &global) == napi_ok && global) {
        set_global_once(env, global, "l410UiHostClipboardPull", fn);
    }
    if (napi_create_function(env, "l410UiHostAudioPush", NAPI_AUTO_LENGTH, audio_push, NULL, &fn) == napi_ok &&
        napi_get_global(env, &global) == napi_ok && global) {
        set_global_once(env, global, "l410UiHostAudioPush", fn);
    }
    if (napi_create_function(env, "l410UiHostAudioPull", NAPI_AUTO_LENGTH, audio_pull, NULL, &fn) == napi_ok &&
        napi_get_global(env, &global) == napi_ok && global) {
        set_global_once(env, global, "l410UiHostAudioPull", fn);
    }
    if (napi_create_function(env, "l410UiHostTouch", NAPI_AUTO_LENGTH, touch, NULL, &fn) == napi_ok &&
        napi_get_global(env, &global) == napi_ok && global) {
        set_global_once(env, global, "l410UiHostTouch", fn);
    }
    if (napi_create_function(env, "l410UiHostAlloc", NAPI_AUTO_LENGTH, alloc_session, NULL, &fn) == napi_ok &&
        napi_get_global(env, &global) == napi_ok && global) {
        set_global_once(env, global, "l410UiHostAlloc", fn);
    }
    if (napi_create_function(env, "l410UiHostStop", NAPI_AUTO_LENGTH, stop_session, NULL, &fn) == napi_ok &&
        napi_get_global(env, &global) == napi_ok && global) {
        set_global_once(env, global, "l410UiHostStop", fn);
    }
    if (napi_create_function(env, "l410ManagerCall", NAPI_AUTO_LENGTH, manager_call, NULL, &fn) == napi_ok &&
        napi_get_global(env, &global) == napi_ok && global) {
        napi_set_named_property(env, exports, "managerCall", fn);
        set_global_once(env, global, "l410ManagerCall", fn);
    }
    if (napi_create_function(env, "loadStatusBarManager", NAPI_AUTO_LENGTH,
                             load_status_bar_manager, NULL, &fn) == napi_ok) {
        napi_set_named_property(env, exports, "loadStatusBarManager", fn);
    }
    if (napi_create_function(env, "setupStatusBarFactories", NAPI_AUTO_LENGTH,
                             setup_status_bar_factories, NULL, &fn) == napi_ok) {
        napi_set_named_property(env, exports, "setupStatusBarFactories", fn);
    }

    if (napi_get_named_property(env, exports, "__NATIVE_XCOMPONENT_OBJ__", &object) == napi_ok &&
        napi_unwrap(env, object, &unwrapped) == napi_ok && unwrapped) {
        /* The framework loads this library once per window, each time with that
         * window's own component, so remember it for the session its container
         * id names. Without this the second window would have no component to
         * attach a root node to. */
        char id[128] = {0};
        uint64_t length = sizeof(id);
        OH_NativeXComponent_GetXComponentId((OH_NativeXComponent *)unwrapped, id, &length);
        const char *dash = strrchr(id, '-');
        int index = dash ? parse_int(dash + 1) : 0;
        if (index >= 1 && index <= MAX_SESSIONS) {
            sessions[index - 1].index = index;
            sessions[index - 1].component = (OH_NativeXComponent *)unwrapped;
        static OH_NativeXComponent_Callback xc_callbacks;
        xc_callbacks.DispatchTouchEvent = on_xc_touch;
        int32_t reg = OH_NativeXComponent_RegisterCallback((OH_NativeXComponent *)unwrapped, &xc_callbacks);
        markf("HOST_XC_CALLBACK session=%d rc=%d", index, reg);
            markf("HOST_COMPONENT session=%d %p id=%s", index, unwrapped, id);
        } else {
            first_component = (OH_NativeXComponent *)unwrapped;
            snprintf(first_component_id, sizeof(first_component_id), "%s", id);
            markf("HOST_COMPONENT unbound %p id=%s", unwrapped, id);
        }
    } else {
        host_mark("HOST_NO_COMPONENT");
    }
    host_mark("HOST_LIBRARY_LOADED");
    write_state_file();          /* a fresh process has no live windows */
    return exports;
}

static napi_module module = {1, 0, NULL, init, "l410uihost", NULL, {0}};

__attribute__((constructor)) static void register_module(void) {
    napi_module_register(&module);
}
