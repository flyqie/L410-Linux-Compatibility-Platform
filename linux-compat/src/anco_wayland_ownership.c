#include "anco_wayland_ownership.h"
#include <wayland-server-protocol.h>
#include <errno.h>
#include <string.h>

typedef struct Client Client;
typedef struct Surface Surface;
typedef struct Window Window;
struct Surface {
    Client *client;
    struct wl_resource *resource;
    struct wl_listener destroy;
    struct wl_list link;
    Window *window;
    gint32 activity;
    AncoLinuxProcess process;
};
struct Window {
    Surface *surface;
    const void *object;
    struct wl_resource *resource;
    struct wl_listener destroy;
    struct wl_listener role_destroy;
    struct wl_signal *role_lifetime;
    AncoLinuxWindowOwner creation;
};
struct Client {
    AncoWaylandOwnership *adapter;
    struct wl_client *client;
    struct wl_listener destroy;
    struct wl_list surfaces, link;
};
struct AncoWaylandOwnership {
    AncoLinuxCatalog *catalog;
    struct wl_list clients;
    guint count;
    guint windows;
    AncoWaylandWindowRetireFunc retire;
    void *retire_data;
    AncoWaylandClientRetireFunc client_retire;
    void *client_retire_data;
    gboolean retiring;
};

static void forget_window(Window *w)
{
    Surface *s = w->surface;
    AncoWaylandOwnership *a = s->client->adapter;
    if (a->retire) {
        a->retiring = TRUE;
        a->retire(a,(struct wl_resource *)w->object,s->resource,
            &w->creation,a->retire_data);
        a->retiring = FALSE;
    }
    /* Actual process exit may already have retired the catalog record. */
    anco_linux_catalog_window_remove(a->catalog, s->client->client, w->object);
    if (w->resource) wl_list_remove(&w->destroy.link);
    if (w->role_lifetime) wl_list_remove(&w->role_destroy.link);
    s->window = NULL;
    --a->windows;
    g_free(w);
}
static void window_destroyed(struct wl_listener *listener, void *data)
{
    (void)data;
    Window *w = wl_container_of(listener, w, destroy);
    forget_window(w);
}
static void role_destroyed(struct wl_listener *listener, void *data)
{
    (void)data;
    Window *w = wl_container_of(listener, w, role_destroy);
    forget_window(w);
}
static void forget_surface(Surface *s)
{
    AncoWaylandOwnership *a = s->client->adapter;
    if (s->window) forget_window(s->window);
    /* Process exit may already have removed the catalog binding. */
    anco_linux_catalog_remove_surface(a->catalog, s->client->client, s->resource);
    wl_list_remove(&s->destroy.link);
    wl_list_remove(&s->link);
    --a->count;
    g_free(s);
}
static void surface_destroyed(struct wl_listener *listener, void *data)
{
    (void)data;
    Surface *s = wl_container_of(listener, s, destroy);
    forget_surface(s);
}
static void forget_client(Client *c, gboolean disconnected)
{
    Surface *s, *next;
    wl_list_for_each_safe(s, next, &c->surfaces, link) forget_surface(s);
    if (disconnected) anco_linux_catalog_disconnect(c->adapter->catalog, c->client);
    AncoWaylandOwnership *a = c->adapter;
    if (a->client_retire) {
        a->retiring = TRUE;
        a->client_retire(a, c->client, disconnected, a->client_retire_data);
        a->retiring = FALSE;
    }
    wl_list_remove(&c->destroy.link);
    wl_list_remove(&c->link);
    g_free(c);
}
static void client_destroyed(struct wl_listener *listener, void *data)
{
    (void)data;
    Client *c = wl_container_of(listener, c, destroy);
    /* Wayland 1.21 calls this BEFORE destroying the client's resources. Detach
     * their listeners while all resources remain live; no order dependency. */
    forget_client(c, TRUE);
}
static Client *find_client(AncoWaylandOwnership *a, struct wl_client *client)
{
    Client *c;
    wl_list_for_each(c, &a->clients, link) if (c->client == client) return c;
    return NULL;
}
static Surface *find_surface(Client *c, struct wl_resource *resource)
{
    if (c) {
        Surface *s;
        wl_list_for_each(s, &c->surfaces, link) if (s->resource == resource) return s;
    }
    return NULL;
}
static int bind_real(AncoWaylandOwnership *a, struct wl_resource *resource,
                     gint32 activity, AncoLinuxSurfaceOwner *out)
{
    struct wl_client *client = wl_resource_get_client(resource);
    pid_t pid = -1; uid_t uid = (uid_t)-1; gid_t gid = (gid_t)-1;
    wl_client_get_credentials(client, &pid, &uid, &gid);
    return activity ? anco_linux_catalog_bind_surface_for_activity(a->catalog, activity,
        client, resource, pid, uid, gid, out) :
        anco_linux_catalog_bind_surface(a->catalog, client, resource, pid, uid, gid, out);
}
static int bind_process(AncoWaylandOwnership *a, struct wl_resource *resource,
                        gint32 activity, const AncoLinuxProcess *process,
                        AncoLinuxSurfaceOwner *out)
{
    if (!process) return bind_real(a, resource, activity, out);
    struct wl_client *client = wl_resource_get_client(resource);
    return anco_linux_catalog_bind_surface_for_process(a->catalog,
        activity,client,resource,process,out);
}
AncoWaylandOwnership *anco_wayland_ownership_new(AncoLinuxCatalog *catalog)
{
    if (!catalog) return NULL;
    AncoWaylandOwnership *a = g_new0(AncoWaylandOwnership, 1);
    a->catalog = catalog;
    wl_list_init(&a->clients);
    return a;
}
void anco_wayland_ownership_free(AncoWaylandOwnership *a)
{
    if (!a) return;
    /* A borrowed retirement callback cannot destroy its owning traversal. */
    if (a->retiring) return;
    Client *c, *next;
    wl_list_for_each_safe(c, next, &a->clients, link) forget_client(c, FALSE);
    g_free(a);
}
int anco_wayland_ownership_on_window_retire(AncoWaylandOwnership *a,
    AncoWaylandWindowRetireFunc callback, void *data)
{
    if (!a) return -EINVAL;
    if (a->retiring || a->windows) return -EBUSY;
    if (callback && a->retire) return -EALREADY;
    if (!callback && a->retire && a->retire_data != data) return -EPERM;
    a->retire=callback;a->retire_data=callback ? data : NULL;
    return 0;
}
int anco_wayland_ownership_on_client_retire(AncoWaylandOwnership *a,
    AncoWaylandClientRetireFunc callback, void *data)
{
    if (!a) return -EINVAL;
    if (a->retiring || !wl_list_empty(&a->clients)) return -EBUSY;
    if (callback && a->client_retire) return -EALREADY;
    if (!callback && a->client_retire && a->client_retire_data != data) return -EPERM;
    a->client_retire = callback;
    a->client_retire_data = callback ? data : NULL;
    return 0;
}
static int track_activity(AncoWaylandOwnership *a, struct wl_resource *resource,
                           gint32 activity, const AncoLinuxProcess *process,
                           AncoLinuxSurfaceOwner *out)
{
    if (!a || !resource || !out) return -EINVAL;
    if (a->retiring) return -EBUSY;
    if (strcmp(wl_resource_get_class(resource), wl_surface_interface.name)) return -EPROTOTYPE;
    struct wl_client *client = wl_resource_get_client(resource);
    Client *c = find_client(a, client);
    Surface *existing = find_surface(c, resource);
    if (existing && activity && existing->activity != activity) return -EEXIST;
    if (existing && process &&
        (existing->process.pid != process->pid ||
         existing->process.uid != process->uid ||
         existing->process.gid != process->gid ||
         existing->process.start_ticks != process->start_ticks)) return -ESTALE;
    int error = bind_process(a, resource, existing ? existing->activity : activity,
        existing ? &existing->process : process, out);
    if (error) return error;
    if (existing) return 0;
    if (!c) {
        c = g_new0(Client, 1); c->adapter = a; c->client = client;
        wl_list_init(&c->surfaces); wl_list_insert(&a->clients, &c->link);
        c->destroy.notify = client_destroyed;
        wl_client_add_destroy_listener(client, &c->destroy);
    }
    Surface *s = g_new0(Surface, 1); s->client = c; s->resource = resource;
    s->activity = out->activity_id;
    s->process = out->process;
    wl_list_insert(&c->surfaces, &s->link);
    s->destroy.notify = surface_destroyed;
    wl_resource_add_destroy_listener(resource, &s->destroy);
    ++a->count;
    return 0;
}
int anco_wayland_ownership_track(AncoWaylandOwnership *a,
                                 struct wl_resource *resource, AncoLinuxSurfaceOwner *out)
{
    return track_activity(a, resource, 0, NULL, out);
}
int anco_wayland_ownership_track_activity(AncoWaylandOwnership *a,
    struct wl_resource *resource, gint32 activity, AncoLinuxSurfaceOwner *out)
{
    return activity > 0 ? track_activity(a, resource, activity, NULL, out) : -EINVAL;
}
int anco_wayland_ownership_track_activity_process(AncoWaylandOwnership *a,
    struct wl_resource *resource, gint32 activity,
    const AncoLinuxProcess *process, AncoLinuxSurfaceOwner *out)
{
    return activity >= 0 && process ?
        track_activity(a, resource, activity, process, out) : -EINVAL;
}
int anco_wayland_ownership_owner(AncoWaylandOwnership *a,
                                 struct wl_resource *resource,
                                 AncoLinuxSurfaceOwner *out)
{
    if (!a || !resource || !out) return -EINVAL;
    if (a->retiring) return -EBUSY;
    Client *c = find_client(a, wl_resource_get_client(resource));
    Surface *s = find_surface(c, resource);
    return s ? bind_process(a, resource, s->activity, &s->process, out) : -ENOENT;
}
guint anco_wayland_ownership_surfaces(AncoWaylandOwnership *a)
{
    return a ? a->count : 0;
}

static Window *find_window(Client *c, struct wl_resource *role)
{
    if (c) {
        Surface *s;
        wl_list_for_each(s, &c->surfaces, link)
            if (s->window && s->window->object == role) return s->window;
    }
    return NULL;
}
static int track_window(AncoWaylandOwnership *a,
                                       struct wl_resource *role,
                                       struct wl_resource *resource,
                                       struct wl_signal *lifetime,
                                       AncoLinuxWindowOwner *out)
{
    if (!a || !role || !resource || !out) return -EINVAL;
    if (a->retiring) return -EBUSY;
    const char *type = wl_resource_get_class(role);
    if (strcmp(type, "xdg_toplevel") && strcmp(type, "xdg_popup")) return -EPROTOTYPE;
    if (strcmp(wl_resource_get_class(resource), wl_surface_interface.name)) return -EPROTOTYPE;
    struct wl_client *client = wl_resource_get_client(role);
    if (client != wl_resource_get_client(resource)) return -EXDEV;
    Client *c = find_client(a, client);
    Surface *s = find_surface(c, resource);
    if (!s) return -ENOENT;
    Window *existing = find_window(c, role);
    if (existing) {
        if (existing->surface != s) return -EEXIST;
        if (lifetime && existing->role_lifetime != lifetime) return -EEXIST;
        return anco_linux_catalog_window_owner(a->catalog, client, role, out);
    }
    if (s->window) return -EEXIST;
    Window *w = g_try_new0(Window, 1);
    if (!w) return -ENOMEM;
    AncoLinuxWindowOwner owner;
    const int error = anco_linux_catalog_window_add(a->catalog, client, role, resource, &owner);
    if (error) { g_free(w); return error; }
    w->surface = s; w->object = role; w->resource = role;
    w->destroy.notify = window_destroyed;
    w->creation = owner;
    wl_resource_add_destroy_listener(role, &w->destroy);
    w->role_lifetime = lifetime;
    if (lifetime) {
        w->role_destroy.notify = role_destroyed;
        wl_signal_add(lifetime, &w->role_destroy);
    }
    s->window = w;
    ++a->windows;
    *out = owner;
    return 0;
}
int anco_wayland_ownership_track_window_key_lifetime(AncoWaylandOwnership *a,
    const void *role, struct wl_resource *surface,
    struct wl_signal *lifetime, AncoLinuxWindowOwner *out)
{
    if (!a || !role || !surface || !lifetime || !out) return -EINVAL;
    if (a->retiring) return -EBUSY;
    if (strcmp(wl_resource_get_class(surface), wl_surface_interface.name))
        return -EPROTOTYPE;
    struct wl_client *client = wl_resource_get_client(surface);
    Client *c = find_client(a, client);
    Surface *s = find_surface(c, surface);
    if (!s) return -ENOENT;
    Window *existing = find_window(c, (struct wl_resource *)role);
    if (existing) {
        if (existing->surface != s || existing->role_lifetime != lifetime)
            return -EEXIST;
        return anco_linux_catalog_window_owner(a->catalog, client, role, out);
    }
    if (s->window) return -EEXIST;
    Window *w = g_try_new0(Window, 1);
    if (!w) return -ENOMEM;
    AncoLinuxWindowOwner owner;
    const int error = anco_linux_catalog_window_add(a->catalog, client,
        role, surface, &owner);
    if (error) { g_free(w); return error; }
    w->surface = s; w->object = role; w->role_lifetime = lifetime;
    w->creation = owner; w->role_destroy.notify = role_destroyed;
    wl_signal_add(lifetime, &w->role_destroy);
    s->window = w; ++a->windows; *out = owner;
    return 0;
}
int anco_wayland_ownership_track_window(AncoWaylandOwnership *a,
    struct wl_resource *role, struct wl_resource *surface, AncoLinuxWindowOwner *out)
{
    return track_window(a, role, surface, NULL, out);
}
int anco_wayland_ownership_track_window_lifetime(AncoWaylandOwnership *a,
    struct wl_resource *role, struct wl_resource *surface,
    struct wl_signal *lifetime, AncoLinuxWindowOwner *out)
{
    return lifetime ? track_window(a, role, surface, lifetime, out) : -EINVAL;
}
int anco_wayland_ownership_window_owner(AncoWaylandOwnership *a,
                                        struct wl_resource *role,
                                        AncoLinuxWindowOwner *out)
{
    if (!a || !role || !out) return -EINVAL;
    if (a->retiring) return -EBUSY;
    struct wl_client *client = wl_resource_get_client(role);
    return find_window(find_client(a, client), role) ?
        anco_linux_catalog_window_owner(a->catalog, client, role, out) : -ENOENT;
}
int anco_wayland_ownership_surface_window(AncoWaylandOwnership *a,
    struct wl_resource *resource, AncoLinuxWindowOwner *out)
{
    if (!a || !resource || !out) return -EINVAL;
    if (a->retiring) return -EBUSY;
    if (strcmp(wl_resource_get_class(resource), wl_surface_interface.name)) return -EPROTOTYPE;
    struct wl_client *client = wl_resource_get_client(resource);
    Surface *s = find_surface(find_client(a, client), resource);
    if (!s || !s->window) return -ENOENT;
    AncoLinuxWindowOwner owner;
    int error = anco_linux_catalog_window_owner(a->catalog, client,
        s->window->object, &owner);
    if (!error) *out = owner;
    return error;
}

guint anco_wayland_ownership_windows(AncoWaylandOwnership *a)
{
    return a ? a->windows : 0;
}

static gboolean same_window_owner(const AncoLinuxWindowOwner *a,
                                  const AncoLinuxWindowOwner *b)
{
    const AncoLinuxSurfaceOwner *x = &a->owner, *y = &b->owner;
    return a->window_id == b->window_id && x->user_id == y->user_id &&
        x->task_id == y->task_id && x->activity_id == y->activity_id &&
        x->guest_uid == y->guest_uid && x->process.pid == y->process.pid &&
        x->process.uid == y->process.uid && x->process.gid == y->process.gid &&
        x->process.start_ticks == y->process.start_ticks;
}

int anco_wayland_ownership_resolve_window(AncoWaylandOwnership *a,
    const AncoLinuxWindowOwner *identity, struct wl_resource **role,
    struct wl_resource **surface)
{
    if (!a || !identity || !role || !surface || role == surface) return -EINVAL;
    if (a->retiring) return -EBUSY;
    Client *c;
    wl_list_for_each(c, &a->clients, link) {
        Surface *s;
        wl_list_for_each(s, &c->surfaces, link) {
            Window *w = s->window;
            if (!w || w->creation.window_id != identity->window_id) continue;
            if (!same_window_owner(&w->creation, identity)) return -ESTALE;
            AncoLinuxWindowOwner current;
            int error = anco_linux_catalog_window_owner(a->catalog, c->client,
                                                        w->object, &current);
            if (error) return error;
            if (!same_window_owner(&current, identity)) return -ESTALE;
            if (!w->resource) return -EPROTOTYPE;
            *role = w->resource;
            *surface = s->resource;
            return 0;
        }
    }
    return -ENOENT;
}

int anco_wayland_ownership_task_windows(AncoWaylandOwnership *a,
    gint32 task, AncoLinuxWindowOwner **windows, guint *count)
{
    if (!a || task <= 0 || !windows || !count) return -EINVAL;
    if (a->retiring) return -EBUSY;
    AncoLinuxLaunch live;
    int error = anco_linux_catalog_task(a->catalog, task, &live);
    if (error) return error;
    AncoLinuxWindowOwner *snapshot = a->windows ?
        g_try_new(AncoLinuxWindowOwner, a->windows) : NULL;
    if (a->windows && !snapshot) return -ENOMEM;
    guint used = 0;
    Client *c;
    wl_list_for_each(c, &a->clients, link) {
        Surface *s;
        wl_list_for_each(s, &c->surfaces, link) {
            Window *w = s->window;
            if (!w || w->creation.owner.task_id != task) continue;
            AncoLinuxWindowOwner current;
            error = anco_linux_catalog_window_owner(a->catalog, c->client,
                                                    w->object, &current);
            if (!error && !same_window_owner(&w->creation, &current)) error = -ESTALE;
            if (error) { g_free(snapshot); return error; }
            snapshot[used++] = current;
        }
    }
    if (!used) { g_free(snapshot); snapshot = NULL; }
    *windows = snapshot;
    *count = used;
    return 0;
}
