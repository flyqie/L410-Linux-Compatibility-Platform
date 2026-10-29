#define _GNU_SOURCE
#include "anco_linux_ownership.h"

#include <errno.h>
#include <limits.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

typedef struct Process Process;
typedef struct Task Task;
typedef struct Activity Activity;
typedef struct Surface Surface;
typedef struct Window Window;
typedef struct Cursor Cursor;

struct Process {
    AncoLinuxProcess identity;
    int pidfd;
    Process *next;
};
struct Task {
    int32_t id, user, guest_uid;
    char *bundle, *ability;
    Process *process;
    bool finished;
    Task *next;
};
struct Activity {
    int32_t id;
    Task *task;
    Process *process;
    Activity *next;
};
struct Surface {
    const void *client, *object;
    Activity *activity;
    Process *process;
    Surface *next;
};
struct Cursor {
    int32_t user, last;
    Cursor *next;
};
struct Window {
    const void *object;
    Surface *surface;
    int32_t id;
    Window *next;
};
struct AncoLinuxOwnership {
    Process *processes;
    Task *tasks;
    Activity *activities;
    Surface *surfaces;
    Window *windows;
    Cursor *cursors;
    int32_t last_activity;
    int32_t window_base, last_window;
    bool windows_configured;
};

static int process_stat(pid_t pid, uint64_t *ticks)
{
    char path[64], line[4096];
    snprintf(path, sizeof(path), "/proc/%ld/stat", (long)pid);
    FILE *f = fopen(path, "re");
    if (!f) return -errno;
    char *got = fgets(line, sizeof(line), f);
    fclose(f);
    if (!got) return -ESRCH;
    /* comm may itself contain spaces and ')'; fields follow its last ')'. */
    char *end = strrchr(line, ')');
    if (!end || end[1] != ' ' || !end[2]) return -EPROTO;
    if (end[2] == 'Z' || end[2] == 'X' || end[2] == 'x') return -ESRCH;
    char *save = NULL;
    char *token = strtok_r(end + 2, " \n", &save);
    for (int field = 3; field < 22 && token; ++field)
        token = strtok_r(NULL, " \n", &save);
    if (!token || token[0] < '0' || token[0] > '9') return -EPROTO;
    errno = 0;
    char *tail = NULL;
    const unsigned long long value = strtoull(token, &tail, 10);
    if (errno || !tail || *tail || !value) return -EPROTO;
    *ticks = (uint64_t)value;
    return 0;
}

int anco_linux_process_read(pid_t pid, AncoLinuxProcess *out)
{
    if (pid <= 0 || !out) return -EINVAL;
    char path[64], line[512];
    struct stat self_ns, peer_ns;
    snprintf(path, sizeof(path), "/proc/%ld/ns/pid", (long)pid);
    if (stat("/proc/self/ns/pid", &self_ns) || stat(path, &peer_ns))
        return -errno;
    if (self_ns.st_dev != peer_ns.st_dev || self_ns.st_ino != peer_ns.st_ino)
        return -EXDEV;
    uint64_t before = 0, after = 0;
    int result = process_stat(pid, &before);
    if (result) return result;
    snprintf(path, sizeof(path), "/proc/%ld/status", (long)pid);
    FILE *f = fopen(path, "re");
    if (!f) return -errno;
    unsigned long ruid = 0, euid = 0, suid = 0, fsuid = 0;
    unsigned long rgid = 0, egid = 0, sgid = 0, fsgid = 0;
    bool have_uid = false, have_gid = false;
    while (fgets(line, sizeof(line), f)) {
        if (!strncmp(line, "Uid:", 4))
            have_uid = sscanf(line + 4, "%lu %lu %lu %lu", &ruid, &euid,
                              &suid, &fsuid) == 4;
        if (!strncmp(line, "Gid:", 4))
            have_gid = sscanf(line + 4, "%lu %lu %lu %lu", &rgid, &egid,
                              &sgid, &fsgid) == 4;
    }
    fclose(f);
    if (!have_uid || !have_gid) return -EPROTO;
    /* SO_PEERCRED is the effective credential at connection creation. Refuse
     * a process with divergent credentials rather than guessing its identity.
     */
    if (ruid != euid || suid != euid || fsuid != euid ||
        rgid != egid || sgid != egid || fsgid != egid) return -EPERM;
    if ((unsigned long)(uid_t)euid != euid ||
        (unsigned long)(gid_t)egid != egid) return -ERANGE;
    result = process_stat(pid, &after);
    if (result) return result;
    if (before != after) return -ESTALE;
    *out = (AncoLinuxProcess){pid, (uid_t)euid, (gid_t)egid, before};
    return 0;
}

static bool identity_equal(const AncoLinuxProcess *a, const AncoLinuxProcess *b)
{
    return a->pid == b->pid && a->uid == b->uid && a->gid == b->gid &&
        a->start_ticks == b->start_ticks;
}

static bool process_alive(const Process *p)
{
    if (p->pidfd >= 0) {
        struct pollfd fd = {p->pidfd, POLLIN, 0};
        int ready;
        do { ready = poll(&fd, 1, 0); } while (ready < 0 && errno == EINTR);
        /* A pidfd pins this process identity across numeric PID reuse. Do not
         * reopen several /proc files on the normal surface lookup path. */
        return ready == 0;
    }
    AncoLinuxProcess now;
    return !anco_linux_process_read(p->identity.pid, &now) &&
        identity_equal(&now, &p->identity);
}

static void collect_processes(AncoLinuxOwnership *s)
{
    Process **p = &s->processes;
    while (*p) {
        bool used = false;
        for (Task *t = s->tasks; t && !used; t = t->next)
            used = t->process == *p;
        for (Activity *a = s->activities; a && !used; a = a->next)
            used = a->process == *p;
        for (Surface *w = s->surfaces; w && !used; w = w->next)
            used = w->process == *p;
        if (used) { p = &(*p)->next; continue; }
        Process *old = *p; *p = old->next;
        if (old->pidfd >= 0) close(old->pidfd);
        free(old);
    }
}

static int process_get(AncoLinuxOwnership *s, const AncoLinuxProcess *identity,
                       Process **out)
{
    if (!identity) return -EINVAL;
    AncoLinuxProcess now;
    int result = anco_linux_process_read(identity->pid, &now);
    if (result) return result;
    if (!identity_equal(identity, &now)) return -ESTALE;
    for (Process *p = s->processes; p; p = p->next) {
        if (identity_equal(&p->identity, identity)) {
            if (!process_alive(p)) return -ESRCH;
            *out = p;
            return 0;
        }
    }
    Process *p = calloc(1, sizeof(*p));
    if (!p) return -ENOMEM;
    p->identity = *identity;
    p->pidfd = -1;
#ifdef SYS_pidfd_open
    p->pidfd = (int)syscall(SYS_pidfd_open, identity->pid, 0);
    if (p->pidfd < 0 && errno != ENOSYS && errno != EINVAL && errno != EPERM) {
        result = -errno;
        free(p);
        return result;
    }
#endif
    /* pidfd_open may race numeric PID reuse after the first /proc snapshot.
     * Recapture while holding the pidfd before admitting the record. */
    result = anco_linux_process_read(identity->pid, &now);
    if (result || !identity_equal(identity, &now) || !process_alive(p)) {
        if (p->pidfd >= 0) close(p->pidfd);
        free(p);
        return -ESTALE;
    }
    p->next = s->processes;
    s->processes = p;
    *out = p;
    return 0;
}

static Task *find_task(AncoLinuxOwnership *s, int32_t id)
{
    for (Task *t = s->tasks; t; t = t->next)
        if (t->id == id) return t;
    return NULL;
}

static Activity *find_activity(AncoLinuxOwnership *s, int32_t id)
{
    for (Activity *a = s->activities; a; a = a->next)
        if (a->id == id) return a;
    return NULL;
}

static bool valid_user(int32_t user)
{
    /* Keep the upper-exclusive bound representable in the original int32 ID. */
    return user >= 0 && ((int64_t)user + 1) * 100000 <= INT32_MAX;
}

static bool valid_app(const AncoLinuxApp *app)
{
    return app && valid_user(app->user_id) && app->guest_uid >= 0 &&
        app->guest_uid / 100000 == app->user_id &&
        app->guest_uid % 100000 >= 10000 &&
        app->guest_uid % 100000 <= 19999 &&
        app->bundle && *app->bundle && strlen(app->bundle) <= 256 &&
        app->ability && *app->ability && strlen(app->ability) <= 256;
}

static int get_cursor(AncoLinuxOwnership *s, int32_t user, Cursor **out)
{
    if (!valid_user(user)) return -EINVAL;
    for (Cursor *c = s->cursors; c; c = c->next) {
        if (c->user == user) { *out = c; return 0; }
    }
    Cursor *c = calloc(1, sizeof(*c));
    if (!c) return -ENOMEM;
    c->user = user;
    c->last = user * 100000;
    c->next = s->cursors;
    s->cursors = c;
    *out = c;
    return 0;
}

static int next_task(AncoLinuxOwnership *s, Cursor *c, int32_t *out)
{
    int32_t id = c->last + 1;
    if (id == (c->user + 1) * 100000) id = c->user * 100000;
    while (find_task(s, id)) {
        ++id;
        if (id == (c->user + 1) * 100000) id = c->user * 100000;
        /* Original getNextTaskIdForUser throws on reaching the prior cursor
         * after a collision, before examining that cursor as a candidate. */
        if (id == c->last) return -ENOSPC;
    }
    *out = id;
    return 0;
}

AncoLinuxOwnership *anco_linux_ownership_new(void)
{
    return calloc(1, sizeof(AncoLinuxOwnership));
}

int anco_linux_task_restore_cursor(AncoLinuxOwnership *s,
                                   int32_t user, int32_t id)
{
    if (!s || !valid_user(user) || id < user * 100000 ||
        id >= (user + 1) * 100000) return -EINVAL;
    Cursor *c;
    int result = get_cursor(s, user, &c);
    if (result) return result;
    if (id > c->last) c->last = id;
    return 0;
}

int anco_linux_task_create(AncoLinuxOwnership *s, const AncoLinuxApp *app,
                          const AncoLinuxProcess *identity, int32_t *out)
{
    if (!s || !out || !valid_app(app)) return -EINVAL;
    Process *p;
    int result = process_get(s, identity, &p);
    if (result) return result;
    Cursor *c;
    result = get_cursor(s, app->user_id, &c);
    if (result) { collect_processes(s); return result; }
    int32_t id;
    result = next_task(s, c, &id);
    if (result) { collect_processes(s); return result; }
    Task *t = calloc(1, sizeof(*t));
    if (!t) { collect_processes(s); return -ENOMEM; }
    t->bundle = strdup(app->bundle);
    t->ability = strdup(app->ability);
    if (!t->bundle || !t->ability) {
        free(t->bundle); free(t->ability); free(t);
        collect_processes(s);
        return -ENOMEM;
    }
    t->id = id; t->user = app->user_id; t->guest_uid = app->guest_uid;
    t->process = p;
    t->next = s->tasks;
    s->tasks = t;
    c->last = id;
    *out = id;
    return 0;
}

int anco_linux_activity_create(AncoLinuxOwnership *s, int32_t task_id,
                              const AncoLinuxApp *app,
                              const AncoLinuxProcess *identity, int32_t *out)
{
    if (!s || !out || !valid_app(app)) return -EINVAL;
    Task *t = find_task(s, task_id);
    if (!t) return -ENOENT;
    if (t->finished) return -ESHUTDOWN;
    /* Restrict this first integration to the registered component. Cross-app
     * Activities or helper-process admission require explicit launcher proof.
     */
    if (t->user != app->user_id || t->guest_uid != app->guest_uid ||
        strcmp(t->bundle, app->bundle) || strcmp(t->ability, app->ability))
        return -EPERM;
    Process *p;
    int result = process_get(s, identity, &p);
    if (result) return result;
    if (p != t->process) { collect_processes(s); return -EPERM; }
    if (s->last_activity == INT32_MAX) return -ENOSPC;
    Activity *a = calloc(1, sizeof(*a));
    if (!a) return -ENOMEM;
    a->id = ++s->last_activity;
    a->task = t; a->process = p;
    a->next = s->activities;
    s->activities = a;
    *out = a->id;
    return 0;
}

int anco_linux_surface_bind(AncoLinuxOwnership *s, int32_t activity_id,
                            const void *client, const void *surface,
                            pid_t pid, uid_t uid, gid_t gid)
{
    if (!s || !client || !surface || pid <= 0) return -EINVAL;
    Activity *a = find_activity(s, activity_id);
    if (!a) return -ENOENT;
    if (a->task->finished || !process_alive(a->process)) return -ESRCH;
    const AncoLinuxProcess *p = &a->process->identity;
    if (p->pid != pid || p->uid != uid || p->gid != gid) return -EPERM;
    for (Surface *w = s->surfaces; w; w = w->next) {
        if (w->client == client && w->object == surface)
            return w->activity == a ? 0 : -EEXIST;
    }
    Surface *w = calloc(1, sizeof(*w));
    if (!w) return -ENOMEM;
    w->client = client; w->object = surface; w->activity = a;
    w->process = a->process;
    w->next = s->surfaces;
    s->surfaces = w;
    return 0;
}

static int process_parent(pid_t pid, pid_t *out)
{
    char path[64], line[4096];
    snprintf(path,sizeof(path),"/proc/%ld/stat",(long)pid);
    FILE *f=fopen(path,"re");
    if(!f)return -errno;
    char *got=fgets(line,sizeof(line),f);fclose(f);
    if(!got)return -ESRCH;
    char *end=strrchr(line,')');
    if(!end || end[1]!=' ' || !end[2])return -EPROTO;
    char *save=NULL;
    char *state=strtok_r(end+2," \n",&save);
    char *parent=strtok_r(NULL," \n",&save);
    if(!state || !parent || !*parent)return -EPROTO;
    errno=0;char *tail=NULL;long value=strtol(parent,&tail,10);
    if(errno || !tail || *tail || value<0 || value>INT_MAX)return -EPROTO;
    *out=(pid_t)value;return 0;
}

int anco_linux_process_is_descendant(const AncoLinuxProcess *ancestor,
    const AncoLinuxProcess *candidate)
{
    pid_t current=candidate->pid;
    for(unsigned depth=0;depth<256;++depth){
        if(current==ancestor->pid){
            AncoLinuxProcess actual;
            const int status=anco_linux_process_read(current,&actual);
            return status ? status :
                (identity_equal(&actual,ancestor)?0:-ESTALE);
        }
        pid_t parent=0;
        const int status=process_parent(current,&parent);
        if(status)return status;
        if(parent<=1 || parent==current)return -EPERM;
        current=parent;
    }
    return -ELOOP;
}

int anco_linux_surface_bind_descendant(AncoLinuxOwnership *s,
    int32_t activity_id,const void *client,const void *surface,
    const AncoLinuxProcess *identity)
{
    if(!s || !client || !surface || !identity)return -EINVAL;
    Activity *a=find_activity(s,activity_id);
    if(!a)return -ENOENT;
    if(a->task->finished || !process_alive(a->process))return -ESRCH;
    if(identity->uid!=a->process->identity.uid ||
       identity->gid!=a->process->identity.gid)return -EPERM;
    int result=anco_linux_process_is_descendant(&a->process->identity,identity);
    if(result)return result;
    Process *process=NULL;
    result=process_get(s,identity,&process);
    if(result)return result;
    for(Surface *w=s->surfaces;w;w=w->next){
        if(w->client!=client || w->object!=surface)continue;
        return w->activity==a && w->process==process ? 0 : -EEXIST;
    }
    Surface *w=calloc(1,sizeof(*w));
    if(!w){collect_processes(s);return -ENOMEM;}
    w->client=client;w->object=surface;w->activity=a;w->process=process;
    w->next=s->surfaces;s->surfaces=w;
    return 0;
}

int anco_linux_surface_owner(AncoLinuxOwnership *s, const void *client,
                             const void *surface, AncoLinuxSurfaceOwner *out)
{
    if (!s || !client || !surface || !out) return -EINVAL;
    for (Surface *w = s->surfaces; w; w = w->next) {
        if (w->client == client && w->object == surface) {
            Activity *a = w->activity;
            Task *t = a->task;
            if (t->finished || !process_alive(a->process) ||
                !process_alive(w->process)) return -ESRCH;
            *out = (AncoLinuxSurfaceOwner){t->user, t->id, a->id, t->guest_uid,
                                         w->process->identity};
            return 0;
        }
    }
    return -ENOENT;
}

static void retire_surface_windows(AncoLinuxOwnership *s, const Surface *surface)
{
    Window **w = &s->windows;
    while (*w) {
        if ((*w)->surface == surface) {
            Window *old = *w; *w = old->next; free(old);
        } else w = &(*w)->next;
    }
}

int anco_linux_windows_configure(AncoLinuxOwnership *s, int32_t value)
{
    if (!s || value < 0 || (int64_t)value * 100000 >= INT32_MAX)
        return -EINVAL;
    const int32_t base = (int32_t)((int64_t)value * 100000);
    if (s->windows_configured) return base == s->window_base ? 0 : -EALREADY;
    s->window_base = s->last_window = base;
    s->windows_configured = true;
    return 0;
}

static Window *find_window(AncoLinuxOwnership *s, const void *client,
                            const void *object)
{
    for (Window *w = s->windows; w; w = w->next)
        if (w->surface->client == client && w->object == object) return w;
    return NULL;
}

static int next_window(AncoLinuxOwnership *s, int32_t *out)
{
    int32_t first = 0;
    for (;;) {
        /* Java int addition wraps negative at INT_MAX. Avoid C signed UB. */
        s->last_window = s->last_window == INT32_MAX ?
            s->window_base + 1 : s->last_window + 1;
        const int32_t id = s->last_window;
        /* Bounded failure only when the whole positive range is occupied;
         * original WMS would keep searching indefinitely in this case. */
        if (id == first) return -ENOSPC;
        if (!first) first = id;
        bool occupied = false;
        for (Window *w = s->windows; w && !occupied; w = w->next)
            occupied = w->id == id;
        if (!occupied) { *out = id; return 0; }
    }
}

int anco_linux_window_reserve_transient(AncoLinuxOwnership *s,
                                        int32_t activity_id, int32_t *out)
{
    if (!s || !out) return -EINVAL;
    if (!s->windows_configured) return -ENODATA;
    if (!find_activity(s, activity_id)) return -ENOENT;
    return next_window(s, out);
}

int anco_linux_window_add(AncoLinuxOwnership *s, const void *client,
                          const void *object, const void *surface,
                          AncoLinuxWindowOwner *out)
{
    if (!s || !client || !object || !surface || !out) return -EINVAL;
    if (!s->windows_configured) return -ENODATA;
    AncoLinuxSurfaceOwner owner;
    int error = anco_linux_surface_owner(s, client, surface, &owner);
    if (error) return error;
    Window *existing = find_window(s, client, object);
    if (existing) {
        if (existing->surface->object != surface) return -EEXIST;
        *out = (AncoLinuxWindowOwner){existing->id, owner};
        return 0;
    }
    Surface *binding = NULL;
    for (Surface *p = s->surfaces; p; p = p->next)
        if (p->client == client && p->object == surface) { binding = p; break; }
    for (Window *w = s->windows; w; w = w->next)
        if (w->surface == binding) return -EEXIST;
    Window *w = calloc(1, sizeof(*w));
    if (!w) return -ENOMEM;
    error = next_window(s, &w->id);
    if (error) { free(w); return error; }
    w->object = object; w->surface = binding;
    w->next = s->windows; s->windows = w;
    *out = (AncoLinuxWindowOwner){w->id, owner};
    return 0;
}

int anco_linux_window_owner(AncoLinuxOwnership *s, const void *client,
                            const void *object, AncoLinuxWindowOwner *out)
{
    if (!s || !client || !object || !out) return -EINVAL;
    Window *w = find_window(s, client, object);
    if (!w) return -ENOENT;
    AncoLinuxSurfaceOwner owner;
    int error = anco_linux_surface_owner(s, client, w->surface->object, &owner);
    if (error) return error;
    *out = (AncoLinuxWindowOwner){w->id, owner};
    return 0;
}

int anco_linux_window_remove(AncoLinuxOwnership *s, const void *client,
                             const void *object)
{
    if (!s || !client || !object) return -EINVAL;
    Window **w = &s->windows;
    while (*w) {
        if ((*w)->surface->client == client && (*w)->object == object) {
            Window *old = *w; *w = old->next; free(old); return 0;
        }
        w = &(*w)->next;
    }
    return -ENOENT;
}

int anco_linux_surface_remove(AncoLinuxOwnership *s, const void *client,
                              const void *surface)
{
    if (!s || !client || !surface) return -EINVAL;
    Surface **w = &s->surfaces;
    while (*w) {
        if ((*w)->client == client && (*w)->object == surface) {
            Surface *old = *w; *w = old->next;
            retire_surface_windows(s, old); free(old); collect_processes(s); return 0;
        }
        w = &(*w)->next;
    }
    return -ENOENT;
}

void anco_linux_client_disconnected(AncoLinuxOwnership *s, const void *client)
{
    if (!s || !client) return;
    Surface **w = &s->surfaces;
    while (*w) {
        if ((*w)->client == client) {
            Surface *old = *w; *w = old->next;
            retire_surface_windows(s, old); free(old); collect_processes(s);
        } else w = &(*w)->next;
    }
}

int anco_linux_activity_remove(AncoLinuxOwnership *s, int32_t id)
{
    if (!s) return -EINVAL;
    Activity **a = &s->activities;
    while (*a && (*a)->id != id) a = &(*a)->next;
    if (!*a) return -ENOENT;
    Activity *old = *a;
    Surface **w = &s->surfaces;
    while (*w) {
        if ((*w)->activity == old) {
            Surface *gone = *w; *w = gone->next;
            retire_surface_windows(s, gone); free(gone);
        } else w = &(*w)->next;
    }
    *a = old->next;
    free(old);
    return 0;
}

int anco_linux_task_finish(AncoLinuxOwnership *s, int32_t id)
{
    if (!s) return -EINVAL;
    Task *t = find_task(s, id);
    if (!t) return -ENOENT;
    Activity *a = s->activities;
    while (a) {
        Activity *next = a->next;
        if (a->task == t) anco_linux_activity_remove(s, a->id);
        a = next;
    }
    t->finished = true;
    t->process = NULL;
    collect_processes(s);
    return 0;
}

int anco_linux_task_remove_recent(AncoLinuxOwnership *s, int32_t id)
{
    if (!s) return -EINVAL;
    Task **t = &s->tasks;
    while (*t && (*t)->id != id) t = &(*t)->next;
    if (!*t) return -ENOENT;
    /* Removing a recent entry must not silently tear down a running Activity. */
    if (!(*t)->finished) return -EBUSY;
    Task *old = *t; *t = old->next;
    free(old->bundle); free(old->ability); free(old);
    collect_processes(s);
    return 0;
}

int anco_linux_task_alive(AncoLinuxOwnership *s, int32_t id)
{
    if (!s) return -EINVAL;
    Task *t = find_task(s, id);
    if (!t) return -ENOENT;
    return !t->finished && process_alive(t->process);
}

unsigned anco_linux_ownership_reap(AncoLinuxOwnership *s)
{
    if (!s) return 0;
    unsigned removed = 0;
    Activity *a = s->activities;
    while (a) {
        Activity *next = a->next;
        if (!process_alive(a->process)) {
            anco_linux_activity_remove(s, a->id);
            ++removed;
        }
        a = next;
    }
    for (Task *t = s->tasks; t; t = t->next) {
        if (!t->finished && !process_alive(t->process)) {
            t->finished = true;
            t->process = NULL;
        }
    }
    collect_processes(s);
    return removed;
}

void anco_linux_ownership_free(AncoLinuxOwnership *s)
{
    if (!s) return;
    while (s->activities) anco_linux_activity_remove(s, s->activities->id);
    while (s->tasks) {
        s->tasks->finished = true;
        anco_linux_task_remove_recent(s, s->tasks->id);
    }
    while (s->processes) {
        Process *p = s->processes; s->processes = p->next;
        if (p->pidfd >= 0) close(p->pidfd);
        free(p);
    }
    while (s->cursors) {
        Cursor *c = s->cursors; s->cursors = c->next; free(c);
    }
    free(s);
}
