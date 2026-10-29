#define _GNU_SOURCE
#include "anco_linux_catalog.h"
#include <json-glib/json-glib.h>
#include <gio/gio.h>
#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

typedef struct {
    AncoLinuxApp identity;
    JsonObject *metadata;
    char **argv;
    char *cwd;
} Entry;
typedef struct {
    char *bundle;
    gint32 app_id;
    char *payload;
    gsize length;
    gboolean accepted;
    GHashTable *entries;
} Package;
typedef struct {
    AncoLinuxCatalog *catalog;
    Entry *entry;
    AncoLinuxLaunch launch;
    GArray *tasks; /* admitted Task IDs, not a window-tree traversal order */
    guint watch;
    gboolean adopted; /* not spawned/reaped by this catalog */
} Instance;
typedef struct {
    AncoLinuxLaunch launch;
    gboolean finished;
    gboolean ordinary_launcher;
    Instance *instance; /* live direct-child owner; NULL after its wait event */
} TaskRecord;
struct AncoLinuxCatalog {
    char *prefix;
    gint32 user;
    gboolean privacy;
    GHashTable *packages;
    GPtrArray *instances;
    GHashTable *task_records;
    GHashTable *activities;
    AncoLinuxExitFunc exited;
    void *exit_data;
    guint exit_depth;
    guint creation_depth;
    AncoLinuxActivityCreatedFunc activity_created;
    void *creation_data;
    gint32 exit_task;
    GArray *exit_activities; /* borrowed current callback snapshot */
    AncoLinuxOwnership *ownership;
};

static void instance_free(gpointer data)
{
    Instance *instance = data;
    g_array_unref(instance->tasks);
    g_free(instance);
}

static void task_record_free(gpointer data)
{
    TaskRecord *record = data;
    g_free((char *)record->launch.app.bundle);
    g_free((char *)record->launch.app.ability);
    g_free(record);
}

static void entry_free(gpointer data)
{
    Entry *e = data;
    g_free((char *)e->identity.bundle);
    g_free((char *)e->identity.ability);
    g_strfreev(e->argv);
    g_free(e->cwd);
    json_object_unref(e->metadata);
    g_free(e);
}
static void package_free(gpointer data)
{
    Package *p = data;
    g_hash_table_unref(p->entries);
    g_free(p->bundle);
    g_free(p->payload);
    g_free(p);
}
static JsonObject *object_member(JsonObject *object, const char *name)
{
    JsonNode *node = object ? json_object_get_member(object, name) : NULL;
    return node && JSON_NODE_HOLDS_OBJECT(node) ? json_node_get_object(node) : NULL;
}
static JsonArray *array_member(JsonObject *object, const char *name)
{
    JsonNode *node = object ? json_object_get_member(object, name) : NULL;
    return node && JSON_NODE_HOLDS_ARRAY(node) ? json_node_get_array(node) : NULL;
}
static const char *string_node(JsonNode *node)
{
    return node && JSON_NODE_HOLDS_VALUE(node) &&
        json_node_get_value_type(node) == G_TYPE_STRING ? json_node_get_string(node) : NULL;
}
static gboolean int_member(JsonObject *object, const char *name, gint64 *value)
{
    JsonNode *node = object ? json_object_get_member(object, name) : NULL;
    if (!node || !JSON_NODE_HOLDS_VALUE(node) || json_node_get_value_type(node) != G_TYPE_INT64)
        return FALSE;
    *value = json_node_get_int(node);
    return TRUE;
}
static gboolean contains_string(JsonArray *array, const char *value)
{
    if (!array) return FALSE;
    for (guint i = 0; i < json_array_get_length(array); ++i) {
        const char *entry = string_node(json_array_get_element(array, i));
        if (entry && !strcmp(entry, value)) return TRUE;
    }
    return FALSE;
}

AncoLinuxCatalog *anco_linux_catalog_new(const char *prefix, gint32 user,
                                        gboolean privacy)
{
    /* Original guest package UID is an appId; runtime guest UID uses100000.
     * Do not confuse either with host IPC UID or the Linux child's credential. */
    if (!prefix || !*prefix || prefix[strlen(prefix) - 1] != '.' || user < 0 ||
        (gint64)user * 100000 + 19999 > G_MAXINT32) return NULL;
    AncoLinuxCatalog *c = g_new0(AncoLinuxCatalog, 1);
    c->prefix = g_strdup(prefix); c->user = user; c->privacy = privacy;
    c->packages = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, package_free);
    c->instances = g_ptr_array_new_with_free_func(instance_free);
    c->task_records = g_hash_table_new_full(g_direct_hash, g_direct_equal,
                                           NULL, task_record_free);
    c->activities = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, g_free);
    c->ownership = anco_linux_ownership_new();
    if (!c->ownership) { anco_linux_catalog_free(c); return NULL; }
    return c;
}
int anco_linux_catalog_free(AncoLinuxCatalog *c)
{
    if (!c) return 0;
    if (c->instances->len || c->exited || c->exit_depth ||
        c->activity_created || c->creation_depth) return -EBUSY;
    anco_linux_ownership_free(c->ownership);
    g_ptr_array_unref(c->instances);
    g_hash_table_unref(c->activities);
    g_hash_table_unref(c->task_records);
    g_hash_table_unref(c->packages);
    g_free(c->prefix); g_free(c);
    return 0;
}

int anco_linux_catalog_on_exit(AncoLinuxCatalog *c, AncoLinuxExitFunc callback,
                               void *data)
{
    if (!c) return -EINVAL;
    if (c->exit_depth || c->creation_depth) return -EBUSY;
    if (callback && c->exited) return -EALREADY;
    if (!callback && c->exited && c->exit_data != data) return -EPERM;
    c->exited = callback;
    c->exit_data = callback ? data : NULL;
    return 0;
}

int anco_linux_catalog_on_activity_created(AncoLinuxCatalog *c,
    AncoLinuxActivityCreatedFunc observer, void *data)
{
    if (!c) return -EINVAL;
    if (c->exit_depth || c->creation_depth) return -EBUSY;
    if (observer && c->activity_created) return -EALREADY;
    if (!observer && c->activity_created && c->creation_data != data) return -EPERM;
    c->activity_created = observer;
    c->creation_data = observer ? data : NULL;
    return 0;
}

static void activity_committed(AncoLinuxCatalog *c, const AncoLinuxLaunch *activity)
{
    if (c->activity_created) {
        ++c->creation_depth;
        c->activity_created(c, activity, c->creation_data);
        --c->creation_depth;
    }
}

int anco_linux_catalog_stage(AncoLinuxCatalog *c, const void *data, gsize length)
{
    if (!c || !data) return -EINVAL;
    if (!length || length > ANCO_BMS_MAX_BYTES) return -EMSGSIZE;
    if (!g_utf8_validate(data, (gssize)length, NULL)) return -EILSEQ;
    JsonParser *parser = json_parser_new();
    GError *parse_error = NULL;
    int error = -EPROTO;
    Package *p = NULL;
    if (!json_parser_load_from_data(parser, data, (gssize)length, &parse_error) ||
        json_parser_has_assignment(parser, NULL)) goto done;
    JsonNode *root_node = json_parser_get_root(parser);
    if (!root_node || !JSON_NODE_HOLDS_OBJECT(root_node)) goto done;
    JsonObject *root = json_node_get_object(root_node);
    JsonObject *app = object_member(root, "applicationInfo");
    JsonObject *modules = object_member(root, "moduleInfos");
    JsonObject *module = object_member(modules, "entry");
    JsonObject *abilities = object_member(root, "abilityInfos");
    JsonObject *skills = object_member(root, "skillInfos");
    const char *bundle = app ? string_node(json_object_get_member(app, "bundleName")) : NULL;
    const char *module_name = module ? string_node(json_object_get_member(module, "moduleName")) : NULL;
    JsonArray *keys = array_member(module, "abilityKeys");
    gint64 uid, type;
    if (!app || !abilities || !skills || !module_name || strcmp(module_name, ".entry") ||
        !keys || !bundle || !g_str_has_prefix(bundle, c->prefix) || !bundle[strlen(c->prefix)] ||
        !int_member(app, "virtualAppType", &type) || type != 1 ||
        !int_member(app, "uid", &uid) || uid < 10000 || uid > 19999 ||
        json_object_get_size(modules) != 1 || !json_object_get_size(abilities)) goto done;
    if (c->privacy) {
        JsonArray *users = array_member(app, "userIds");
        gboolean present = FALSE;
        for (guint i = 0; users && i < json_array_get_length(users); ++i) {
            JsonNode *n = json_array_get_element(users, i);
            if (JSON_NODE_HOLDS_VALUE(n) && json_node_get_value_type(n) == G_TYPE_INT64 &&
                json_node_get_int(n) == c->user) present = TRUE;
        }
        if (!present) goto done;
    }
    if (g_hash_table_contains(c->packages, bundle)) { error = -EEXIST; goto done; }
    GHashTableIter iter;
    gpointer value;
    g_hash_table_iter_init(&iter, c->packages);
    while (g_hash_table_iter_next(&iter, NULL, &value)) {
        if (((Package *)value)->app_id == uid) { error = -EADDRINUSE; goto done; }
    }
    p = g_new0(Package, 1); p->bundle = g_strdup(bundle); p->app_id = (gint32)uid;
    p->payload = g_memdup2(data, length); p->length = length;
    p->entries = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, entry_free);
    GList *names = json_object_get_members(abilities);
    for (GList *n = names; n; n = n->next) {
        const char *ability = n->data;
        if (!*ability || !contains_string(keys, ability) || !object_member(abilities, ability)) {
            g_list_free(names); goto done;
        }
        Entry *e = g_new0(Entry, 1);
        e->metadata = json_object_ref(object_member(abilities, ability));
        e->identity = (AncoLinuxApp){c->user, c->user * 100000 + (gint32)uid,
                                    g_strdup(bundle), g_strdup(ability)};
        g_hash_table_insert(p->entries, (gpointer)e->identity.ability, e);
    }
    g_list_free(names);
    for (guint i = 0; i < json_array_get_length(keys); ++i) {
        const char *ability = string_node(json_array_get_element(keys, i));
        if (!ability || !g_hash_table_contains(p->entries, ability)) goto done;
    }
    names = json_object_get_members(skills);
    for (GList *n = names; n; n = n->next) {
        if (!g_hash_table_contains(p->entries, n->data) || !array_member(skills, n->data)) {
            g_list_free(names); goto done;
        }
    }
    g_list_free(names);
    g_hash_table_insert(c->packages, p->bundle, p); p = NULL; error = 0;
done:
    if (p) package_free(p);
    g_clear_error(&parse_error);
    g_object_unref(parser);
    return error;
}

int anco_linux_catalog_command(AncoLinuxCatalog *c, const char *bundle,
                               const char *ability, const char *const *argv,
                               const char *cwd)
{
    if (!c || !bundle || !ability || !argv || !argv[0] || argv[0][0] != '/' ||
        !cwd || cwd[0] != '/') return -EINVAL;
    Package *p = g_hash_table_lookup(c->packages, bundle);
    Entry *e = p ? g_hash_table_lookup(p->entries, ability) : NULL;
    if (!e) return -ENOENT;
    if (p->accepted) return -EBUSY;
    if (access(argv[0], X_OK) || access(cwd, X_OK)) return -errno;
    if (!g_file_test(argv[0], G_FILE_TEST_IS_REGULAR) ||
        !g_file_test(cwd, G_FILE_TEST_IS_DIR)) return -EINVAL;
    g_strfreev(e->argv); g_free(e->cwd);
    e->argv = g_strdupv((char **)argv); e->cwd = g_strdup(cwd);
    return 0;
}
static void local_result(AncoBmsRegistrationResult *r, int error)
{
    if (r) *r = (AncoBmsRegistrationResult){error, -1, FALSE, -1};
}
gboolean anco_linux_catalog_publish(AncoLinuxCatalog *c, GBinderRemoteObject *bms,
                                    const char *bundle, AncoBmsRegistrationResult *r)
{
    Package *p = c && bundle ? g_hash_table_lookup(c->packages, bundle) : NULL;
    if (!p || !r) { local_result(r, -ENOENT); return FALSE; }
    if (p->accepted) { local_result(r, -EALREADY); return FALSE; }
    GHashTableIter iter; gpointer value;
    g_hash_table_iter_init(&iter, p->entries);
    while (g_hash_table_iter_next(&iter, NULL, &value)) {
        if (!((Entry *)value)->argv) { local_result(r, -ENODATA); return FALSE; }
    }
    if (!anco_bms_register(bms, p->payload, p->length, r)) return FALSE;
    p->accepted = TRUE;
    return TRUE;
}

static gboolean same_string(JsonObject *a, const char *ak, JsonObject *b, const char *bk)
{
    const char *av = string_node(json_object_get_member(a, ak));
    const char *bv = string_node(json_object_get_member(b, bk));
    return av && bv && !strcmp(av, bv);
}
static gboolean same_integer(JsonObject *a, const char *key, JsonObject *b)
{
    gint64 av, bv;
    return int_member(a, key, &av) && int_member(b, key, &bv) && av == bv;
}
static gboolean same_boolean(JsonObject *a, const char *key, JsonObject *b)
{
    JsonNode *av = json_object_get_member(a, key), *bv = json_object_get_member(b, key);
    return av && bv && JSON_NODE_HOLDS_VALUE(av) && JSON_NODE_HOLDS_VALUE(bv) &&
        json_node_get_value_type(av) == G_TYPE_BOOLEAN &&
        json_node_get_value_type(bv) == G_TYPE_BOOLEAN &&
        json_node_get_boolean(av) == json_node_get_boolean(bv);
}

static gboolean recover_matches(Package *p, JsonObject *record)
{
    JsonParser *source = json_parser_new();
    gboolean matches = FALSE;
    if (!json_parser_load_from_data(source, p->payload, p->length, NULL)) goto done;
    JsonObject *app = object_member(json_node_get_object(json_parser_get_root(source)), "applicationInfo");
    gint64 uid;
    if (!app || !same_string(record, "name", app, "bundleName") ||
        !int_member(record, "uid", &uid) || uid != p->app_id ||
        !same_integer(record, "versionCode", app) ||
        !same_string(record, "versionName", app, "versionName") ||
        !same_integer(record, "installTime", app) ||
        !same_integer(record, "updateTime", app)) goto done;
    JsonArray *modules = array_member(record, "hapModuleInfos");
    if (!modules || json_array_get_length(modules) != 1) goto done;
    JsonNode *module_node = json_array_get_element(modules, 0);
    if (!JSON_NODE_HOLDS_OBJECT(module_node)) goto done;
    JsonObject *module = json_node_get_object(module_node);
    const char *module_name = string_node(json_object_get_member(module, "name"));
    if (!module_name || strcmp(module_name, ".entry")) goto done;
    JsonArray *abilities = array_member(module, "abilityInfos");
    if (!abilities || json_array_get_length(abilities) != g_hash_table_size(p->entries)) goto done;
    GHashTable *seen = g_hash_table_new(g_str_hash, g_str_equal);
    for (guint i = 0; i < json_array_get_length(abilities); ++i) {
        JsonNode *node = json_array_get_element(abilities, i);
        if (!JSON_NODE_HOLDS_OBJECT(node)) goto entries_done;
        JsonObject *a = json_node_get_object(node);
        const char *name = string_node(json_object_get_member(a, "name"));
        Entry *e = name ? g_hash_table_lookup(p->entries, name) : NULL;
        if (!e || g_hash_table_contains(seen, name)) goto entries_done;
        g_hash_table_add(seen, (gpointer)name);
        if (!same_string(a, "bundleName", app, "bundleName") ||
            !same_integer(a, "launchMode", e->metadata) ||
            !same_integer(a, "orientation", e->metadata) ||
            !same_boolean(a, "visible", e->metadata) ||
            !same_boolean(a, "enabled", e->metadata)) goto entries_done;
        JsonArray *actual_modes = array_member(a, "supportWindowMode");
        JsonArray *expected_modes = array_member(e->metadata, "supportWindowModes");
        if (!actual_modes || !expected_modes ||
            json_array_get_length(actual_modes) != json_array_get_length(expected_modes)) goto entries_done;
        for (guint m = 0; m < json_array_get_length(actual_modes); ++m) {
            if (!json_node_equal(json_array_get_element(actual_modes, m),
                                 json_array_get_element(expected_modes, m))) goto entries_done;
        }
        const char *permission = string_node(json_object_get_member(e->metadata, "permission"));
        JsonArray *permissions = array_member(a, "permissions");
        if (!permission || !permissions ||
            json_array_get_length(permissions) != (guint)(*permission ? 1 : 0) ||
            (*permission && !contains_string(permissions, permission))) goto entries_done;
    }
    matches = TRUE;
entries_done:
    g_hash_table_unref(seen);
done:
    g_object_unref(source);
    return matches;
}

gboolean anco_linux_catalog_recover(AncoLinuxCatalog *c, GBinderRemoteObject *bms,
    const char *bundle, gint32 host_user, const char *decoder, AncoBmsRegistrationResult *r)
{
    Package *p = c && bundle ? g_hash_table_lookup(c->packages, bundle) : NULL;
    if (!p || !r || !decoder || decoder[0] != '/') { local_result(r, -EINVAL); return FALSE; }
    if (c->privacy || c->user != 0 || host_user != 100) { local_result(r, -ENOTSUP); return FALSE; }
    /* A newly staged package is independent of already running packages.
     * Recovery is serialized by the runtime main loop, and existing Instance
     * objects retain their own Package/Entry values. Only callback reentry or
     * an already accepted target package makes this admission unsafe. */
    if (p->accepted || c->exit_depth || c->creation_depth) {
        local_result(r, -EBUSY); return FALSE;
    }
    GHashTableIter iter; gpointer value;
    g_hash_table_iter_init(&iter, p->entries);
    while (g_hash_table_iter_next(&iter, NULL, &value)) {
        if (!((Entry *)value)->argv) { local_result(r, -ENODATA); return FALSE; }
    }
    GBinderRemoteReply *reply = anco_bms_query_bundle(bms, bundle, 7, host_user, r);
    if (!reply) return FALSE;
    if (r->service_result) { gbinder_remote_reply_unref(reply); return FALSE; }
    GBinderReader reader;
    gbinder_remote_reply_init_reader(reply, &reader);
    gsize length = 0;
    const void *data = gbinder_reader_get_data(&reader, &length);
    if (!data || length < 16 || length > 32768 + 12) {
        r->local_error = -EMSGSIZE; gbinder_remote_reply_unref(reply); return FALSE;
    }
    GBytes *input = g_bytes_new(data, length), *output = NULL, *diagnostic = NULL;
    GError *error = NULL;
    GSubprocess *child = g_subprocess_new(G_SUBPROCESS_FLAGS_STDIN_PIPE |
        G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_PIPE, &error, decoder, NULL);
    gboolean ok = child && g_subprocess_communicate(child, input, NULL, &output, &diagnostic, &error) &&
        g_subprocess_get_successful(child);
    r->local_error = ok ? -EPROTO : -EIO;
    JsonParser *parser = json_parser_new();
    if (ok && output) {
        const char *json = g_bytes_get_data(output, &length);
        if (json_parser_load_from_data(parser, json, length, NULL)) {
            JsonNode *root = json_parser_get_root(parser);
            if (JSON_NODE_HOLDS_OBJECT(root) && recover_matches(p, json_node_get_object(root))) {
                p->accepted = TRUE;
                r->local_error = 0;
            } else r->local_error = -ESTALE;
        }
    }
    g_object_unref(parser);
    g_clear_error(&error);
    if (child) g_object_unref(child);
    g_bytes_unref(input);
    if (output) g_bytes_unref(output);
    if (diagnostic) g_bytes_unref(diagnostic);
    gbinder_remote_reply_unref(reply);
    return p->accepted;
}
gboolean anco_linux_catalog_unpublish(AncoLinuxCatalog *c, GBinderRemoteObject *bms,
                                      const char *bundle, AncoBmsRegistrationResult *r)
{
    Package *p = c && bundle ? g_hash_table_lookup(c->packages, bundle) : NULL;
    if (!p || !p->accepted || !r) { local_result(r, -ENOENT); return FALSE; }
    for (guint i = 0; i < c->instances->len; ++i) {
        Instance *instance = g_ptr_array_index(c->instances, i);
        if (!strcmp(instance->entry->identity.bundle, bundle)) {
            local_result(r, -EBUSY); return FALSE;
        }
    }
    if (!anco_bms_unregister(bms, p->bundle, c->privacy, c->user, r)) return FALSE;
    p->accepted = FALSE;
    return TRUE;
}
const AncoLinuxApp *anco_linux_catalog_registered(AncoLinuxCatalog *c,
                                                  const char *bundle, const char *ability)
{
    Package *p = c && bundle ? g_hash_table_lookup(c->packages, bundle) : NULL;
    Entry *e = p && p->accepted && ability ? g_hash_table_lookup(p->entries, ability) : NULL;
    return e ? &e->identity : NULL;
}

static int retire_task_identity(AncoLinuxCatalog *c, TaskRecord *record)
{
    const gint32 task = record->launch.task_id;
    const int result = anco_linux_task_finish(c->ownership, task);
    if (result) return result;
    GHashTableIter activities; gpointer value;
    g_hash_table_iter_init(&activities, c->activities);
    while (g_hash_table_iter_next(&activities, NULL, &value)) {
        if (((AncoLinuxLaunch *)value)->task_id == task)
            g_hash_table_iter_remove(&activities);
    }
    record->finished = TRUE;
    record->instance = NULL;
    return 0;
}

int anco_linux_catalog_exit_activities(AncoLinuxCatalog *c, gint32 task,
    const AncoLinuxLaunch **activities, guint *count)
{
    if (!c || !activities || !count || task <= 0) return -EINVAL;
    if (!c->exit_depth || !c->exit_activities) return -EPERM;
    if (task != c->exit_task) return -ENOENT;
    *activities = (const AncoLinuxLaunch *)c->exit_activities->data;
    *count = c->exit_activities->len;
    return 0;
}

static void child_exited(GPid pid, gint status, gpointer data)
{
    Instance *instance = data; AncoLinuxCatalog *c = instance->catalog;
    const AncoLinuxLaunch process_exit = instance->launch;
    GPtrArray *records = g_ptr_array_new();
    GPtrArray *snapshots = g_ptr_array_new_with_free_func((GDestroyNotify)g_array_unref);
    for (guint i = 0; i < instance->tasks->len; ++i) {
        const gint32 task = g_array_index(instance->tasks, gint32, i);
        TaskRecord *record = g_hash_table_lookup(c->task_records, GINT_TO_POINTER(task));
        g_assert(record != NULL && record->instance == instance && !record->finished);
        /* Snapshot before live Activity records disappear. These identities
         * are not a claim that the corresponding host windows were exported.
         * Their strings belong to retained TaskRecords, not removed entries. */
        GArray *snapshot = g_array_new(FALSE, FALSE, sizeof(AncoLinuxLaunch));
        GHashTableIter iter; gpointer value;
        g_hash_table_iter_init(&iter, c->activities);
        while (g_hash_table_iter_next(&iter, NULL, &value)) {
            const AncoLinuxLaunch *activity = value;
            if (activity->task_id == task) g_array_append_val(snapshot, *activity);
        }
        g_ptr_array_add(snapshots, snapshot);
        const int result = retire_task_identity(c, record);
        if (result) g_printerr("ANCO_LINUX_CHILD_RETIRE_FAILED pid=%ld task=%d status=%d error=%d\n",
            (long)pid, task, status, result);
        g_assert(result == 0);
        g_ptr_array_add(records, record);
        g_print("ANCO_LINUX_CHILD_EXIT pid=%ld task=%d status=%d\n", (long)pid,
                task, status);
    }
    g_spawn_close_pid(pid);
    g_ptr_array_remove(c->instances, instance);
    if (c->exited) {
        ++c->exit_depth;
        if (!records->len) {
            /* A valid headless launch may terminate before creating any
             * top-level window.  Report the real process exit without
             * fabricating a Task/Activity merely to carry this callback. */
            c->exit_task = -1;
            c->exit_activities = NULL;
            c->exited(c, &process_exit, status, c->exit_data);
        }
        for (guint i = 0; i < records->len; ++i) {
            const TaskRecord *record = g_ptr_array_index(records, i);
            /* A synchronous notification can dispatch another real child
             * watch. Keep the outer callback's still-owned snapshot intact. */
            const gint32 outer_task = c->exit_task;
            GArray *outer_activities = c->exit_activities;
            c->exit_task = record->launch.task_id;
            c->exit_activities = g_ptr_array_index(snapshots, i);
            c->exited(c, &record->launch, status, c->exit_data);
            c->exit_activities = outer_activities;
            c->exit_task = outer_task;
        }
        --c->exit_depth;
    }
    g_ptr_array_unref(snapshots);
    g_ptr_array_unref(records);
}

static TaskRecord *retain_task(AncoLinuxCatalog *c, Instance *instance,
                                const AncoLinuxLaunch *launch)
{
    /* Retained Task identity outlives the direct child and mutable metadata. */
    TaskRecord *record = g_new0(TaskRecord, 1);
    record->launch = *launch;
    record->launch.app.bundle = g_strdup(launch->app.bundle);
    record->launch.app.ability = g_strdup(launch->app.ability);
    record->instance = instance;
    g_assert(!g_hash_table_contains(c->task_records, GINT_TO_POINTER(launch->task_id)));
    g_hash_table_insert(c->task_records, GINT_TO_POINTER(launch->task_id), record);
    g_hash_table_insert(c->activities, GINT_TO_POINTER(launch->activity_id),
                        g_memdup2(&record->launch, sizeof(record->launch)));
    g_array_append_val(instance->tasks, launch->task_id);
    g_print("ANCO_LINUX_TASK_RETAIN pid=%ld task=%d activity=%d task_count=%u\n",
        (long)launch->process.pid, launch->task_id, launch->activity_id,
        instance->tasks->len);
    /* The first real window Task becomes the default surface association for
     * this process.  Later independent roots allocate their own Tasks before
     * binding, so this is only a compatibility/default lookup, not a rule
     * that one process owns one window. */
    if (instance->launch.task_id <= 0) instance->launch = record->launch;
    return record;
}
static void abort_child(GPid child)
{
    /* Exclusive unreaped child ownership keeps its numeric PID reserved. */
    kill(child, SIGKILL);
    while (waitpid(child, NULL, 0) < 0 && errno == EINTR) {}
    g_spawn_close_pid(child);
}

static Entry *entry_from_inherited_launch(AncoLinuxCatalog *c, pid_t pid)
{
    char path[64];
    const int path_length=snprintf(path,sizeof(path),"/proc/%ld/environ",(long)pid);
    if(path_length<=0 || (size_t)path_length>=sizeof(path))return NULL;
    gchar *environment=NULL;gsize length=0;
    if(!g_file_get_contents(path,&environment,&length,NULL) || length>65536){
        g_free(environment);return NULL;
    }
    const char *bundle=NULL,*ability=NULL;
    static const char bundle_key[]="L410_ANCO_BUNDLE=";
    static const char ability_key[]="L410_ANCO_ABILITY=";
    for(gsize offset=0;offset<length;){
        const char *item=environment+offset;
        const gsize remaining=length-offset;
        const char *end=memchr(item,'\0',remaining);
        const gsize item_length=end?(gsize)(end-item):remaining;
        if(item_length>=sizeof(bundle_key)-1 &&
           !memcmp(item,bundle_key,sizeof(bundle_key)-1))
            bundle=item+sizeof(bundle_key)-1;
        else if(item_length>=sizeof(ability_key)-1 &&
                !memcmp(item,ability_key,sizeof(ability_key)-1))
            ability=item+sizeof(ability_key)-1;
        offset+=item_length+(end!=NULL);
    }
    Entry *selected=NULL;
    Package *package=bundle&&*bundle?g_hash_table_lookup(c->packages,bundle):NULL;
    if(package&&package->accepted&&ability&&*ability)
        selected=g_hash_table_lookup(package->entries,ability);
    gboolean active_launch=FALSE;
    for(guint i=0;selected&&i<c->instances->len;++i){
        Instance *instance=g_ptr_array_index(c->instances,i);
        if(!instance->adopted&&instance->entry==selected){active_launch=TRUE;break;}
    }
    if(!active_launch)selected=NULL;
    if(selected)g_print("ANCO_LINUX_PROCESS_LAUNCH_PROVENANCE pid=%ld bundle=%s ability=%s active_registered_launch=1\n",
        (long)pid,selected->identity.bundle,selected->identity.ability);
    g_free(environment);
    return selected;
}

static int start_process_instance(AncoLinuxCatalog *c, const char *bundle,
    const char *ability, Instance **out)
{
    if (!c || !out) return -EINVAL;
    if (c->exit_depth || c->creation_depth) return -EBUSY;
    const AncoLinuxApp *app = anco_linux_catalog_registered(c, bundle, ability);
    if (!app) return -ENOENT;
    Package *p = g_hash_table_lookup(c->packages, bundle);
    Entry *entry = g_hash_table_lookup(p->entries, ability);
    GPid child = 0; GError *error = NULL;
    gchar **environment = g_get_environ();
    environment = g_environ_setenv(environment, "L410_ANCO_BUNDLE",
                                    app->bundle, TRUE);
    environment = g_environ_setenv(environment, "L410_ANCO_ABILITY",
                                    app->ability, TRUE);
    if (!g_spawn_async(entry->cwd, entry->argv, environment,
                       G_SPAWN_DO_NOT_REAP_CHILD,
                       NULL, NULL, &child, &error)) {
        g_strfreev(environment); g_clear_error(&error); return -ENOEXEC;
    }
    g_strfreev(environment);
    AncoLinuxLaunch launch = {.app = *app, .task_id = -1, .activity_id = -1};
    int result = anco_linux_process_read(child, &launch.process);
    if (result) {
        abort_child(child); return result;
    }
    Instance *instance = g_new0(Instance, 1);
    instance->catalog = c; instance->entry = entry; instance->launch = launch;
    instance->tasks = g_array_new(FALSE, FALSE, sizeof(gint32));
    g_ptr_array_add(c->instances, instance);
    instance->watch = g_child_watch_add(child, child_exited, instance);
    *out = instance;
    return 0;
}

int anco_linux_catalog_start_process(AncoLinuxCatalog *c, const char *bundle,
    const char *ability, AncoLinuxProcess *out)
{
    if (!out) return -EINVAL;
    Instance *instance = NULL;
    const int result = start_process_instance(c, bundle, ability, &instance);
    if (!result) *out = instance->launch.process;
    return result;
}

int anco_linux_catalog_start_new(AncoLinuxCatalog *c, const char *bundle,
                                 const char *ability, AncoLinuxLaunch *out)
{
    if (!out) return -EINVAL;
    Instance *instance = NULL;
    int result = start_process_instance(c, bundle, ability, &instance);
    if (!result) result = anco_linux_catalog_task_create_for_process(c,
        &instance->launch.process, out);
    if (result && instance) {
        const GPid child = instance->launch.process.pid;
        if (instance->watch) g_source_remove(instance->watch);
        g_ptr_array_remove(c->instances, instance);
        abort_child(child);
    }
    return result;
}

int anco_linux_catalog_task_create_in_process(AncoLinuxCatalog *c,
    gint32 source_task, AncoLinuxLaunch *out)
{
    if (!c || !out || source_task <= 0) return -EINVAL;
    if (c->exit_depth || c->creation_depth) return -EBUSY;
    AncoLinuxLaunch source;
    int result = anco_linux_catalog_task(c, source_task, &source);
    if (result) return result;
    const AncoLinuxApp *app = anco_linux_catalog_registered(c,
        source.app.bundle, source.app.ability);
    if (!app) return -ENOENT;
    TaskRecord *origin = g_hash_table_lookup(c->task_records, GINT_TO_POINTER(source_task));
    g_assert(origin != NULL && origin->instance != NULL);
    AncoLinuxLaunch launch = {.app = *app, .process = source.process,
                              .task_id = -1, .activity_id = -1};
    result = anco_linux_task_create(c->ownership, app, &launch.process, &launch.task_id);
    if (!result && launch.task_id <= 0) result = -ENOSPC;
    if (!result) result = anco_linux_activity_create(c->ownership, launch.task_id,
        app, &launch.process, &launch.activity_id);
    if (result) {
        if (launch.task_id >= 0) anco_linux_task_finish(c->ownership, launch.task_id);
        return result; /* the existing child belongs to other Tasks; never kill */
    }
    const AncoLinuxLaunch committed = retain_task(c, origin->instance, &launch)->launch;
    activity_committed(c, &committed);
    *out = committed;
    return 0;
}

int anco_linux_catalog_task_create_for_process(AncoLinuxCatalog *c,
    const AncoLinuxProcess *process, AncoLinuxLaunch *out)
{
    if(!c || !process || !out || process->pid<=0 || !process->start_ticks)
        return -EINVAL;
    if(c->exit_depth || c->creation_depth)return -EBUSY;
    AncoLinuxProcess current;
    int result=anco_linux_process_read(process->pid,&current);
    if(result)return result;
    if(current.pid!=process->pid || current.uid!=process->uid ||
       current.gid!=process->gid || current.start_ticks!=process->start_ticks)
        return -ESTALE;
    Instance *selected=NULL;
    for(guint i=0;i<c->instances->len;++i){
        Instance *instance=g_ptr_array_index(c->instances,i);
        const AncoLinuxProcess *owned=&instance->launch.process;
        const gboolean exact=owned->pid==process->pid &&
            owned->uid==process->uid && owned->gid==process->gid &&
            owned->start_ticks==process->start_ticks;
        if(!exact && anco_linux_process_is_descendant(owned,process))continue;
        if(selected)return -ENOTUNIQ;
        selected=instance;
    }
    if(!selected)return -ESRCH;
    const AncoLinuxApp *app=&selected->entry->identity;
    AncoLinuxLaunch launch={.app=*app,.process=*process,
        .task_id=-1,.activity_id=-1};
    result=anco_linux_task_create(c->ownership,app,&launch.process,&launch.task_id);
    if(!result && launch.task_id<=0)result=-ENOSPC;
    if(!result)result=anco_linux_activity_create(c->ownership,launch.task_id,
        app,&launch.process,&launch.activity_id);
    if(result){
        if(launch.task_id>=0)anco_linux_task_finish(c->ownership,launch.task_id);
        return result;
    }
    const AncoLinuxLaunch committed=retain_task(c,selected,&launch)->launch;
    activity_committed(c,&committed);
    *out=committed;
    return 0;
}

int anco_linux_catalog_task_adopt_registered_process(AncoLinuxCatalog *c,
    const AncoLinuxProcess *process,AncoLinuxLaunch *out)
{
    if(!c || !process || !out || process->pid<=0 || !process->start_ticks)
        return -EINVAL;
    if(c->exit_depth || c->creation_depth)return -EBUSY;
    AncoLinuxProcess current;
    int result=anco_linux_process_read(process->pid,&current);
    if(result){
        g_print("ANCO_LINUX_PROCESS_ADOPT_REJECT pid=%ld stage=process-read status=%d\n",
            (long)process->pid,result);
        return result;
    }
    if(current.pid!=process->pid || current.uid!=process->uid ||
       current.gid!=process->gid || current.start_ticks!=process->start_ticks)
        return -ESTALE;
    Entry *selected=entry_from_inherited_launch(c,process->pid);
    if(!selected){
        g_print("ANCO_LINUX_PROCESS_ADOPT_REJECT pid=%ld stage=launch-provenance status=%d\n",
            (long)process->pid,-ESRCH);
        return -ESRCH;
    }
    Instance *instance=g_new0(Instance,1);
    instance->catalog=c;instance->entry=selected;instance->adopted=TRUE;
    instance->launch=(AncoLinuxLaunch){.app=selected->identity,
        .process=*process,.task_id=-1,.activity_id=-1};
    instance->tasks=g_array_new(FALSE,FALSE,sizeof(gint32));
    g_ptr_array_add(c->instances,instance);
    const AncoLinuxApp *app=&selected->identity;
    AncoLinuxLaunch launch={.app=*app,.process=*process,
        .task_id=-1,.activity_id=-1};
    result=anco_linux_task_create(c->ownership,app,&launch.process,&launch.task_id);
    if(!result && launch.task_id<=0)result=-ENOSPC;
    if(!result)result=anco_linux_activity_create(c->ownership,launch.task_id,
        app,&launch.process,&launch.activity_id);
    if(result){
        g_print("ANCO_LINUX_PROCESS_ADOPT_REJECT pid=%ld stage=task-activity-create status=%d task=%d activity=%d bundle=%s\n",
            (long)process->pid,result,launch.task_id,launch.activity_id,
            app->bundle);
        if(launch.task_id>=0)anco_linux_task_finish(c->ownership,launch.task_id);
        g_ptr_array_remove(c->instances,instance);
        return result;
    }
    const AncoLinuxLaunch committed=retain_task(c,instance,&launch)->launch;
    activity_committed(c,&committed);
    g_print("ANCO_LINUX_PROCESS_ADOPT pid=%ld task=%d activity=%d bundle=%s source=inherited-launch\n",
        (long)process->pid,committed.task_id,committed.activity_id,
        committed.app.bundle);
    *out=committed;
    return 0;
}

static int standard_launcher_entry(Entry *entry, gint32 user)
{
    gint64 mode;
    /* Original mapper: Android mode0 -> host STANDARD1 (uniquely). Never
     * infer guest mode2 vs3 from host SINGLETON0 or from a process singleton.
     * Require explicit fields rather than guessing native omission defaults.
     */
    if (!int_member(entry->metadata, "launchMode", &mode)) return -ENODATA;
    if (mode != 1) return -ENOTSUP;
    JsonNode *enabled = json_object_get_member(entry->metadata, "enabled");
    if (!enabled || !JSON_NODE_HOLDS_VALUE(enabled) ||
        json_node_get_value_type(enabled) != G_TYPE_BOOLEAN) return -ENODATA;
    JsonArray *disabled = array_member(entry->metadata, "disabledUserIds");
    if (json_object_has_member(entry->metadata, "disabledUserIds") && !disabled)
        return -EPROTO;
    /* Stock AbilityInfoExt.isEnabled: a populated disabled-user set takes
     * precedence; enabled Boolean is its empty-set fallback. No global writes.
     */
    if (disabled && json_array_get_length(disabled)) {
        for (guint i = 0; i < json_array_get_length(disabled); ++i) {
            JsonNode *node = json_array_get_element(disabled, i);
            if (!JSON_NODE_HOLDS_VALUE(node) ||
                json_node_get_value_type(node) != G_TYPE_INT64) return -EPROTO;
            if (json_node_get_int(node) == user) return -EACCES;
        }
    } else if (!json_node_get_boolean(enabled)) return -EACCES;
    const char *permission = string_node(json_object_get_member(entry->metadata, "permission"));
    if (!permission) return -ENODATA;
    if (*permission) return -ENOTSUP; /* guest permission resolver missing */
    return 0;
}

int anco_linux_catalog_select_launcher(AncoLinuxCatalog *c, const char *bundle,
                                      const char *ability, gint32 hint,
                                      AncoLinuxLaunch *out, gboolean *created)
{
    if (!c || !out || !created) return -EINVAL;
    if (c->exit_depth || c->creation_depth) return -EBUSY;
    const AncoLinuxApp *app = anco_linux_catalog_registered(c, bundle, ability);
    if (!app) return -ENOENT;
    Package *package = g_hash_table_lookup(c->packages, bundle);
    Entry *entry = g_hash_table_lookup(package->entries, ability);
    const int admissible = standard_launcher_entry(entry, c->user);
    if (admissible) return admissible;
    TaskRecord *selected = NULL;
    if (hint != -1) {
        /* getReusableTask explicit branch resolves that ID or null, then
         * RETURNS immediately. A missing explicit ID is not an implicit
         * component/package reuse request. Retained recent task restoration
         * is a separate unfinished branch, not permission to change its ID.
         */
        selected = g_hash_table_lookup(c->task_records, GINT_TO_POINTER(hint));
        if (selected && (!selected->ordinary_launcher || selected->finished))
            return -ENOTSUP;
        if (selected && (strcmp(selected->launch.app.bundle, bundle) ||
                         strcmp(selected->launch.app.ability, ability)))
            return -ENOTSUP; /* add/reparent Activity in existing Task missing */
    } else {
        GHashTableIter iter; gpointer value;
        g_hash_table_iter_init(&iter, c->task_records);
        while (g_hash_table_iter_next(&iter, NULL, &value)) {
            TaskRecord *record = value;
            if (record->finished) continue;
            if (strcmp(record->launch.app.bundle, bundle)) continue;
            if (strcmp(record->launch.app.ability, ability)) continue;
            /* The original launcher filter searches for a reusable Task of
             * this exact component.  A Task belonging to another component,
             * or a semantic window Task created without this launcher Want,
             * is not a global rejection condition.  It is simply not a
             * candidate for this component's ordinary launcher root. */
            if (!record->ordinary_launcher) continue;
            /* For this ordinary canonical filter, realActivity/component and
             * null document-data are an ideal match. Multiple ideals require
             * actual display/front-order lifecycle, not hash/PID/ID ordering.
             */
            if (selected) return -ENOTSUP;
            selected = record;
        }
    }
    if (selected) {
        AncoLinuxLaunch actual;
        const int live = anco_linux_catalog_task(c, selected->launch.task_id, &actual);
        if (live) return live;
        /* Selecting a surviving sibling/top Activity after root retirement
         * requires original task-order policy, not the launch snapshot. */
        if (!g_hash_table_contains(c->activities, GINT_TO_POINTER(actual.activity_id)))
            return -ENOTSUP;
        *out = actual; /* original same-filter root branch, no second exec */
        *created = FALSE;
        return 0;
    }
    AncoLinuxLaunch actual;
    const int started = anco_linux_catalog_start_new(c, bundle, ability, &actual);
    if (started) return started;
    TaskRecord *record = g_hash_table_lookup(c->task_records, GINT_TO_POINTER(actual.task_id));
    g_assert(record != NULL);
    record->ordinary_launcher = TRUE;
    *out = actual;
    *created = TRUE;
    return 0;
}
int anco_linux_catalog_start_launcher(AncoLinuxCatalog *c, const char *bundle,
    const char *ability, gint32 hint, AncoLinuxLaunch *out)
{
    gboolean created;
    return anco_linux_catalog_select_launcher(c, bundle, ability, hint, out, &created);
}
int anco_linux_catalog_bind_surface(AncoLinuxCatalog *c, const void *client,
                                    const void *surface, pid_t pid, uid_t uid, gid_t gid,
                                    AncoLinuxSurfaceOwner *out)
{
    if (!c || !out) return -EINVAL;
    AncoLinuxSurfaceOwner existing;
    int known = anco_linux_surface_owner(c->ownership, client, surface, &existing);
    if (!known) return anco_linux_catalog_bind_surface_for_activity(c, existing.activity_id,
        client, surface, pid, uid, gid, out);
    if (known != -ENOENT) return known;
    for (guint i = 0; i < c->instances->len; ++i) {
        Instance *instance = g_ptr_array_index(c->instances, i);
        if (instance->launch.process.pid != pid) continue;
        if (instance->launch.activity_id <= 0) return -ENOENT;
        return anco_linux_catalog_bind_surface_for_activity(c, instance->launch.activity_id,
            client, surface, pid, uid, gid, out);
    }
    return -ESRCH;
}
int anco_linux_catalog_task(AncoLinuxCatalog *c, gint32 task, AncoLinuxLaunch *out)
{
    if (!c || !out || task <= 0) return -EINVAL;
    TaskRecord *record = g_hash_table_lookup(c->task_records, GINT_TO_POINTER(task));
    if (!record || record->finished || !record->instance) return -ENOENT;
    int alive = anco_linux_task_alive(c->ownership, task);
    if (alive <= 0) return alive < 0 ? alive : -ESRCH;
    *out = record->launch;
    return 0;
}

int anco_linux_catalog_task_window_modes(AncoLinuxCatalog *c, gint32 task,
    gint32 modes[4], guint *count)
{
    if (!c || !modes || !count || task <= 0) return -EINVAL;
    AncoLinuxLaunch live;
    const int result = anco_linux_catalog_task(c, task, &live);
    if (result) return result;
    Package *package = g_hash_table_lookup(c->packages, live.app.bundle);
    Entry *entry = package ? g_hash_table_lookup(package->entries,
        live.app.ability) : NULL;
    JsonArray *array = entry ? array_member(entry->metadata,
        "supportWindowModes") : NULL;
    const guint length = array ? json_array_get_length(array) : 0;
    if (!length || length > 4) return -ENODATA;
    gint32 staged[4] = {0};
    guint seen = 0;
    for (guint i = 0; i < length; ++i) {
        JsonNode *node = json_array_get_element(array, i);
        if (!JSON_NODE_HOLDS_VALUE(node) ||
            json_node_get_value_type(node) != G_TYPE_INT64) return -EPROTO;
        const gint64 value = json_node_get_int(node);
        if (value < 0 || value > 3 || (seen & (1u << value))) return -EPROTO;
        seen |= 1u << value;
        staged[i] = (gint32)value;
    }
    memcpy(modes, staged, sizeof(staged));
    *count = length;
    return 0;
}
int anco_linux_catalog_task_record(AncoLinuxCatalog *c, gint32 task,
                                   AncoLinuxLaunch *out, gboolean *finished)
{
    if (!c || !out || !finished || task <= 0) return -EINVAL;
    TaskRecord *record = g_hash_table_lookup(c->task_records, GINT_TO_POINTER(task));
    if (!record) return -ENOENT;
    *out = record->launch;
    *finished = record->finished;
    return 0;
}
int anco_linux_catalog_task_retire(AncoLinuxCatalog *c, gint32 task)
{
    if (!c || task <= 0) return -EINVAL;
    if (c->exit_depth || c->creation_depth) return -EBUSY;
    TaskRecord *record = g_hash_table_lookup(c->task_records, GINT_TO_POINTER(task));
    if (!record) return -ENOENT;
    if (record->finished) return 0;
    Instance *instance = record->instance;
    g_assert(instance != NULL);
    for (guint i = 0; i < instance->tasks->len; ++i) {
        if (g_array_index(instance->tasks, gint32, i) != task) continue;
        const int result = retire_task_identity(c, record);
        if (result) return result;
        g_array_remove_index(instance->tasks, i);
        if(instance->adopted && !instance->tasks->len)
            g_ptr_array_remove(c->instances,instance);
        return 0;
    }
    g_assert_not_reached();
    return -ENOENT;
}
int anco_linux_catalog_activity_create(AncoLinuxCatalog *c, gint32 task,
                                       AncoLinuxLaunch *out)
{
    if (!c || !out) return -EINVAL;
    if (c->exit_depth || c->creation_depth) return -EBUSY;
    AncoLinuxLaunch live;
    int result = anco_linux_catalog_task(c, task, &live);
    if (result) return result;
    const AncoLinuxApp *app = anco_linux_catalog_registered(c,
        live.app.bundle, live.app.ability);
    if (!app) return -ENOENT;
    result = anco_linux_activity_create(c->ownership, task, app,
        &live.process, &live.activity_id);
    if (result) return result;
    TaskRecord *record = g_hash_table_lookup(c->task_records, GINT_TO_POINTER(task));
    live.app = record->launch.app; /* durable identity, not mutable metadata */
    g_hash_table_insert(c->activities, GINT_TO_POINTER(live.activity_id),
                        g_memdup2(&live, sizeof(live)));
    activity_committed(c, &live);
    *out = live;
    return 0;
}
int anco_linux_catalog_activity(AncoLinuxCatalog *c, gint32 activity,
                                AncoLinuxLaunch *out)
{
    if (!c || !out || activity <= 0) return -EINVAL;
    AncoLinuxLaunch *record = g_hash_table_lookup(c->activities, GINT_TO_POINTER(activity));
    if (!record) return -ENOENT;
    const int alive = anco_linux_task_alive(c->ownership, record->task_id);
    if (alive <= 0) return alive < 0 ? alive : -ESRCH;
    *out = *record;
    return 0;
}
int anco_linux_catalog_activity_remove(AncoLinuxCatalog *c, gint32 activity)
{
    if (!c || activity <= 0) return -EINVAL;
    if (c->exit_depth || c->creation_depth) return -EBUSY;
    if (!g_hash_table_contains(c->activities, GINT_TO_POINTER(activity))) return -ENOENT;
    int result = anco_linux_activity_remove(c->ownership, activity);
    if (!result) g_hash_table_remove(c->activities, GINT_TO_POINTER(activity));
    return result;
}
int anco_linux_catalog_bind_surface_for_activity(AncoLinuxCatalog *c,
    gint32 activity, const void *client, const void *surface,
    pid_t pid, uid_t uid, gid_t gid, AncoLinuxSurfaceOwner *out)
{
    if (!c || !out) return -EINVAL;
    AncoLinuxLaunch live;
    int result = anco_linux_catalog_activity(c, activity, &live);
    if (result) return result;
    result = anco_linux_surface_bind(c->ownership, activity, client, surface, pid, uid, gid);
    return result ? result : anco_linux_surface_owner(c->ownership, client, surface, out);
}
int anco_linux_catalog_bind_surface_for_process(AncoLinuxCatalog *c,
    gint32 activity,const void *client,const void *surface,
    const AncoLinuxProcess *process,AncoLinuxSurfaceOwner *out)
{
    if(!c || !client || !surface || !process || !out)return -EINVAL;
    AncoLinuxLaunch live;
    int result=0;
    if(activity>0)result=anco_linux_catalog_activity(c,activity,&live);
    else {
        Instance *selected=NULL;
        for(guint i=0;i<c->instances->len;++i){
            Instance *candidate=g_ptr_array_index(c->instances,i);
            if(anco_linux_process_is_descendant(&candidate->launch.process,
                    process))continue;
            if(selected)return -ENOTUNIQ;
            selected=candidate;
        }
        if(!selected)return -ESRCH;
        activity=selected->launch.activity_id;
        /* A process-only launcher has no Activity until its first real
         * toplevel creates one.  Report the absence explicitly so the
         * compositor can create that Task exactly once; do not pass the
         * sentinel through the ordinary activity lookup. */
        if(activity<=0)return -ENOENT;
        result=anco_linux_catalog_activity(c,activity,&live);
    }
    if(result)return result;
    result=anco_linux_surface_bind_descendant(c->ownership,activity,client,
        surface,process);
    return result ? result : anco_linux_surface_owner(c->ownership,client,
        surface,out);
}
int anco_linux_catalog_remove_surface(AncoLinuxCatalog *c, const void *client,
                                      const void *surface)
{
    return c ? anco_linux_surface_remove(c->ownership, client, surface) : -EINVAL;
}
void anco_linux_catalog_disconnect(AncoLinuxCatalog *c, const void *client)
{
    if (c) anco_linux_client_disconnected(c->ownership, client);
}
int anco_linux_catalog_windows_configure(AncoLinuxCatalog *c, gint32 value)
{
    return c ? anco_linux_windows_configure(c->ownership, value) : -EINVAL;
}
int anco_linux_catalog_window_reserve_transient(AncoLinuxCatalog *c,
                                                gint32 activity, gint32 *out)
{
    return c ? anco_linux_window_reserve_transient(c->ownership, activity, out) :
        -EINVAL;
}
int anco_linux_catalog_window_add(AncoLinuxCatalog *c, const void *client,
                                  const void *window, const void *surface,
                                  AncoLinuxWindowOwner *out)
{
    return c ? anco_linux_window_add(c->ownership, client, window, surface, out) : -EINVAL;
}
int anco_linux_catalog_window_owner(AncoLinuxCatalog *c, const void *client,
                                    const void *window, AncoLinuxWindowOwner *out)
{
    return c ? anco_linux_window_owner(c->ownership, client, window, out) : -EINVAL;
}
int anco_linux_catalog_window_remove(AncoLinuxCatalog *c, const void *client,
                                     const void *window)
{
    return c ? anco_linux_window_remove(c->ownership, client, window) : -EINVAL;
}
int anco_linux_catalog_signal(AncoLinuxCatalog *c, gint32 task, int signo)
{
    if (!c || (signo != SIGTERM && signo != SIGKILL)) return -EINVAL;
    AncoLinuxLaunch live;
    const int result = anco_linux_catalog_task(c, task, &live);
    if (result) return result;
    return kill(live.process.pid, signo) ? -errno : 0;
}
guint anco_linux_catalog_children(AncoLinuxCatalog *c)
{
    return c ? c->instances->len : 0;
}
gint32 anco_linux_catalog_user(AncoLinuxCatalog *c)
{
    return c ? c->user : -1;
}
gboolean anco_linux_catalog_privacy(AncoLinuxCatalog *c)
{
    return c ? c->privacy : FALSE;
}
