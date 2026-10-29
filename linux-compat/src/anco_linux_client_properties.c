#include "anco_linux_client_properties.h"
#include <json-glib/json-glib.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
typedef struct {
    AncoClientProperties properties;
    gchar *text;
} CachedProperties;
struct AncoClientPropertiesObserver {
    gint32 pid;
    gchar *directory;
    GFileMonitor *monitor;
    guint scan_source;
    gboolean scan_started;
    GHashTable *files;
    AncoClientPropertiesUpdate update;
    AncoClientPropertiesRemove remove;
    gpointer data;
};
static void cached_properties_free(gpointer data)
{
    CachedProperties *saved=data;
    if(!saved)return;
    g_free(saved->text);g_free(saved);
}
static gboolean integer(JsonObject *o,const char *name,gint32 *result)
{
    JsonNode *n=o?json_object_get_member(o,name):NULL;
    if(!n || !JSON_NODE_HOLDS_VALUE(n) || json_node_get_value_type(n)!=G_TYPE_INT64)return FALSE;
    const gint64 v=json_node_get_int(n);
    if(v<G_MININT32 || v>G_MAXINT32)return FALSE;
    *result=(gint32)v;return TRUE;
}
static gboolean unsigned_integer(JsonObject *o,const char *name,guint64 *result)
{
    JsonNode *n=o?json_object_get_member(o,name):NULL;
    if(!n || !JSON_NODE_HOLDS_VALUE(n) ||
       json_node_get_value_type(n)!=G_TYPE_INT64)return FALSE;
    const gint64 v=json_node_get_int(n);
    if(v<0)return FALSE;
    *result=(guint64)v;return TRUE;
}
static JsonObject *object(JsonObject *o,const char *name)
{
    JsonNode *n=o?json_object_get_member(o,name):NULL;
    return n&&JSON_NODE_HOLDS_OBJECT(n)?json_node_get_object(n):NULL;
}
static gboolean rect(JsonObject *o,AncoWmsRect *r)
{
    gint32 x,y,w,h;
    if(!integer(o,"left",&x)||!integer(o,"top",&y)||!integer(o,"width",&w)||
       !integer(o,"height",&h)||w<=0||h<=0)return FALSE;
    const gint64 right=(gint64)x+w,bottom=(gint64)y+h;
    if(right<G_MININT32||right>G_MAXINT32||bottom<G_MININT32||bottom>G_MAXINT32)return FALSE;
    *r=(AncoWmsRect){x,y,(gint32)right,(gint32)bottom};return TRUE;
}
static void read_properties(AncoClientPropertiesObserver *self,const char *path)
{
    if(!g_str_has_suffix(path,".json"))return;
    const char *name=strrchr(path,'/');name=name?name+1:path;
    gint32 filename_pid=0,filename_native=0;char trailing=0;
    if(sscanf(name,"window-%d-%d.json%c",&filename_pid,&filename_native,&trailing)!=2 ||
       filename_pid<=0 || filename_native<=0)return;
    gchar *text=NULL;gsize bytes=0;
    if(!g_file_get_contents(path,&text,&bytes,NULL)||bytes>4096){g_free(text);return;}
    CachedProperties *previous=g_hash_table_lookup(self->files,path);
    if(previous && previous->text && strlen(previous->text)==bytes &&
       !memcmp(previous->text,text,bytes)){g_free(text);return;}
    JsonParser *parser=json_parser_new();
    const gboolean parsed=json_parser_load_from_data(parser,text,bytes,NULL);
    JsonNode *root=parsed?json_parser_get_root(parser):NULL;
    JsonObject *o=root&&JSON_NODE_HOLDS_OBJECT(root)?json_node_get_object(root):NULL;
    AncoClientProperties p={0};
    JsonObject *props=object(o,"windowProperties");
    (void)integer(o,"guestWindowId",&p.guest_window);
    gboolean ok=integer(o,"frontendPid",&p.frontend_pid)&&p.frontend_pid>0&&
        (!self->pid||p.frontend_pid==self->pid)&&p.frontend_pid==filename_pid&&
        unsigned_integer(o,"frontendStartTicks",&p.frontend_start_ticks)&&
        p.frontend_start_ticks>0&&
        unsigned_integer(o,"closeRequestSerial",&p.close_request_serial)&&
        integer(o,"taskId",&p.task)&&integer(props,"id",&p.native_id)&&
        p.native_id>0&&p.native_id==filename_native;
    JsonNode *bundle=o?json_object_get_member(o,"bundleName"):NULL;
    if(bundle&&JSON_NODE_HOLDS_VALUE(bundle)&&json_node_get_value_type(bundle)==G_TYPE_STRING)
        p.bundle=json_node_get_string(bundle);
    JsonNode *vpr=o?json_object_get_member(o,"vpr"):NULL;
    if(vpr&&JSON_NODE_HOLDS_VALUE(vpr)&&
       (json_node_get_value_type(vpr)==G_TYPE_DOUBLE||json_node_get_value_type(vpr)==G_TYPE_INT64))
        p.vpr=json_node_get_double(vpr);
    const gboolean system_window=p.task==-1&&p.guest_window>0;
    if(system_window)ok=ok&&!p.bundle;
    else ok=ok&&p.task>=0&&p.guest_window==0&&
        integer(o,"windowMode",&p.mode)&&(p.mode==1||p.mode==102)&&
        integer(o,"windowStatus",&p.status)&&p.status>=1&&p.status<=5&&
        rect(object(props,"windowRect"),&p.outer)&&
        rect(object(props,"drawableRect"),&p.drawable)&&p.bundle&&*p.bundle&&
        isfinite(p.vpr)&&p.vpr>0&&p.drawable.left>=0&&p.drawable.top>=0&&
        (gint64)p.drawable.right<=((gint64)p.outer.right-p.outer.left)&&
        (gint64)p.drawable.bottom<=((gint64)p.outer.bottom-p.outer.top);
    if(ok) {
        // Cache only scalar ownership for an actual removal event. Bundle
        // storage belongs to the parser and must not escape the update call.
        CachedProperties *saved=g_new0(CachedProperties,1);
        saved->properties.frontend_pid=p.frontend_pid;
        saved->properties.frontend_start_ticks=p.frontend_start_ticks;
        saved->properties.task=p.task;
        saved->properties.guest_window=p.guest_window;
        saved->properties.native_id=p.native_id;
        saved->text=g_strndup(text,bytes);
        g_hash_table_replace(self->files,g_strdup(path),saved);
        printf("ANCO_CLIENT_PROPERTIES_OBSERVED task=%d native_id=%d close_serial=%llu source=original-window-properties\n",
            p.task,p.native_id,(unsigned long long)p.close_request_serial);
        self->update(&p,self->data);
    }
    g_free(text);
    g_object_unref(parser);
}
static void changed(GFileMonitor *monitor,GFile *file,GFile *other,
    GFileMonitorEvent event,gpointer data)
{
    (void)monitor;
    AncoClientPropertiesObserver *self=data;
    gchar *path=g_file_get_path(file);
    if(path && event==G_FILE_MONITOR_EVENT_DELETED) {
        const CachedProperties *saved=g_hash_table_lookup(self->files,path);
        const AncoClientProperties *p=saved?&saved->properties:NULL;
        if(p && self->remove)self->remove(p->frontend_pid,p->frontend_start_ticks,
            p->task,p->guest_window,p->native_id,self->data);
        g_hash_table_remove(self->files,path);
    } else if(path)read_properties(self,path);
    g_free(path);
    // Atomic .tmp -> .json produces RENAMED with the target in other.
    if(other){path=g_file_get_path(other);if(path)read_properties(self,path);g_free(path);}
}
static gboolean scan_properties(gpointer data)
{
    AncoClientPropertiesObserver *self=data;
    if(!self->scan_started){
        self->scan_started=TRUE;
        printf("ANCO_CLIENT_PROPERTIES_SCAN_READY directory=%s interval_ms=100\n",
            self->directory);
    }
    GDir *dir=g_dir_open(self->directory,0,NULL);
    if(dir){
        const gchar *name;
        while((name=g_dir_read_name(dir)))if(g_str_has_suffix(name,".json")){
            gchar *path=g_build_filename(self->directory,name,NULL);
            read_properties(self,path);g_free(path);
        }
        g_dir_close(dir);
    }
    /* GFileMonitor can lose its kernel watch if the producer replaces its
     * output directory.  The original property records remain authoritative;
     * this scan only keeps that same channel live and also observes removals. */
    GHashTableIter it;gpointer key,value;
    g_hash_table_iter_init(&it,self->files);
    while(g_hash_table_iter_next(&it,&key,&value))if(!g_file_test(key,G_FILE_TEST_EXISTS)){
        const AncoClientProperties *p=&((CachedProperties *)value)->properties;
        if(self->remove)self->remove(p->frontend_pid,p->frontend_start_ticks,
            p->task,p->guest_window,p->native_id,self->data);
        g_hash_table_iter_remove(&it);
    }
    return G_SOURCE_CONTINUE;
}
AncoClientPropertiesObserver *anco_client_properties_observe(const char *directory,
    gint32 pid,AncoClientPropertiesUpdate update,AncoClientPropertiesRemove remove,
    gpointer data,GError **error)
{
    if(!directory||directory[0]!='/'||pid<0||!update)return NULL;
    AncoClientPropertiesObserver *self=g_new0(AncoClientPropertiesObserver,1);
    GFile *folder=g_file_new_for_path(directory);
    self->monitor=g_file_monitor_directory(folder,G_FILE_MONITOR_WATCH_MOVES,NULL,error);
    g_object_unref(folder);
    if(!self->monitor){g_free(self);return NULL;}
    self->pid=pid;self->directory=g_strdup(directory);
    self->update=update;self->remove=remove;self->data=data;
    self->files=g_hash_table_new_full(g_str_hash,g_str_equal,g_free,cached_properties_free);
    g_file_monitor_set_rate_limit(self->monitor,0);
    g_signal_connect(self->monitor,"changed",G_CALLBACK(changed),self);
    scan_properties(self);
    self->scan_source=g_timeout_add(100,scan_properties,self);
    return self;
}
void anco_client_properties_observer_free(AncoClientPropertiesObserver *self)
{
    if(!self)return;
    if(self->scan_source)g_source_remove(self->scan_source);
    g_file_monitor_cancel(self->monitor);
    g_signal_handlers_disconnect_by_data(self->monitor,self);
    g_object_unref(self->monitor);g_hash_table_unref(self->files);
    g_free(self->directory);g_free(self);
}
