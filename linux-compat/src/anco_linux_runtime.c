/* Common Linux session entry. Run inside enter --anco-native.
 * Original host ABI only. Window/producer composition is still unfinished;
 * ordinary launch is not enabled until its real launcher configuration exists.
 * No embedded timeout, Android bootstrap or synthetic input. */
#define _GNU_SOURCE
#define _DEFAULT_SOURCE
#define WLR_USE_UNSTABLE
#include "anco_ams_agent.h"
#include "anco_resource_agent.h"
#include "anco_host_dependency.h"
#include "anco_wayland_context.h"
#include "anco_wms_catalog.h"
#include "anco_wms_callback_receiver.h"
#include "anco_delegator_registry.h"
#include "anco_surface_identity.h"
#include "anco_shm_queue.h"
#include "wayland_egl_backend.h"
#include "anco_wlroots_admission.h"
#include "anco_graphic_display.h"
#include "anco_wayland_display.h"
#include "anco_wayland_geometry.h"
#include "anco_graphic_layers.h"
#include "anco_text_receiver.h"
#include "anco_linux_imf_runtime.h"
#include "anco_ibus_bridge.h"
#include <wlr/util/box.h>
#include "anco_native_input_relay.h"
#include "anco_linux_keymap.h"
#include "anco_linux_client_properties.h"
#include "anco_linux_session_bus.h"
#include "anco_linux_pasteboard.h"
#include "anco_linux_host_windows.h"
#include "gbinder_ipc.h"
#include "gbinder_local_request_p.h"
#include <json-glib/json-glib.h>
#include <glib-unix.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_subcompositor.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/types/wlr_xdg_decoration_v1.h>
#include <wlr/types/wlr_server_decoration.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/types/wlr_shm.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_xdg_output_v1.h>
#include <wlr/types/wlr_fractional_scale_v1.h>
#include <wlr/types/wlr_viewporter.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/config.h>
#if WLR_HAS_XWAYLAND
#include <wlr/xwayland.h>
#include <xcb/xcb.h>
#endif
#include <wlr/interfaces/wlr_keyboard.h>
#include <wlr/interfaces/wlr_buffer.h>
#include <wlr/util/edges.h>
#include <math.h>
#include <drm_fourcc.h>
#include <linux/input-event-codes.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <fcntl.h>
#include <unistd.h>
#include <grp.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <time.h>
#include <limits.h>
#include <stddef.h>

typedef struct RuntimeWindow RuntimeWindow;
typedef struct RuntimeStartingWindow RuntimeStartingWindow;
typedef struct RuntimePendingToplevel RuntimePendingToplevel;
#if WLR_HAS_XWAYLAND
typedef struct RuntimePendingXwayland RuntimePendingXwayland;
#endif
typedef struct {
    gint32 id, mode;
    gboolean semantic_task;
    gboolean mission_associated;
    gint32 mission_id;
    gint32 frontend_pid;
    guint64 frontend_start_ticks,close_request_serial;
    gboolean close_request_serial_known;
    gboolean retire_requested,process_exited;
    gboolean host_mode_known, host_bounds_valid;
    gboolean frontend_window_registered;
    gboolean graphic_frontend_ready;
    gboolean launch_mode_present;
    gint32 launch_mode;
    gboolean launch_mode_observed;
    gboolean launch_construction_mode_observed;
    gboolean launch_mode_resolved;
    AncoWmsRect host_bounds;
    gfloat host_scale;
    gboolean client_area_valid, client_area_deferred;
    AncoWmsRect client_outer, client_area;
    gint32 client_native_id, client_mode, client_status;
    double client_vpr;
    GArray *activities;
    RuntimeStartingWindow *starting_window;
} RuntimeTask;

typedef struct {
    GMainLoop *loop;
    GPtrArray *host_windows;
    struct wl_display *display;
    AncoWaylandContext *wayland;
    AncoWaylandOwnership *ownership;
    AncoLinuxCatalog *catalog;
    const char *catalog_directory, *catalog_decoder;
    const char *icon_directory;
    AncoAmsAgent *agent;
    AncoWmsBroker *wms;
    AncoWmsCallbackReceiver *wms_callback;
    GBinderIpc *ipc;
    AncoDelegatorRegistry *delegators;
    char *buffershared;
    gint32 shared_flag;
    struct wlr_xdg_shell *shell;
    struct wlr_compositor *compositor;
#if WLR_HAS_XWAYLAND
    struct wlr_xwayland *xwayland;
    struct wl_listener xwayland_ready, xwayland_new_surface;
    xcb_atom_t xwayland_kde_override_atom;
#endif
    struct wlr_xdg_decoration_manager_v1 *decoration_manager;
    struct wlr_server_decoration_manager *legacy_decoration_manager;
    AncoWaylandDisplayInfo display_info;
    struct wlr_output *output;
    struct wlr_output_layout *output_layout;
    struct wlr_seat *seat;
    AncoTextReceiver *text_receiver;
    AncoTextSeat *text_seat;
    AncoLinuxImfRuntime *imf;
    AncoIbusBridge *ibus;
    AncoLinuxPasteboard *pasteboard;
    GHashTable *tasks;
    GList *task_stack, *windows, *pending_toplevels;
#if WLR_HAS_XWAYLAND
    GList *pending_xwayland;
#endif
    AncoGraphicLayerStream *layer_stream;
    guint layer_idle;
    guint initial_commit_resume_idle,producer_resume_idle;
    guint task_retire_idle;
    RuntimeWindow *focused_window;
    gint32 focused_activity;
    gboolean container_has_focus, container_focused;
    int input_fd;
    guint input_source;
    gboolean input_session_open;
    AncoClientPropertiesObserver *client_properties_observer;
    GHashTable *native_system_ids;
    AncoNativeInputRelay input_state;
    struct wlr_keyboard input_keyboard;
    struct wl_listener input_key, input_modifiers;
    gboolean input_keyboard_ready, input_key_dispatch;
    GQueue input_pending_keys;
    guint64 input_key_received, input_key_sent, input_key_unmapped, input_key_no_focus;
    guint64 input_key_imf_queued, input_key_imf_consumed, input_key_imf_stale;
    gboolean input_pointer_ready;
    RuntimeWindow *input_pointer_window;
    double input_pointer_surface_origin_x, input_pointer_surface_origin_y;
    gboolean input_pointer_position_valid;
    double input_pointer_display_x, input_pointer_display_y;
    RuntimeWindow *interactive_move_window;
    gboolean interactive_move_server_owned;
    double interactive_move_start_x, interactive_move_start_y;
    gint32 interactive_move_start_task_x, interactive_move_start_task_y;
    RuntimeWindow *interactive_resize_window;
    gboolean interactive_resize_server_owned;
    gboolean interactive_resize_native_root;
    double interactive_resize_start_x, interactive_resize_start_y;
    gint32 interactive_resize_start_width, interactive_resize_start_height;
    gint32 interactive_resize_width, interactive_resize_height;
    guint32 interactive_resize_edges;
    AncoWmsRect interactive_resize_start_outer;
    gint32 interactive_resize_frame_width,interactive_resize_frame_height;
    RuntimeWindow *server_close_window;
    guint64 input_pointer_received, input_pointer_sent, input_pointer_unsupported,
        input_pointer_no_target;
    struct wl_listener new_toplevel, new_popup, new_decoration;
    struct wl_listener new_client;
    const char *startup_bundle, *startup_ability;
    const char *startup_broker_version;
    gboolean startup_multiwindow;
    gint32 startup_task;
    guint startup_idle;
    int launch_listener;
    guint launch_source;
    gboolean stopping;
    gboolean failed;
    gboolean creating_window_carrier;
} Runtime;

static gboolean stop_runtime(void *data);
static gboolean start_new_task(void *data);
static GBinderRemoteObject *find_bms(GBinderIpc *ipc);
static gboolean load_catalog_bundle(AncoLinuxCatalog *catalog,
    GBinderRemoteObject *bms,const char *directory,const char *decoder,
    const char *requested_bundle);
static int launch_control_start(Runtime *runtime);
static void launch_control_finish(Runtime *runtime);
static int input_keyboard_start(Runtime *runtime);
static void input_keyboard_finish(Runtime *runtime);
static void input_key_drain(Runtime *runtime);
static int input_pointer_start(Runtime *runtime);
static void input_pointer_finish(Runtime *runtime);
static void input_pointer_received(Runtime *runtime,const AncoNativeInputPointer *pointer);
static void input_pointer_forget_window(Runtime *runtime,RuntimeWindow *window);
static gboolean window_receives_keys(RuntimeWindow *window);
static void apply_window_focus(Runtime *runtime,RuntimeWindow *selected,
    const char *source);
static void recompute_window_focus(Runtime *runtime);
static void schedule_semantic_task_retire(Runtime *runtime);
static void schedule_initial_commits_for_task(Runtime *runtime,RuntimeTask *task);
static void schedule_graphic_producers_for_task(Runtime *runtime,RuntimeTask *task);
static int ensure_starting_window(Runtime *runtime,RuntimeTask *task);
static void retire_starting_window(RuntimeTask *task,const char *reason);
static void retire_matching_starting_windows(RuntimeWindow *window);
static void handle_launch_switch_ready(Runtime *runtime,RuntimeTask *task,
    const char *source);
static RuntimeWindow *runtime_parent_for_xdg(Runtime *runtime,
    struct wlr_xdg_surface *xdg);
static RuntimeWindow *direct_parent_window(RuntimeWindow *window);
static RuntimeWindow *original_attached_parent(RuntimeWindow *window);
static int committed_original_layer(RuntimeWindow *window,
    const AncoWaylandGeometry *geometry,struct wlr_buffer *buffer,
    const AncoWmsRect *decoration,AncoGraphicLayer *out);
#if WLR_HAS_XWAYLAND
static void track_xwayland_surface(Runtime *runtime,
    struct wlr_xwayland_surface *surface,gint32 retire_task,
    gboolean admit_if_associated);
#endif

static void refresh_frontend_window_registration(Runtime *runtime)
{
    if(!runtime->tasks)return;
    GHashTableIter iter;gpointer value;
    g_hash_table_iter_init(&iter,runtime->tasks);
    while(g_hash_table_iter_next(&iter,NULL,&value)){
        RuntimeTask *task=value;
        gboolean registered=FALSE;
        if(task->mission_associated && task->mission_id>0 && runtime->host_windows){
            for(guint i=0;i<runtime->host_windows->len;++i){
                const AncoLinuxHostWindow *host=
                    g_ptr_array_index(runtime->host_windows,i);
                if(host->fields.window_id==task->mission_id){
                    registered=TRUE;break;
                }
            }
        }
        if(task->frontend_window_registered!=registered){
            task->frontend_window_registered=registered;
            printf("RUNTIME_ORIGINAL_FRONTEND_WINDOW task=%d mission=%d registered=%d source=original-input-window-table\n",
                task->id,task->mission_id,registered);
            if(registered)schedule_initial_commits_for_task(runtime,task);
        }
        /* The stock input callback reports container=1 only after the AnCo
         * Task page has entered its real foreground container and installed
         * the SurfaceDelegator client.  Latch that original transition once;
         * later backgrounding must not tear down the producer. */
        if(registered && runtime->input_state.container &&
           !task->graphic_frontend_ready){
            task->graphic_frontend_ready=TRUE;
            printf("RUNTIME_ORIGINAL_GRAPHIC_FRONTEND task=%d mission=%d ready=1 source=original-input-container\n",
                task->id,task->mission_id);
            schedule_initial_commits_for_task(runtime,task);
            schedule_graphic_producers_for_task(runtime,task);
            if(!task->semantic_task&&task->retire_requested&&
               task->mission_associated&&task->client_area_valid)
                handle_launch_switch_ready(runtime,task,"graphic-frontend");
        }
    }
}

static void input_relay_disconnect(Runtime *runtime)
{
    input_pointer_finish(runtime);
    input_keyboard_finish(runtime);
    runtime->input_session_open=FALSE;
    runtime->input_state=(AncoNativeInputRelay){0};
    g_clear_pointer(&runtime->host_windows,g_ptr_array_unref);
    refresh_frontend_window_registration(runtime);
    if(runtime->input_fd>=0){
        shutdown(runtime->input_fd,SHUT_RDWR);
        close(runtime->input_fd);runtime->input_fd=-1;
    }
    puts("RUNTIME_NATIVE_INPUT_DISCONNECTED dispatch_connected=0");
}

static gboolean input_relay_received(gint fd,GIOCondition condition,gpointer data)
{
    Runtime *runtime=data;
    for(;;){
        ssize_t queued=recv(fd,NULL,0,MSG_DONTWAIT|MSG_PEEK|MSG_TRUNC);
        if(queued<0&&errno==EINTR)continue;
        if(queued<0&&(errno==EAGAIN||errno==EWOULDBLOCK)){
            if(!(condition&(G_IO_HUP|G_IO_ERR|G_IO_NVAL)))return G_SOURCE_CONTINUE;
            break;
        }
        if(queued<(ssize_t)sizeof(AncoNativeInputRelay)||queued>G_MAXINT){
            puts("RUNTIME_NATIVE_INPUT_INVALID_SIZE");break;
        }
        g_autofree unsigned char *bytes_buffer=g_try_malloc((gsize)queued);
        if(!bytes_buffer){puts("RUNTIME_NATIVE_INPUT_ALLOCATION_FAILED");break;}
        struct iovec vector={bytes_buffer,(size_t)queued};
        struct msghdr message={.msg_iov=&vector,.msg_iovlen=1};
        ssize_t bytes=recvmsg(fd,&message,MSG_DONTWAIT);
        if(bytes<0&&errno==EINTR)continue;
        if(bytes<0&&(errno==EAGAIN||errno==EWOULDBLOCK)){
            if(!(condition&(G_IO_HUP|G_IO_ERR|G_IO_NVAL)))return G_SOURCE_CONTINUE;
            break;
        }
        if(bytes<=0)break;
        AncoNativeInputRelay packet;
        if(bytes<(ssize_t)sizeof(packet)||(message.msg_flags&MSG_TRUNC)){
            puts("RUNTIME_NATIVE_INPUT_INVALID_PACKET");break;
        }
        memcpy(&packet,bytes_buffer,sizeof(packet));
        const size_t expected=packet.code==0?sizeof(AncoNativeInputPointer):
            packet.code==1?sizeof(AncoNativeInputKey):
            packet.code==2?(size_t)bytes:sizeof(packet);
        if(bytes!=(ssize_t)expected||packet.bytes!=expected||
           (packet.code!=ANCO_INPUT_RELAY_SESSION&&packet.code!=0&&packet.code!=1&&packet.code!=2&&packet.code!=3&&packet.code!=5)){
            puts("RUNTIME_NATIVE_INPUT_INVALID_PACKET");break;
        }
        runtime->input_state=packet;
        if(packet.code==2){
            GPtrArray *records=anco_linux_host_windows_decode(bytes_buffer,(gsize)bytes);
            if(!records){puts("RUNTIME_NATIVE_WINDOWS_INVALID_SNAPSHOT");break;}
            g_clear_pointer(&runtime->host_windows,g_ptr_array_unref);
            runtime->host_windows=records;
            refresh_frontend_window_registration(runtime);
            printf("RUNTIME_NATIVE_WINDOWS_SNAPSHOT records=%u bytes=%zd original_order=1 pointer_consumer=0\n",
                records->len,bytes);
        }
        if(packet.code==ANCO_INPUT_RELAY_SESSION){
            runtime->input_session_open=packet.status==0;
            printf("RUNTIME_NATIVE_INPUT_SESSION original_result=%d open=%d\n",
                packet.status,runtime->input_session_open);
            if(packet.status){runtime->failed=TRUE;g_main_loop_quit(runtime->loop);break;}
            if(input_keyboard_start(runtime)){runtime->failed=TRUE;break;}
            if(input_pointer_start(runtime)){runtime->failed=TRUE;break;}
        }
        if(packet.code==0){
            AncoNativeInputPointer pointer;
            memcpy(&pointer,bytes_buffer,sizeof(pointer));
            if(pointer.item_count>32){puts("RUNTIME_NATIVE_INPUT_INVALID_POINTER");break;}
            printf("RUNTIME_NATIVE_POINTER_RAW event=%d raw_action=%d source=%d action=%d button=%d display=%d host_window=%d items=%u scroll=%.9g,%.9g dispatch_connected=%d\n",
                pointer.fields_10_to_28[1],pointer.fields_10_to_28[2],pointer.fields_38_to_48[1],
                pointer.fields_38_to_48[2],pointer.fields_38_to_48[3],pointer.fields_10_to_28[4],
                pointer.fields_10_to_28[5],pointer.item_count,pointer.field_30,pointer.field_34,
                runtime->input_pointer_ready);
            for(guint i=0;i<pointer.item_count;++i)
                printf("RUNTIME_NATIVE_POINTER_ITEM event=%d id=%d display_xy=%d,%d tool=%d device=%d\n",
                    pointer.fields_10_to_28[1],pointer.items[i].field_08,
                    pointer.items[i].field_10,pointer.items[i].field_14,
                    pointer.items[i].field_18,pointer.items[i].field_1c);
            input_pointer_received(runtime,&pointer);
        }
        if(packet.code==1){
            AncoNativeInputKey *key=g_new(AncoNativeInputKey,1);
            memcpy(key,bytes_buffer,sizeof(*key));
            if(key->reserved||(key->android_action!=0&&key->android_action!=1)){
                g_free(key);puts("RUNTIME_NATIVE_INPUT_INVALID_KEY");break;
            }
            ++runtime->input_key_received;
            g_queue_push_tail(&runtime->input_pending_keys,key);
        }
        printf("RUNTIME_NATIVE_INPUT_STATE code=%u open=%d focus=%d container=%u excluded=%u windows=%u knuckle=%u keyboard_connected=%d pointer_connected=%d\n",
            packet.code,runtime->input_session_open,packet.host_focus,packet.container,
            packet.key_excluded,packet.retained_windows,packet.knuckle,
            runtime->input_keyboard_ready,runtime->input_pointer_ready);
        input_key_drain(runtime);
        /* These are real HOST container/filter states, not guest logical IDs.
         * Never replace WMS focus, activate a surface or advertise keyboard
         * capability merely because the host state/registration arrived. */
    }
    runtime->input_source=0;
    input_relay_disconnect(runtime);
    if(!runtime->stopping){
        /* Actual peer failure is an application-session failure, not a
         * diagnostic success while the GUI silently loses all input state.
         * Use the existing child lifecycle, with no timeout/watchdog. */
        runtime->failed=TRUE;
        stop_runtime(runtime);
    }
    return G_SOURCE_REMOVE;
}

static int input_relay_attach(Runtime *runtime)
{
    const char *text=g_getenv("ANCO_NATIVE_INPUT_FD");
    if(!text)return 0;
    char *end=NULL;errno=0;long value=strtol(text,&end,10);
    if(errno||!*text||*end||value<3||value>G_MAXINT)return -EINVAL;
    const int fd=(int)value;
    int type=0;socklen_t size=sizeof(type);
    const int flags=fcntl(fd,F_GETFD);
    if(flags<0||getsockopt(fd,SOL_SOCKET,SO_TYPE,&type,&size)||type!=SOCK_SEQPACKET)return -EINVAL;
    if(fcntl(fd,F_SETFD,flags|FD_CLOEXEC))return -errno;
    g_unsetenv("ANCO_NATIVE_INPUT_FD"); // Never inherited by Linux applications.
    runtime->input_fd=fd;
    runtime->input_source=g_unix_fd_add(fd,G_IO_IN|G_IO_HUP|G_IO_ERR|G_IO_NVAL,
        input_relay_received,runtime);
    if(!runtime->input_source){input_relay_disconnect(runtime);return -EIO;}
    puts("RUNTIME_NATIVE_INPUT_ATTACHED common_loop=1 keyboard_consumer=1 pointer_consumer=1");
    return 0;
}

static void configuration_received(gint32 display, gint32 user,
    GHashTable *configuration, void *data)
{
    (void)data;
    GHashTableIter iter;
    gpointer key, value;
    g_hash_table_iter_init(&iter, configuration);
    while (g_hash_table_iter_next(&iter, &key, &value))
        printf("RUNTIME_CONFIGURATION display=%d user=%d key=%s value=%s\n",
            display, user, (char *)key, (char *)value);
}

#if WLR_HAS_XWAYLAND
static void xwayland_ready(struct wl_listener *listener,void *data)
{
    (void)data;
    Runtime *runtime=wl_container_of(listener,runtime,xwayland_ready);
    if(!runtime->xwayland || !runtime->xwayland->display_name ||
       !*runtime->xwayland->display_name){
        puts("RUNTIME_XWAYLAND_READY_INVALID display_missing=1");
        runtime->failed=TRUE;
        stop_runtime(runtime);
        return;
    }
    g_setenv("DISPLAY",runtime->xwayland->display_name,TRUE);
    int screen=0;
    xcb_connection_t *connection=xcb_connect(runtime->xwayland->display_name,&screen);
    if(connection && !xcb_connection_has_error(connection)){
        static const char name[]="_KDE_NET_WM_WINDOW_TYPE_OVERRIDE";
        const xcb_intern_atom_cookie_t cookie=xcb_intern_atom(connection,0,
            sizeof(name)-1,name);
        xcb_intern_atom_reply_t *reply=xcb_intern_atom_reply(connection,cookie,NULL);
        if(reply){runtime->xwayland_kde_override_atom=reply->atom;free(reply);}
        xcb_screen_iterator_t screens=xcb_setup_roots_iterator(
            xcb_get_setup(connection));
        for(int index=0;index<screen&&screens.rem;++index)
            xcb_screen_next(&screens);
        int dpi_status=-ENOENT;
        const int xft_dpi=isfinite(runtime->display_info.scale) &&
            runtime->display_info.scale>0?
            (int)lround(96.0*runtime->display_info.scale):0;
        if(screens.rem && xft_dpi>0){
            char resource[256];
            const int length=snprintf(resource,sizeof(resource),
                "Xft.dpi:\t%d\n"
                "Xft.antialias:\t1\n"
                "Xft.hinting:\t1\n"
                "Xft.hintstyle:\thintslight\n"
                "Xft.rgba:\trgb\n"
                "Xft.lcdfilter:\tlcddefault\n",xft_dpi);
            if(length>0 && (size_t)length<sizeof(resource)){
                const xcb_void_cookie_t changed=xcb_change_property_checked(
                    connection,XCB_PROP_MODE_REPLACE,screens.data->root,
                    XCB_ATOM_RESOURCE_MANAGER,XCB_ATOM_STRING,8,
                    (uint32_t)length,resource);
                xcb_generic_error_t *error=xcb_request_check(connection,changed);
                dpi_status=error?-(int)error->error_code:0;
                free(error);xcb_flush(connection);
            }
        }
        printf("RUNTIME_XWAYLAND_DPI host_vpr=%.9g density_dpi=%d xft_dpi=%d resource_manager_status=%d native_root_pixels=%dx%d antialias=1 hintstyle=hintslight rgba=rgb lcdfilter=lcddefault\n",
            runtime->display_info.scale,runtime->display_info.density_dpi,
            xft_dpi,dpi_status,
            runtime->display_info.display.width,runtime->display_info.display.height);
    }
    if(connection)xcb_disconnect(connection);
    printf("RUNTIME_XWAYLAND_READY display=%s standard_xwm=1 pixel_capture=0\n",
        runtime->xwayland->display_name);
    printf("RUNTIME_XWAYLAND_ATOMS kde_override=%u screen=%d\n",
        runtime->xwayland_kde_override_atom,screen);
    if(runtime->startup_bundle && !runtime->startup_idle && !runtime->startup_task){
        runtime->startup_idle=g_idle_add(start_new_task,runtime);
        puts("RUNTIME_XWAYLAND_STARTUP_RELEASE display_ready=1 explicit_task_pending=1");
    }
}

static void xwayland_new_surface(struct wl_listener *listener,void *data)
{
    Runtime *runtime=wl_container_of(listener,runtime,xwayland_new_surface);
    struct wlr_xwayland_surface *surface=data;
    if(!surface)return;
    printf("RUNTIME_XWAYLAND_NEW_SURFACE xid=%u pid=%ld override_redirect=%d associated=%d title=%s class=%s\n",
        surface->window_id,(long)surface->pid,surface->override_redirect,
        surface->surface!=NULL,surface->title?surface->title:"",
        surface->class?surface->class:"");
    track_xwayland_surface(runtime,surface,0,TRUE);
}
#endif

typedef struct {
    gboolean valid;
    guint32 serial;
    union {
        struct wlr_xdg_toplevel_configure toplevel;
        struct wlr_xdg_popup_configure popup;
    } role;
} ClientConfiguration;

struct RuntimeWindow {
    Runtime *runtime;
    struct wlr_xdg_surface *xdg;
#if WLR_HAS_XWAYLAND
    struct wlr_xwayland_surface *xwayland_surface;
#endif
    AncoLinuxWindowOwner identity;
    struct wl_listener commit, destroy, configure, ack_configure, map, unmap,
        request_move, request_resize, request_maximize, request_minimize,
        request_fullscreen, request_activate, request_configure, set_title;
#if WLR_HAS_XWAYLAND
    struct wl_listener set_window_type, set_hints, set_decorations;
#endif
    guint64 mapping_epoch;
    ClientConfiguration sent_configuration, accepted_configuration;
    guint32 pending_configure_serial;
    guint64 pending_mapping_epoch;
    enum wl_output_transform pending_transform;
    AncoWaylandGeometry pending_geometry, submitted_geometry;
    /* Compositor-owned SERVER decoration margins in surface logical units.
     * Roots stay undecorated here because their caption belongs to the native
     * Harmony Task. Parented toplevels are attached original AnCo Windows. */
    AncoWmsRect pending_decoration, submitted_decoration;
    /* Surface-local logical bounds from the SAME commit as the real buffer.
     * Input inversion must not borrow a later mutable surface.current. */
    gint pending_surface_width, pending_surface_height;
    gint submitted_surface_width, submitted_surface_height;
    gboolean submitted_geometry_valid;
    AncoGraphicLayer pending_layer, submitted_layer;
    gboolean pending_layer_valid, submitted_layer_valid;
    guint32 submitted_sequence;
    struct wlr_buffer *pending_buffer;
    guint32 pending_sequence;
    AncoShmQueue *queue;
    guint submit_idle;
    gboolean retiring,submit_failed;
    gboolean wms_create_attempted;
    gboolean wms_create_entered;
    gboolean wms_show_entered;
    /* Linux xdg_toplevel.set_parent keeps its source application ownership,
     * while the native carrier uses the original AnCo system-task branch.
     * This is not a second Task or a new host window API. */
    gboolean native_system_window;
    gboolean native_system_id_valid;
    gint32 native_system_id;
    gint32 native_frontend_pid;
    guint64 native_frontend_start_ticks,native_close_request_serial;
    guint64 native_move_request_serial;
    guint64 native_window_request_serial;
    guint32 native_window_request;
    guint64 native_resize_request_serial;
    AncoWmsRect native_resize_request;
    gboolean native_size_limits_known;
    guint32 native_min_width,native_min_height,native_max_width,native_max_height;
    gboolean native_close_request_serial_known;
    gboolean pointer_no_target_reported;
    gboolean initial_commit_pending;
    gboolean initial_configure_sent;
    gint32 original_type;
    gboolean reported_buffer;
    AncoDelegatorSurfaceKey surface_key;
    AncoDelegatorProducer *producer;
    gboolean producer_bound;
    AncoWmsRect layout_frame, layout_insets;
    gboolean layout_attempted;
    gboolean manual_task_position_valid;
    gint32 manual_task_x, manual_task_y;
    gboolean resize_anchor_valid;
    guint32 resize_anchor_edges, resize_finalize_serial;
    gint32 resize_anchor_right_task, resize_anchor_bottom_task;
};

typedef struct {
    struct wlr_buffer base;
    guint8 *pixels;
    size_t stride;
} RuntimeStartingBuffer;

struct RuntimeStartingWindow {
    Runtime *runtime;
    RuntimeTask *task;
    gint32 activity_id, window_id;
    AncoDelegatorSurfaceKey surface_key;
    AncoDelegatorProducer *producer;
    AncoShmQueue *queue;
    RuntimeStartingBuffer *buffer;
    AncoGraphicLayer layer;
    guint32 sequence;
    guint submit_idle;
    gboolean wms_created, wms_shown, producer_bound, submitted, retiring;
};

static struct wlr_surface *runtime_window_surface(const RuntimeWindow *window)
{
    if (!window) return NULL;
    if (window->xdg) return window->xdg->surface;
#if WLR_HAS_XWAYLAND
    if (window->xwayland_surface) return window->xwayland_surface->surface;
#endif
    return NULL;
}

static gboolean runtime_window_is_toplevel(const RuntimeWindow *window)
{
    if (!window) return FALSE;
    if (window->xdg)
        return window->xdg->role == WLR_XDG_SURFACE_ROLE_TOPLEVEL;
#if WLR_HAS_XWAYLAND
    return window->xwayland_surface &&
        !window->xwayland_surface->override_redirect;
#else
    return FALSE;
#endif
}

static gboolean runtime_window_is_popup(const RuntimeWindow *window)
{
    if (!window) return FALSE;
    if (window->xdg)
        return window->xdg->role == WLR_XDG_SURFACE_ROLE_POPUP;
#if WLR_HAS_XWAYLAND
    return window->xwayland_surface &&
        window->xwayland_surface->override_redirect;
#else
    return FALSE;
#endif
}

static const char *runtime_window_title(const RuntimeWindow *window)
{
    if (!window) return NULL;
    if (window->xdg && runtime_window_is_toplevel(window))
        return window->xdg->toplevel->title;
#if WLR_HAS_XWAYLAND
    if (window->xwayland_surface) return window->xwayland_surface->title;
#endif
    return NULL;
}

/* Standard client size constraints, kept in client logical units.  They are
 * projected by the original frontend through its real VPR and frame margins;
 * the Linux runtime must not guess Rosen's native caption size.  A zero
 * maximum keeps the Wayland/X11 "unbounded" meaning. */
static void runtime_window_size_limits(const RuntimeWindow *window,
    guint32 *min_width,guint32 *min_height,guint32 *max_width,guint32 *max_height)
{
    *min_width=0;*min_height=0;*max_width=0;*max_height=0;
    if(!window || !runtime_window_is_toplevel(window))return;
    if(window->xdg){
        const struct wlr_xdg_toplevel_state *state=&window->xdg->toplevel->current;
        *min_width=state->min_width>0?(guint32)state->min_width:0;
        *min_height=state->min_height>0?(guint32)state->min_height:0;
        *max_width=state->max_width>0?(guint32)state->max_width:0;
        *max_height=state->max_height>0?(guint32)state->max_height:0;
        return;
    }
#if WLR_HAS_XWAYLAND
    if(window->xwayland_surface && window->xwayland_surface->size_hints){
        const xcb_size_hints_t *hints=window->xwayland_surface->size_hints;
        if(hints->flags&XCB_ICCCM_SIZE_HINT_P_MIN_SIZE){
            *min_width=hints->min_width>0?(guint32)hints->min_width:0;
            *min_height=hints->min_height>0?(guint32)hints->min_height:0;
        }
        if(hints->flags&XCB_ICCCM_SIZE_HINT_P_MAX_SIZE){
            *max_width=hints->max_width>0?(guint32)hints->max_width:0;
            *max_height=hints->max_height>0?(guint32)hints->max_height:0;
        }
    }
#endif
}

static gboolean runtime_window_is_xwayland(const RuntimeWindow *window)
{
#if WLR_HAS_XWAYLAND
    return window && window->xwayland_surface;
#else
    (void)window;
    return FALSE;
#endif
}

/* Xwayland exposes an X11 pixel grid.  Xft.dpi carries the host density into
 * toolkits, so applying the Wayland fractional output scale again would
 * enlarge and filter the submitted pixels a second time.  Native Wayland
 * clients continue to use the host fractional scale. */
static double runtime_window_content_scale(const RuntimeWindow *window)
{
    return runtime_window_is_xwayland(window) ? 1.0 :
        window->runtime->display_info.scale;
}

#if WLR_HAS_XWAYLAND
static gboolean xwayland_surface_has_atom(const Runtime *runtime,
    const struct wlr_xwayland_surface *surface,xcb_atom_t atom)
{
    if(!runtime || !surface || atom==XCB_ATOM_NONE)return FALSE;
    for(size_t i=0;i<surface->window_type_len;++i)
        if(surface->window_type[i]==atom)return TRUE;
    return FALSE;
}

static gboolean runtime_window_decor_visible(const RuntimeWindow *window)
{
    if(!window)return TRUE;
    if(window->xdg && runtime_window_is_toplevel(window)){
        struct wlr_xdg_toplevel_decoration_v1 *xdg_decoration;
        wl_list_for_each(xdg_decoration,
            &window->runtime->decoration_manager->decorations,link){
            if(xdg_decoration->toplevel!=window->xdg->toplevel)continue;
            enum wlr_xdg_toplevel_decoration_v1_mode mode=
                xdg_decoration->current.mode;
            if(mode==WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_NONE)
                mode=xdg_decoration->scheduled_mode;
            if(mode==WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_NONE)
                mode=xdg_decoration->requested_mode;
            return mode!=WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE;
        }
        struct wlr_server_decoration *legacy;
        wl_list_for_each(legacy,
            &window->runtime->legacy_decoration_manager->decorations,link){
            if(legacy->surface==runtime_window_surface(window))
                return legacy->mode!=WLR_SERVER_DECORATION_MANAGER_MODE_CLIENT;
        }
        /* The advertised KDE server-decoration protocol defines a surface
         * which creates no decoration object as client-side decorated.  GTK
         * creates the legacy object and follows our SERVER default; Qt clients
         * may use xdg-decoration.  A client which binds neither protocol keeps
         * ownership of the frame already drawn into its wl_surface. */
        return FALSE;
    }
    if(!window->xwayland_surface)return TRUE;
    const struct wlr_xwayland_surface *surface=window->xwayland_surface;
    const guint32 hidden=WLR_XWAYLAND_SURFACE_DECORATIONS_NO_BORDER |
        WLR_XWAYLAND_SURFACE_DECORATIONS_NO_TITLE;
    return (surface->decorations&hidden)==0 &&
        !xwayland_surface_has_atom(window->runtime,surface,
            window->runtime->xwayland_kde_override_atom);
}
#else
static gboolean runtime_window_decor_visible(const RuntimeWindow *window)
{
    (void)window;return TRUE;
}
#endif

static void runtime_window_send_close(RuntimeWindow *window)
{
    if (!window) return;
    if (window->xdg && runtime_window_is_toplevel(window)) {
        wlr_xdg_toplevel_send_close(window->xdg->toplevel);
        return;
    }
#if WLR_HAS_XWAYLAND
    if (window->xwayland_surface)
        wlr_xwayland_surface_close(window->xwayland_surface);
#endif
}

static void runtime_window_set_activated(RuntimeWindow *window,
    gboolean activated)
{
    if (!window) return;
    if (window->xdg && runtime_window_is_toplevel(window)) {
        wlr_xdg_toplevel_set_activated(window->xdg->toplevel, activated);
        return;
    }
#if WLR_HAS_XWAYLAND
    if (window->xwayland_surface) {
        /* wlroots maps each managed X11 surface at the bottom of its X stack.
         * The compositor must mirror its native focus order explicitly: XWM
         * activation updates focus and EWMH state, but deliberately does not
         * restack.  Keep the active Linux window above its X11 siblings so
         * Xwayland hit-testing agrees with the original AnCo target window. */
        if (activated && !window->xwayland_surface->override_redirect)
            wlr_xwayland_surface_restack(window->xwayland_surface, NULL,
                XCB_STACK_MODE_ABOVE);
        wlr_xwayland_surface_activate(window->xwayland_surface, activated);
    }
#endif
}

struct RuntimePendingToplevel {
    Runtime *runtime;
    struct wlr_xdg_surface *xdg;
    struct wl_listener commit, destroy;
    guint admit_idle;
    gboolean initial_commit_received;
};

#if WLR_HAS_XWAYLAND
struct RuntimePendingXwayland {
    Runtime *runtime;
    struct wlr_xwayland_surface *surface;
    struct wl_listener associate, destroy;
    /* A root X11 Window can outlive a wl_surface association.  Retire its
     * semantic Task only if the X Window itself is destroyed before a later
     * associate, never merely because X11 temporarily unmapped it. */
    gint32 retire_task;
};
#endif

static void original_task_remove_received(AncoLinuxCatalog *catalog, gint32 task,
    const char *reason, void *data)
{
    Runtime *runtime=data;
    g_assert(runtime->catalog==catalog);
    guint windows=0;
    for(GList *node=runtime->windows;node;node=node->next){
        RuntimeWindow *window=node->data;
        if(window->retiring || window->identity.owner.task_id!=task ||
            !runtime_window_is_toplevel(window))continue;
        runtime_window_send_close(window);
        ++windows;
    }
    printf("RUNTIME_ORIGINAL_TASK_REMOVE task=%d windows=%u reason=%s standard_xdg_close=1\n",
        task,windows,reason?reason:"");
    if(windows && runtime->wayland)anco_wayland_context_wake(runtime->wayland);
}

static RuntimeTask *live_task_state(Runtime *,gint32);
static gboolean window_receives_keys(RuntimeWindow *);
static void update_original_window_layout_force(RuntimeWindow *,
    const AncoWaylandGeometry *,gboolean);

static gboolean root_native_title_owner(const RuntimeWindow *window)
{
    if(!window || !runtime_window_is_toplevel(window))return FALSE;
#if WLR_HAS_XWAYLAND
    if (window->xwayland_surface)
        return window->xwayland_surface->parent == NULL;
#endif
    struct wlr_xdg_toplevel *top=window->xdg->toplevel;
    if(!top->parent)return TRUE;
    /* Admission has already assigned the real ownership before initial title
     * publication, while original_type is assigned only on first WMS create.
     * Use that same identity relation here: a cross-Task Linux toplevel owns
     * its new Task frame; only a same-Task attached Window does not. */
    AncoLinuxWindowOwner parent;
    if(anco_wayland_ownership_surface_window(window->runtime->ownership,
       top->parent->base->surface->resource,&parent))return FALSE;
    return parent.owner.task_id!=window->identity.owner.task_id ||
        parent.owner.activity_id!=window->identity.owner.activity_id;
}

static char *native_title_path(const RuntimeWindow *window)
{
    if(!root_native_title_owner(window))return NULL;
    const char *directory=g_getenv("ANCO_NATIVE_WINDOW_TITLE_DIR");
    RuntimeTask *task=window?live_task_state(window->runtime,
        window->identity.owner.task_id):NULL;
    gint32 pid=task?task->frontend_pid:0;
    if(pid<=0){
        const char *frontend=g_getenv("ANCO_NATIVE_FRONTEND_PID");
        char *end=NULL;errno=0;
        const long fallback=frontend?strtol(frontend,&end,10):-1;
        if(!frontend||!*frontend||errno||*end||fallback<=0||fallback>G_MAXINT32)
            return NULL;
        pid=(gint32)fallback;
    }
    if(!directory || directory[0]!='/')return NULL;
    return g_strdup_printf("%s/title-%d-%d.json",directory,pid,
        window->identity.owner.task_id);
}

static int publish_native_metadata(RuntimeWindow *window)
{
    g_autofree char *path=native_title_path(window);
    if(!path)return -EINVAL;
    AncoLinuxLaunch launch;
    if(anco_linux_catalog_task(window->runtime->catalog,
       window->identity.owner.task_id,&launch))return -ENOENT;
    const char *title=runtime_window_title(window);
    if(title && *title && (!g_utf8_validate(title,-1,NULL) ||
       strlen(title)>1024))return -EINVAL;
    gint32 modes[4]={0};guint mode_count=0;
    const int mode_status=anco_linux_catalog_task_window_modes(
        window->runtime->catalog,window->identity.owner.task_id,modes,&mode_count);
    if(mode_status)return mode_status;
    JsonBuilder *builder=json_builder_new();
    json_builder_begin_object(builder);
    json_builder_set_member_name(builder,"frontendPid");
    RuntimeTask *task=live_task_state(window->runtime,
        window->identity.owner.task_id);
    json_builder_add_int_value(builder,task&&task->frontend_pid>0?
        task->frontend_pid:strtol(g_getenv("ANCO_NATIVE_FRONTEND_PID"),NULL,10));
    json_builder_set_member_name(builder,"taskId");
    json_builder_add_int_value(builder,window->identity.owner.task_id);
    json_builder_set_member_name(builder,"bundleName");
    json_builder_add_string_value(builder,launch.app.bundle);
    json_builder_set_member_name(builder,"title");
    json_builder_add_string_value(builder,title ? title : "");
    json_builder_set_member_name(builder,"decorVisible");
    json_builder_add_boolean_value(builder,runtime_window_decor_visible(window));
    guint32 min_width=0,min_height=0,max_width=0,max_height=0;
    runtime_window_size_limits(window,&min_width,&min_height,&max_width,&max_height);
    json_builder_set_member_name(builder,"contentSizeLimits");
    json_builder_begin_object(builder);
    json_builder_set_member_name(builder,"minWidth");
    json_builder_add_int_value(builder,min_width);
    json_builder_set_member_name(builder,"minHeight");
    json_builder_add_int_value(builder,min_height);
    json_builder_set_member_name(builder,"maxWidth");
    json_builder_add_int_value(builder,max_width);
    json_builder_set_member_name(builder,"maxHeight");
    json_builder_add_int_value(builder,max_height);
    json_builder_end_object(builder);
    json_builder_set_member_name(builder,"moveRequestSerial");
    json_builder_add_int_value(builder,(gint64)window->native_move_request_serial);
    json_builder_set_member_name(builder,"windowRequestSerial");
    json_builder_add_int_value(builder,(gint64)window->native_window_request_serial);
    json_builder_set_member_name(builder,"windowRequest");
    json_builder_add_int_value(builder,window->native_window_request);
    json_builder_set_member_name(builder,"resizeRequestSerial");
    json_builder_add_int_value(builder,(gint64)window->native_resize_request_serial);
    json_builder_set_member_name(builder,"resizeRequest");
    json_builder_begin_object(builder);
    json_builder_set_member_name(builder,"left");
    json_builder_add_int_value(builder,window->native_resize_request.left);
    json_builder_set_member_name(builder,"top");
    json_builder_add_int_value(builder,window->native_resize_request.top);
    json_builder_set_member_name(builder,"width");
    json_builder_add_int_value(builder,(gint64)window->native_resize_request.right-
        window->native_resize_request.left);
    json_builder_set_member_name(builder,"height");
    json_builder_add_int_value(builder,(gint64)window->native_resize_request.bottom-
        window->native_resize_request.top);
    json_builder_end_object(builder);
    json_builder_set_member_name(builder,"supportWindowModes");
    json_builder_begin_array(builder);
    for(guint i=0;i<mode_count;++i)json_builder_add_int_value(builder,modes[i]);
    json_builder_end_array(builder);
    json_builder_end_object(builder);
    JsonGenerator *generator=json_generator_new();
    JsonNode *root=json_builder_get_root(builder);
    json_generator_set_root(generator,root);
    g_autofree char *text=json_generator_to_data(generator,NULL);
    g_autofree char *temporary=g_strconcat(path,".tmp",NULL);
    GError *error=NULL;
    gboolean written=g_file_set_contents(temporary,text,-1,&error);
    int result=0;
    if(!written)result=error ? -error->code : -EIO;
    else if(chmod(temporary,0644) || rename(temporary,path))result=-errno;
    if(result)unlink(temporary);
    printf("RUNTIME_NATIVE_METADATA_PUBLISH window=%d task=%d modes=%u status=%d original_support_modes_pending=%d\n",
        window->identity.window_id,window->identity.owner.task_id,mode_count,result,!result);
    if(title && *title)printf("RUNTIME_NATIVE_TITLE_PUBLISH window=%d task=%d title=%s status=%d original_set_label_pending=%d\n",
        window->identity.window_id,window->identity.owner.task_id,title,result,!result);
    if(error){fprintf(stderr,"RUNTIME_NATIVE_TITLE_ERROR %s\n",error->message);g_error_free(error);}
    json_node_free(root);g_object_unref(generator);g_object_unref(builder);
    return result;
}

enum NativeWindowRequest {
    NATIVE_WINDOW_REQUEST_NONE=0,
    NATIVE_WINDOW_REQUEST_MAXIMIZE=1,
    NATIVE_WINDOW_REQUEST_RECOVER=2,
    NATIVE_WINDOW_REQUEST_MINIMIZE=3,
    NATIVE_WINDOW_REQUEST_FULLSCREEN=4,
    NATIVE_WINDOW_REQUEST_EXIT_FULLSCREEN=5,
    NATIVE_WINDOW_REQUEST_FOCUS=6,
};

static gboolean request_native_window(RuntimeWindow *window,guint32 request,
    const char *protocol)
{
    if(!root_native_title_owner(window) || request<NATIVE_WINDOW_REQUEST_MAXIMIZE ||
       request>NATIVE_WINDOW_REQUEST_FOCUS ||
       window->native_window_request_serial>=G_MAXINT64)return FALSE;
    ++window->native_window_request_serial;
    window->native_window_request=request;
    const int status=publish_native_metadata(window);
    printf("RUNTIME_NATIVE_WINDOW_REQUEST window=%d task=%d serial=%llu request=%u protocol=%s publish=%d original_window_operation_pending=%d\n",
        window->identity.window_id,window->identity.owner.task_id,
        (unsigned long long)window->native_window_request_serial,request,
        protocol,status,!status);
    if(status)return FALSE;
    if(window->submitted_geometry_valid)
        update_original_window_layout_force(window,&window->submitted_geometry,TRUE);
    else if(window->pending_buffer)
        update_original_window_layout_force(window,&window->pending_geometry,TRUE);
    return TRUE;
}

static gboolean request_native_move(RuntimeWindow *window,const char *protocol)
{
    if(!root_native_title_owner(window) ||
       window->native_move_request_serial>=G_MAXINT64)return FALSE;
    ++window->native_move_request_serial;
    const int status=publish_native_metadata(window);
    printf("RUNTIME_NATIVE_MOVE window=%d task=%d serial=%llu protocol=%s publish=%d original_start_move_pending=%d\n",
        window->identity.window_id,window->identity.owner.task_id,
        (unsigned long long)window->native_move_request_serial,protocol,status,!status);
    if(status)return FALSE;
    if(window->submitted_geometry_valid)
        update_original_window_layout_force(window,&window->submitted_geometry,TRUE);
    else if(window->pending_buffer)
        update_original_window_layout_force(window,&window->pending_geometry,TRUE);
    return TRUE;
}

static gboolean request_native_resize(RuntimeWindow *window,
    const AncoWmsRect *outer)
{
    const gint64 width=outer?(gint64)outer->right-outer->left:0;
    const gint64 height=outer?(gint64)outer->bottom-outer->top:0;
    if(!root_native_title_owner(window) || !outer || width<1 || height<1 ||
       width>16384 || height>16384 ||
       window->native_resize_request_serial>=G_MAXINT64)return FALSE;
    ++window->native_resize_request_serial;
    window->native_resize_request=*outer;
    const int status=publish_native_metadata(window);
    printf("RUNTIME_NATIVE_RESIZE window=%d task=%d serial=%llu target=%d,%d,%d,%d publish=%d original_resize_move_pending=%d\n",
        window->identity.window_id,window->identity.owner.task_id,
        (unsigned long long)window->native_resize_request_serial,
        outer->left,outer->top,outer->right,outer->bottom,status,!status);
    if(status)return FALSE;
    if(window->submitted_geometry_valid)
        update_original_window_layout_force(window,&window->submitted_geometry,TRUE);
    else if(window->pending_buffer)
        update_original_window_layout_force(window,&window->pending_geometry,TRUE);
    return TRUE;
}

static void remove_native_title(RuntimeWindow *window)
{
    g_autofree char *path=native_title_path(window);
    if(path)unlink(path);
}

typedef struct { AncoTextSeat *seat; } TextSeatBinding;
typedef struct {
    AncoTextSeat *seat;
    struct wl_listener resource_created,destroy;
} RuntimeTextClient;
static void text_client_destroyed(struct wl_listener *listener,void *data)
{
    (void)data;RuntimeTextClient *client=wl_container_of(listener,client,destroy);
    wl_list_remove(&client->resource_created.link);wl_list_remove(&client->destroy.link);
    g_free(client);
}
static void text_client_resource_created(struct wl_listener *listener,void *data)
{
    RuntimeTextClient *client=wl_container_of(listener,client,resource_created);
    struct wl_resource *resource=data;
    if(!strcmp(wl_resource_get_class(resource),"wl_seat")){
        const int status=anco_text_seat_bind(client->seat,resource);
        printf("RUNTIME_TEXT_SEAT_BIND resource=%u status=%d source=resource-created\n",
            wl_resource_get_id(resource),status);
    }
}
static void text_client_created(struct wl_listener *listener,void *data)
{
    Runtime *runtime=wl_container_of(listener,runtime,new_client);
    struct wl_client *wayland_client=data;
    RuntimeTextClient *client=g_new0(RuntimeTextClient,1);client->seat=runtime->text_seat;
    client->resource_created.notify=text_client_resource_created;
    client->destroy.notify=text_client_destroyed;
    wl_client_add_resource_created_listener(wayland_client,&client->resource_created);
    wl_client_add_destroy_listener(wayland_client,&client->destroy);
}
static enum wl_iterator_result bind_text_seat_resource(struct wl_resource *resource,void *data)
{
    TextSeatBinding *binding=data;
    if(!strcmp(wl_resource_get_class(resource),"wl_seat"))
        (void)anco_text_seat_bind(binding->seat,resource);
    return WL_ITERATOR_CONTINUE;
}
static gboolean effective_task_geometry(const RuntimeTask *task,
    AncoWmsRect *bounds,gint32 *mode,float *scale);
static RuntimeWindow *runtime_window_id(Runtime *runtime,gint32 id)
{
    for(GList *node=runtime->windows;node;node=node->next){
        RuntimeWindow *window=node->data;
        if(!window->retiring&&window->identity.window_id==id)return window;
    }
    return NULL;
}
static void text_cursor(Runtime *runtime,const AncoTextEditor *editor,
    const AncoLinuxWindowOwner *owner)
{
    if(!runtime->imf||!editor||!owner)return;
    int32_t x=0,y=0,width=0,height=0;bool valid=false;
    if(editor->protocol==2){
        const AncoTextV2Fields *f=&editor->state.v2->current;
        valid=f->rect_valid;x=f->x;y=f->y;width=f->width;height=f->height;
    }else{
        const AncoTextRect *r=&editor->state.v3->current.rect;
        valid=r->valid;x=r->x;y=r->y;width=r->width;height=r->height;
    }
    if(!valid||height<=0)return;
    RuntimeWindow *window=runtime_window_id(runtime,owner->window_id);
    RuntimeTask *task=window?live_task_state(runtime,owner->owner.task_id):NULL;
    AncoWmsRect bounds;gint32 mode;float host_scale;
    const double scale=window?runtime_window_content_scale(window):0;
    if(!window||!task||!window->submitted_layer_valid||
       !effective_task_geometry(task,&bounds,&mode,&host_scale)||
       !task->client_area_valid||!isfinite(scale)||scale<=0)return;
    const double global_x=bounds.left+task->client_area.left+
        window->submitted_layer.display_frame[0]+
        (window->submitted_decoration.left+x)*scale;
    const double global_y=bounds.top+task->client_area.top+
        window->submitted_layer.display_frame[1]+
        (window->submitted_decoration.top+y)*scale;
    anco_linux_imf_cursor(runtime->imf,global_x,global_y,height*scale);
    printf("RUNTIME_IMF_CURSOR window=%d protocol=%u local=%d,%d,%d,%d native=%.3f,%.3f,%.3f\n",
        owner->window_id,editor->protocol,x,y,width,height,global_x,global_y,
        height*scale);
}
static void text_changed(void *data,AncoTextSeat *seat,struct wl_resource *input,
    const AncoTextEditor *editor,const AncoLinuxWindowOwner *owner,bool boundary)
{
    (void)seat;(void)input;Runtime *runtime=data;
    if(!editor||!owner){
        anco_linux_imf_blur(runtime->imf);
        puts("RUNTIME_TEXT_EDITOR enabled=0 original_imf_blur=1");
        return;
    }
    if(boundary)anco_linux_imf_focus(runtime->imf,true,0,0,0);
    const char *text=NULL;int32_t cursor=0,anchor=0;
    const int selection=anco_text_seat_surrounding(seat,&text,&cursor,&anchor);
    const int selection_forwarded=selection?selection:
        anco_linux_imf_selection(runtime->imf,text,cursor,anchor);
    text_cursor(runtime,editor,owner);
    printf("RUNTIME_TEXT_EDITOR window=%d protocol=%u boundary=%d original_imf_focus=%d selection_forwarded=%d\n",
        owner->window_id,editor->protocol,boundary,boundary,selection_forwarded);
}
static void text_panel(void *data,AncoTextSeat *seat,struct wl_resource *input,
    const AncoLinuxWindowOwner *owner,bool show)
{
    (void)seat;(void)input;(void)owner;
    Runtime *runtime=data;anco_linux_imf_panel(runtime->imf,show);
    printf("RUNTIME_TEXT_PANEL show=%d original_imf_request=1\n",show);
}

static gboolean ibus_focus(void *data,gboolean focused)
{
    Runtime *runtime=data;
    RuntimeWindow *window=runtime->focused_window;
    const gboolean admitted=focused && !runtime->stopping &&
        runtime->container_focused && window &&
        window_receives_keys(window);
    if(!focused || !admitted){
        anco_linux_imf_blur(runtime->imf);
        printf("RUNTIME_IBUS_HOST_FOCUS requested=%d admitted=0 window=%d protocol=%s\n",
            focused,window?window->identity.window_id:-1,
            window?(runtime_window_is_xwayland(window)?"x11":"xdg"):"none");
        return FALSE;
    }
    /* A standard IBus FocusIn is itself the Linux editor's chosen input
     * context, including Qt/Wayland clients using QT_IM_MODULE=ibus.  Clear
     * only a stale text-input-v2/v3 selection, then attach the same original
     * AnCo IMF runtime; never run two editor routes for one focused window. */
    if(runtime->text_seat)(void)anco_text_seat_focus(runtime->text_seat,NULL);
    const int status=anco_linux_imf_focus(runtime->imf,true,0,0,0);
    printf("RUNTIME_IBUS_HOST_FOCUS requested=1 admitted=%d window=%d protocol=%s status=%d\n",
        status==0,window->identity.window_id,
        runtime_window_is_xwayland(window)?"x11":"xdg",status);
    return status==0;
}

static void ibus_panel(void *data,gboolean shown)
{
    Runtime *runtime=data;
    RuntimeWindow *window=runtime->focused_window;
    if(shown && (runtime->stopping || !runtime->container_focused || !window ||
       !window_receives_keys(window)))return;
    anco_linux_imf_panel(runtime->imf,shown);
    printf("RUNTIME_IBUS_HOST_PANEL show=%d window=%d original_imf_request=1\n",
        shown,window?window->identity.window_id:-1);
}

static void ibus_cursor(void *data,gint x,gint y,gint width,gint height)
{
    Runtime *runtime=data;RuntimeWindow *window=runtime->focused_window;
    if(!window||!window_receives_keys(window)||
       !runtime->container_focused||height<=0)return;
#if WLR_HAS_XWAYLAND
    double global_x=0,global_y=0,global_height=height;
    gint window_x=0,window_y=0;
    if(runtime_window_is_xwayland(window)){
        RuntimeTask *task=live_task_state(runtime,window->identity.owner.task_id);
        const double scale=runtime_window_content_scale(window);
        if(!task||!task->client_area_valid||!isfinite(scale)||scale<=0)return;
        struct wlr_xwayland_surface *surface=window->xwayland_surface;
        window_x=surface->x;window_y=surface->y;
        global_x=window->layout_frame.left+((double)x-surface->x)*scale;
        global_y=window->layout_frame.top+((double)y-surface->y)*scale;
        global_height=height*scale;
    }else{
        /* SetCursorLocationRelative is expressed in the Wayland client's
         * local logical coordinate space.  The AnCo IMF cursor ABI consumes
         * native display coordinates, matching the existing text-input
         * cursor path and the layer projection into layout_frame. */
        const double scale=runtime_window_content_scale(window);
        if(!isfinite(scale)||scale<=0)return;
        global_x=window->layout_frame.left+x*scale;
        global_y=window->layout_frame.top+y*scale;
        global_height=height*scale;
    }
    anco_linux_imf_cursor(runtime->imf,global_x,global_y,
        global_height);
    printf("RUNTIME_IBUS_HOST_CURSOR window=%d protocol=%s input=%d,%d,%d,%d xwindow=%d,%d native=%.3f,%.3f,%.3f\n",
        window->identity.window_id,
        runtime_window_is_xwayland(window)?"x11":"xdg",x,y,width,height,
        window_x,window_y,global_x,global_y,global_height);
#else
    (void)x;(void)y;(void)width;
#endif
}

static void ibus_surrounding(void *data,const char *text,guint cursor_chars,
    guint anchor_chars)
{
    Runtime *runtime=data;RuntimeWindow *window=runtime->focused_window;
    if(!window||!window_receives_keys(window)||
       !runtime->container_focused||!text)return;
    const char *cursor=g_utf8_offset_to_pointer(text,cursor_chars);
    const char *anchor=g_utf8_offset_to_pointer(text,anchor_chars);
    const int status=anco_linux_imf_selection(runtime->imf,text,
        (int32_t)(cursor-text),(int32_t)(anchor-text));
    printf("RUNTIME_IBUS_HOST_SELECTION window=%d bytes=%zu cursor=%td anchor=%td status=%d\n",
        window->identity.window_id,strlen(text),cursor-text,anchor-text,status);
}

static void recompute_window_focus(Runtime *runtime);
static void schedule_layer_scene(Runtime *runtime);
static void update_original_window_layout(RuntimeWindow *window,
    const AncoWaylandGeometry *logical);
static gboolean rect_has_margins(const AncoWmsRect *rect);
static int physical_server_decoration(const RuntimeWindow *window,
    const struct wlr_buffer *buffer,const AncoWmsRect *logical,
    AncoShmDecoration *out);
static RuntimeWindow *runtime_window_for_xdg(Runtime *runtime,
    struct wlr_xdg_surface *xdg);
static void refresh_native_decorations(Runtime *runtime);
static gboolean interactive_move_update(Runtime *runtime,double display_x,double display_y);
static void interactive_move_finish(Runtime *runtime,const char *reason);
static gboolean server_decoration_begin_move(RuntimeWindow *window);
static void toplevel_request_move(struct wl_listener *listener,void *data);
static void toplevel_request_maximize(struct wl_listener *listener,void *data);
static void toplevel_request_minimize(struct wl_listener *listener,void *data);
static void toplevel_request_fullscreen(struct wl_listener *listener,void *data);
static gboolean interactive_resize_update(Runtime *runtime,double display_x,double display_y);
static void interactive_resize_finish(Runtime *runtime,gboolean final_configure,
    const char *reason);
static gboolean server_decoration_begin_resize(RuntimeWindow *window,guint32 edges);
static void toplevel_request_resize(struct wl_listener *listener,void *data);
static void schedule_initial_commits_for_task(Runtime *runtime,RuntimeTask *task);
static void schedule_pending_toplevels(Runtime *runtime,const char *reason);

static void task_state_free(gpointer data)
{
    RuntimeTask *task = data;
    g_assert(task->starting_window==NULL);
    g_array_free(task->activities, TRUE);
    g_free(task);
}

static RuntimeTask *live_task_state(Runtime *runtime, gint32 id)
{
    AncoLinuxLaunch launch;
    if (anco_linux_catalog_task(runtime->catalog,id,&launch)) return NULL;
    return g_hash_table_lookup(runtime->tasks,GINT_TO_POINTER(id));
}

static int no_background_window_policy(AncoLinuxCatalog *catalog,
    const AncoLinuxLaunch *start,GArray *tasks,void *data)
{
    Runtime *runtime=data;
    if(!runtime || runtime->catalog!=catalog || !start || !tasks)return -EINVAL;
    printf("RUNTIME_NO_BACKGROUND_WINDOW_POLICY start=%d background_count=0 window_stack_owned_by_linux=1\n",
        start->task_id);
    return 0;
}

/* The stock frontend exposes the same root Task through two original paths:
 * callback3 carries Task bounds, while the real Rosen Window/client-area
 * listener carries the owning root window rectangle. SP68 can omit callback3
 * for an already-created freeform session. In that finite root case the real
 * AnCo main Window is the Task presentation boundary; retain which original
 * source supplied it instead of inventing a desktop/output rectangle. */
static gboolean effective_task_geometry(const RuntimeTask *task,
    AncoWmsRect *bounds,gint32 *mode,float *scale)
{
    if(!task || !bounds || !mode || !scale)return FALSE;
    if(task->host_bounds_valid){
        if(task->mode!=1 && task->mode!=102)return FALSE;
        *bounds=task->host_bounds;*mode=task->mode;*scale=task->host_scale;
        return task->host_scale==1.0f;
    }
    if(!task->client_area_valid || (task->client_mode!=1 && task->client_mode!=102))
        return FALSE;
    *bounds=task->client_outer;*mode=task->client_mode;*scale=1.0f;
    return bounds->right>bounds->left && bounds->bottom>bounds->top;
}

typedef struct {
    RuntimeWindow *window;
    struct wlr_surface *surface;
    double surface_x, surface_y;
    double surface_origin_x, surface_origin_y;
    gboolean server_decoration;
    gboolean close_button;
    guint32 resize_edges;
} RuntimePointerHit;

static RuntimeTask *input_pointer_task(Runtime *runtime,gint32 host_window)
{
    RuntimeTask *selected=NULL;
    GHashTableIter iter;gpointer value;
    g_hash_table_iter_init(&iter,runtime->tasks);
    while(g_hash_table_iter_next(&iter,NULL,&value)){
        RuntimeTask *task=value;
        if(!task->client_area_valid || task->client_native_id!=host_window ||
            !live_task_state(runtime,task->id))continue;
        if(selected)return NULL;
        selected=task;
    }
    return selected;
}

static RuntimeWindow *input_pointer_system_window(Runtime *runtime,gint32 host_window)
{
    RuntimeWindow *selected=NULL;
    for(GList *node=runtime->windows;node;node=node->next){
        RuntimeWindow *window=node->data;
        if(!window->native_system_window || !window->native_system_id_valid ||
           window->native_system_id!=host_window || window->retiring)continue;
        if(selected)return NULL;
        selected=window;
    }
    return selected;
}

static gboolean input_pointer_root_coordinates(RuntimeWindow *window,
    RuntimeTask *task,double display_x,double display_y,double *root_x,double *root_y)
{
    AncoWmsRect bounds;gint32 mode;float host_scale;
    const double scale=runtime_window_content_scale(window);
    if(window->retiring || !runtime_window_surface(window)->mapped ||
        !window->submitted_geometry_valid || !window->submitted_layer_valid ||
        !effective_task_geometry(task,&bounds,&mode,&host_scale) || !task->client_area_valid ||
        !isfinite(scale) || scale<=0)return FALSE;
    const double origin_x=(double)bounds.left+task->client_area.left+
        window->submitted_layer.display_frame[0];
    const double origin_y=(double)bounds.top+task->client_area.top+
        window->submitted_layer.display_frame[1];
    const double sx=(display_x-origin_x)/scale;
    const double sy=(display_y-origin_y)/scale;
    if(!isfinite(sx)||!isfinite(sy) || fabs(sx)>INT32_MAX || fabs(sy)>INT32_MAX)
        return FALSE;
    *root_x=sx;*root_y=sy;return TRUE;
}

static gboolean input_pointer_system_coordinates(RuntimeWindow *window,
    double display_x,double display_y,double *root_x,double *root_y)
{
    const double scale=runtime_window_content_scale(window);
    RuntimeTask *task=live_task_state(window->runtime,
        window->identity.owner.task_id);
    AncoWmsRect bounds;gint32 mode;float host_scale;
    if(!window->native_system_window || !window->native_system_id_valid ||
       window->retiring || !runtime_window_surface(window)->mapped ||
       !window->submitted_geometry_valid || !window->submitted_layer_valid ||
       !effective_task_geometry(task,&bounds,&mode,&host_scale) ||
       !isfinite(scale) || scale<=0)return FALSE;
    /* committed_original_layer() retains every producer frame in its owning
     * Task's local coordinates. publish_layer_scene() adds these exact Task
     * bounds before the original Graphic broker receives it. Invert the same
     * published frame here; task=-1 is only the WMS carrier identity and does
     * not turn the cached producer frame into display-global coordinates. */
    const double origin_x=(double)bounds.left+
        window->submitted_layer.display_frame[0];
    const double origin_y=(double)bounds.top+
        window->submitted_layer.display_frame[1];
    const double sx=(display_x-origin_x)/scale;
    const double sy=(display_y-origin_y)/scale;
    if(!isfinite(sx)||!isfinite(sy) || fabs(sx)>INT32_MAX || fabs(sy)>INT32_MAX)
        return FALSE;
    *root_x=sx;*root_y=sy;return TRUE;
}

static gboolean input_pointer_hit(Runtime *runtime,RuntimeTask *task,
    RuntimeWindow *system_window,double display_x,double display_y,
    RuntimePointerHit *hit)
{
    for(GList *node=g_list_last(runtime->windows);node;node=node->prev){
        RuntimeWindow *window=node->data;
        if(system_window) {
            if(window!=system_window)continue;
        } else if(window->native_system_window ||
                  window->identity.owner.task_id!=task->id)continue;
        double outer_x=0,outer_y=0,surface_x=0,surface_y=0;
        if(system_window) {
            if(!input_pointer_system_coordinates(window,display_x,display_y,
                    &outer_x,&outer_y))continue;
        } else if(!input_pointer_root_coordinates(window,task,display_x,display_y,
                    &outer_x,&outer_y))continue;
        const AncoWmsRect *decor=&window->submitted_decoration;
        if(rect_has_margins(decor)){
            const double client_width=(gint64)window->submitted_geometry.geometry.right-
                window->submitted_geometry.geometry.left;
            const double client_height=(gint64)window->submitted_geometry.geometry.bottom-
                window->submitted_geometry.geometry.top;
            const double outer_width=decor->left+client_width+decor->right;
            const double outer_height=decor->top+client_height+decor->bottom;
            if(outer_x<0||outer_y<0||outer_x>=outer_width||outer_y>=outer_height)
                continue;
            const double edge=6.0;
            guint32 edges=WLR_EDGE_NONE;
            if(outer_x<edge)edges|=WLR_EDGE_LEFT;
            else if(outer_x>=outer_width-edge)edges|=WLR_EDGE_RIGHT;
            if(outer_y<edge)edges|=WLR_EDGE_TOP;
            else if(outer_y>=outer_height-edge)edges|=WLR_EDGE_BOTTOM;
            const gboolean title=outer_y<decor->top;
            const gboolean close=title && !edges && outer_x>=outer_width-decor->top;
            if(edges||title){
                *hit=(RuntimePointerHit){.window=window,.server_decoration=TRUE,
                    .close_button=close,.resize_edges=edges};
                return TRUE;
            }
            /* The submitted layer includes the compositor-drawn server
             * decoration, while xdg_surface_surface_at() consumes coordinates
             * in the client wl_surface.  Keep the outer coordinates above for
             * native move/resize/close handling, then remove the decoration
             * exactly once before entering the unchanged Wayland input path. */
            outer_x-=decor->left;
            outer_y-=decor->top;
        }
        struct wlr_surface *surface=window->xdg ?
            wlr_xdg_surface_surface_at(window->xdg,outer_x,outer_y,
                &surface_x,&surface_y) :
            wlr_surface_surface_at(runtime_window_surface(window),outer_x,
                outer_y,&surface_x,&surface_y);
        if(!surface){
            if(system_window && !window->pointer_no_target_reported){
                const pixman_box32_t *input=
                    pixman_region32_extents(&runtime_window_surface(window)->input_region);
                window->pointer_no_target_reported=TRUE;
                printf("RUNTIME_SYSTEM_POINTER_NO_TARGET window=%d native=%d display=%.3f,%.3f surface=%.6f,%.6f current=%dx%d submitted=%dx%d input=%d,%d,%d,%d contains=%d mapped=%d\n",
                    window->identity.window_id,window->native_system_id,
                    display_x,display_y,outer_x,outer_y,
                    runtime_window_surface(window)->current.width,
                    runtime_window_surface(window)->current.height,
                    window->submitted_surface_width,
                    window->submitted_surface_height,
                    input->x1,input->y1,input->x2,input->y2,
                    pixman_region32_contains_point(
                        &runtime_window_surface(window)->input_region,
                        (int)floor(outer_x),(int)floor(outer_y),NULL),
                    runtime_window_surface(window)->mapped);
            }
            continue;
        }
        window->pointer_no_target_reported=FALSE;
        *hit=(RuntimePointerHit){.window=window,.surface=surface,
            .surface_x=surface_x,.surface_y=surface_y,
            .surface_origin_x=outer_x-surface_x,
            .surface_origin_y=outer_y-surface_y};
        return TRUE;
    }
    return FALSE;
}

static int input_pointer_button(gint32 wire)
{
    switch(wire){
    case 1:return BTN_LEFT;
    case 2:return BTN_RIGHT;
    case 256:return BTN_MIDDLE;
    case 16:return BTN_FORWARD;
    case 8:return BTN_BACK;
    case 128:return BTN_TASK;
    default:return -1;
    }
}

static int input_pointer_start(Runtime *runtime)
{
    if(runtime->input_pointer_ready || !runtime->input_session_open || !runtime->seat)return 0;
    runtime->input_pointer_ready=TRUE;
    wlr_seat_set_capabilities(runtime->seat,
        runtime->seat->capabilities|WL_SEAT_CAPABILITY_POINTER);
    printf("RUNTIME_NATIVE_POINTER_CONNECTED original_session=1 capabilities=%u committed_frame_coordinates=1 standard_seat=1 scroll=1 touch=0\n",
        runtime->seat->capabilities);
    if(runtime->wayland)anco_wayland_context_wake(runtime->wayland);
    return 0;
}

static void input_pointer_finish(Runtime *runtime)
{
    if(!runtime->input_pointer_ready)return;
    interactive_move_finish(runtime,"pointer-finish");
    interactive_resize_finish(runtime,TRUE,"pointer-finish");
    if(runtime->seat){
        wlr_seat_pointer_notify_clear_focus(runtime->seat);
        wlr_seat_set_capabilities(runtime->seat,
            runtime->seat->capabilities&~WL_SEAT_CAPABILITY_POINTER);
    }
    runtime->input_pointer_ready=FALSE;
    runtime->input_pointer_window=NULL;
    runtime->server_close_window=NULL;
    runtime->input_pointer_position_valid=FALSE;
    printf("RUNTIME_NATIVE_POINTER_CLOSED received=%llu dispatched=%llu unsupported=%llu no_target=%llu\n",
        (unsigned long long)runtime->input_pointer_received,
        (unsigned long long)runtime->input_pointer_sent,
        (unsigned long long)runtime->input_pointer_unsupported,
        (unsigned long long)runtime->input_pointer_no_target);
}

static void input_pointer_forget_window(Runtime *runtime,RuntimeWindow *window)
{
    if(runtime->interactive_move_window==window)
        interactive_move_finish(runtime,"window-retire");
    if(runtime->interactive_resize_window==window)
        interactive_resize_finish(runtime,FALSE,"window-retire");
    if(runtime->server_close_window==window)runtime->server_close_window=NULL;
    if(runtime->input_pointer_window!=window)return;
    runtime->input_pointer_window=NULL;
    if(runtime->seat && runtime->input_pointer_ready)
        wlr_seat_pointer_notify_clear_focus(runtime->seat);
}

static void input_pointer_received(Runtime *runtime,const AncoNativeInputPointer *pointer)
{
    ++runtime->input_pointer_received;
    const gint32 source=pointer->fields_38_to_48[1];
    const gint32 action=pointer->fields_38_to_48[2];
    const gint32 host_window=pointer->fields_10_to_28[5];
    const gint32 display=pointer->fields_10_to_28[4];
    if(!runtime->input_pointer_ready || !runtime->input_session_open ||
        !pointer->state.container || source!=0x2002 || pointer->item_count!=1 ||
        display!=(gint32)runtime->display_info.display.id){
        ++runtime->input_pointer_unsupported;
        printf("RUNTIME_NATIVE_POINTER_UNSUPPORTED event=%d source=%d action=%d display=%d items=%u\n",
            pointer->fields_10_to_28[1],source,action,display,pointer->item_count);
        return;
    }
    const AncoNativeInputPointerItem *item=&pointer->items[0];
    const double display_x=item->field_10,display_y=item->field_14;
    const uint32_t time_msec=(uint32_t)((uint64_t)pointer->field_00/1000);
    runtime->input_pointer_position_valid=TRUE;
    runtime->input_pointer_display_x=display_x;
    runtime->input_pointer_display_y=display_y;
    if(action==10 || action==3){
        interactive_move_finish(runtime,"pointer-cancel");
        interactive_resize_finish(runtime,TRUE,"pointer-cancel");
        runtime->server_close_window=NULL;
        if(runtime->seat->pointer_state.button_count==0){
            wlr_seat_pointer_notify_clear_focus(runtime->seat);
            runtime->input_pointer_window=NULL;
            wlr_seat_pointer_notify_frame(runtime->seat);
        }
        ++runtime->input_pointer_sent;
        return;
    }
    if(runtime->interactive_move_window &&
        (action==2 || action==7 || action==9)){
        const gboolean moved=interactive_move_update(runtime,display_x,display_y);
        ++runtime->input_pointer_sent;
        printf("RUNTIME_STANDARD_MOVE_POINTER event=%d action=%d window=%d display_xy=%.0f,%.0f layout_updated=%d client_motion_suppressed=1\n",
            pointer->fields_10_to_28[1],action,
            runtime->interactive_move_window?
                runtime->interactive_move_window->identity.window_id:-1,
            display_x,display_y,moved);
        if(runtime->wayland)anco_wayland_context_wake(runtime->wayland);
        return;
    }
    if(runtime->interactive_resize_window &&
        (action==2 || action==7 || action==9)){
        const gboolean configured=interactive_resize_update(runtime,display_x,display_y);
        ++runtime->input_pointer_sent;
        printf("RUNTIME_STANDARD_RESIZE_POINTER event=%d action=%d window=%d display_xy=%.0f,%.0f configure_scheduled=%d client_motion_suppressed=1 old_buffer_unchanged=1\n",
            pointer->fields_10_to_28[1],action,
            runtime->interactive_resize_window?
                runtime->interactive_resize_window->identity.window_id:-1,
            display_x,display_y,configured);
        if(runtime->wayland)anco_wayland_context_wake(runtime->wayland);
        return;
    }
    if(runtime->interactive_move_window && action==12){
        const int button=input_pointer_button(pointer->fields_38_to_48[3]);
        const gint32 window_id=runtime->interactive_move_window->identity.window_id;
        const gboolean server_owned=runtime->interactive_move_server_owned;
        if(button<0){
            ++runtime->input_pointer_unsupported;
            interactive_move_finish(runtime,"unsupported-release");
            return;
        }
        if(!server_owned){
            wlr_seat_pointer_notify_button(runtime->seat,time_msec,(uint32_t)button,
                WL_POINTER_BUTTON_STATE_RELEASED);
            wlr_seat_pointer_notify_frame(runtime->seat);
        }
        interactive_move_finish(runtime,"button-release");
        ++runtime->input_pointer_sent;
        printf("RUNTIME_STANDARD_MOVE_RELEASE event=%d window=%d button=%d client_release_sent=%d server_decoration=%d\n",
            pointer->fields_10_to_28[1],window_id,button,!server_owned,server_owned);
        if(runtime->wayland)anco_wayland_context_wake(runtime->wayland);
        return;
    }
    if(runtime->interactive_resize_window && action==12){
        const int button=input_pointer_button(pointer->fields_38_to_48[3]);
        const gint32 window_id=runtime->interactive_resize_window->identity.window_id;
        const gboolean server_owned=runtime->interactive_resize_server_owned;
        if(button<0){
            ++runtime->input_pointer_unsupported;
            interactive_resize_finish(runtime,TRUE,"unsupported-release");
            return;
        }
        if(!server_owned){
            wlr_seat_pointer_notify_button(runtime->seat,time_msec,(uint32_t)button,
                WL_POINTER_BUTTON_STATE_RELEASED);
            wlr_seat_pointer_notify_frame(runtime->seat);
        }
        interactive_resize_finish(runtime,TRUE,"button-release");
        ++runtime->input_pointer_sent;
        printf("RUNTIME_STANDARD_RESIZE_RELEASE event=%d window=%d button=%d client_release_sent=%d server_decoration=%d\n",
            pointer->fields_10_to_28[1],window_id,button,!server_owned,server_owned);
        if(runtime->wayland)anco_wayland_context_wake(runtime->wayland);
        return;
    }
    const gboolean scroll_action=action==8;
    /* Original TransformPointer stores Android MotionEvent AXIS_VSCROLL(9)
     * and AXIS_HSCROLL(10) in these fields. Android vertical positive means
     * viewport-up, while Wayland's positive vector is down; horizontal
     * positive is right in both. A wheel detent is Android magnitude1 and
     * the standard wlroots/libinput pair is 15 surface units/value120=120. */
    const double scroll_vertical=-(double)pointer->field_30;
    const double scroll_horizontal=(double)pointer->field_34;
    if(scroll_action && (!isfinite(scroll_vertical)||!isfinite(scroll_horizontal)||
        fabs(scroll_vertical)>INT32_MAX/(double)WLR_POINTER_AXIS_DISCRETE_STEP||
        fabs(scroll_horizontal)>INT32_MAX/(double)WLR_POINTER_AXIS_DISCRETE_STEP||
        (!scroll_vertical&&!scroll_horizontal))){
        ++runtime->input_pointer_unsupported;
        printf("RUNTIME_NATIVE_POINTER_SCROLL_INVALID event=%d android_vh=%.9g,%.9g\n",
            pointer->fields_10_to_28[1],pointer->field_30,pointer->field_34);
        return;
    }
    if(action!=2 && action!=7 && action!=8 && action!=9 && action!=11 && action!=12){
        ++runtime->input_pointer_unsupported;
        printf("RUNTIME_NATIVE_POINTER_ACTION_UNSUPPORTED event=%d action=%d\n",
            pointer->fields_10_to_28[1],action);
        return;
    }
    RuntimePointerHit hit={0};
    gboolean have_hit=FALSE;
    RuntimeTask *task=input_pointer_task(runtime,host_window);
    RuntimeWindow *system_window=input_pointer_system_window(runtime,host_window);
    const gboolean implicit_grab=runtime->seat->pointer_state.button_count>0 &&
        runtime->input_pointer_window && runtime->seat->pointer_state.focused_surface;
    if(implicit_grab){
        RuntimeWindow *window=runtime->input_pointer_window;
        double root_x=0,root_y=0;
        RuntimeTask *grab_task=live_task_state(runtime,window->identity.owner.task_id);
        const gboolean coordinates=window->native_system_window?
            input_pointer_system_coordinates(window,display_x,display_y,&root_x,&root_y):
            (grab_task&&input_pointer_root_coordinates(window,grab_task,display_x,display_y,
                &root_x,&root_y));
        if(coordinates){
            /* The initial hit removed the compositor-drawn decoration before
             * establishing this Wayland grab.  Keep later motion/release in
             * that same client-surface coordinate space. */
            if(window->native_system_window &&
               rect_has_margins(&window->submitted_decoration)){
                root_x-=window->submitted_decoration.left;
                root_y-=window->submitted_decoration.top;
            }
            hit.window=window;
            hit.surface=runtime->seat->pointer_state.focused_surface;
            hit.surface_x=root_x-runtime->input_pointer_surface_origin_x;
            hit.surface_y=root_y-runtime->input_pointer_surface_origin_y;
            have_hit=TRUE;
        }
    } else if(system_window)have_hit=input_pointer_hit(runtime,NULL,system_window,
        display_x,display_y,&hit);
    else if(task)have_hit=input_pointer_hit(runtime,task,NULL,display_x,display_y,&hit);
    if(action==12 && runtime->server_close_window){
        RuntimeWindow *closing=runtime->server_close_window;
        runtime->server_close_window=NULL;
        const int button=input_pointer_button(pointer->fields_38_to_48[3]);
        const gboolean activate=button==BTN_LEFT && have_hit && hit.server_decoration &&
            hit.close_button && hit.window==closing && !closing->retiring;
        if(activate)runtime_window_send_close(closing);
        wlr_seat_pointer_notify_clear_focus(runtime->seat);
        wlr_seat_pointer_notify_frame(runtime->seat);
        ++runtime->input_pointer_sent;
        printf("RUNTIME_SERVER_DECORATION_CLOSE window=%d activate=%d standard_xdg_close=%d\n",
            closing->identity.window_id,activate,activate);
        if(runtime->wayland)anco_wayland_context_wake(runtime->wayland);
        return;
    }
    if(have_hit && hit.server_decoration){
        RuntimeWindow *keyboard_target=hit.window;
        while(keyboard_target && !runtime_window_is_toplevel(keyboard_target))
            keyboard_target=direct_parent_window(keyboard_target);
        if(keyboard_target && window_receives_keys(keyboard_target))
            apply_window_focus(runtime,keyboard_target,"pointer-native-target");
        runtime->input_pointer_window=hit.window;
        wlr_seat_pointer_notify_clear_focus(runtime->seat);
        gboolean started=FALSE;
        if(action==11 && input_pointer_button(pointer->fields_38_to_48[3])==BTN_LEFT){
            if(hit.close_button){runtime->server_close_window=hit.window;started=TRUE;}
            else if(hit.resize_edges)started=server_decoration_begin_resize(hit.window,
                hit.resize_edges);
            else started=server_decoration_begin_move(hit.window);
        }
        wlr_seat_pointer_notify_frame(runtime->seat);
        ++runtime->input_pointer_sent;
        printf("RUNTIME_SERVER_DECORATION_POINTER event=%d action=%d window=%d close=%d edges=%u operation_started=%d client_pointer_suppressed=1\n",
            pointer->fields_10_to_28[1],action,hit.window->identity.window_id,
            hit.close_button,hit.resize_edges,started);
        if(runtime->wayland)anco_wayland_context_wake(runtime->wayland);
        return;
    }
    if(have_hit){
        /* Click-to-focus is the standard compositor contract. The original
         * callback already names the exact native target in host_window; make
         * its corresponding X11/Wayland window active before delivering the
         * button. This is required for a transient sharing its parent's
         * Task/Activity because an X11 client can otherwise move focus between
         * same-process windows behind the compositor's back. */
        if(action==11 && hit.window){
            RuntimeWindow *keyboard_target=hit.window;
            while(keyboard_target && !runtime_window_is_toplevel(keyboard_target))
                keyboard_target=direct_parent_window(keyboard_target);
            if(keyboard_target && window_receives_keys(keyboard_target))
                apply_window_focus(runtime,keyboard_target,"pointer-native-target");
            else recompute_window_focus(runtime);
        }
        if(!implicit_grab){
            wlr_seat_pointer_notify_enter(runtime->seat,hit.surface,hit.surface_x,hit.surface_y);
            runtime->input_pointer_window=hit.window;
            runtime->input_pointer_surface_origin_x=hit.surface_origin_x;
            runtime->input_pointer_surface_origin_y=hit.surface_origin_y;
        }
        wlr_seat_pointer_notify_motion(runtime->seat,time_msec,hit.surface_x,hit.surface_y);
    } else {
        ++runtime->input_pointer_no_target;
        wlr_seat_pointer_notify_clear_focus(runtime->seat);
        runtime->input_pointer_window=NULL;
    }
    if(scroll_action){
        gint32 vertical120=0,horizontal120=0;
        if(scroll_vertical)vertical120=(gint32)lround(scroll_vertical*
            WLR_POINTER_AXIS_DISCRETE_STEP);
        if(scroll_horizontal)horizontal120=(gint32)lround(scroll_horizontal*
            WLR_POINTER_AXIS_DISCRETE_STEP);
        if(have_hit && scroll_vertical)
            wlr_seat_pointer_notify_axis(runtime->seat,time_msec,
                WL_POINTER_AXIS_VERTICAL_SCROLL,scroll_vertical*15.0,vertical120,
                WL_POINTER_AXIS_SOURCE_WHEEL,
                WL_POINTER_AXIS_RELATIVE_DIRECTION_IDENTICAL);
        if(have_hit && scroll_horizontal)
            wlr_seat_pointer_notify_axis(runtime->seat,time_msec,
                WL_POINTER_AXIS_HORIZONTAL_SCROLL,scroll_horizontal*15.0,horizontal120,
                WL_POINTER_AXIS_SOURCE_WHEEL,
                WL_POINTER_AXIS_RELATIVE_DIRECTION_IDENTICAL);
        wlr_seat_pointer_notify_frame(runtime->seat);
        ++runtime->input_pointer_sent;
        printf("RUNTIME_NATIVE_POINTER_SCROLL event=%d host_window=%d target=%d display_xy=%.0f,%.0f android_vh=%.9g,%.9g wayland_vh=%.9g,%.9g value120=%d,%d dispatched=%d standard_axis=1\n",
            pointer->fields_10_to_28[1],host_window,
            have_hit?hit.window->identity.window_id:-1,display_x,display_y,
            pointer->field_30,pointer->field_34,scroll_vertical*15.0,
            scroll_horizontal*15.0,vertical120,horizontal120,have_hit);
        if(runtime->wayland)anco_wayland_context_wake(runtime->wayland);
        return;
    }
    if(action==11 || action==12){
        const int button=input_pointer_button(pointer->fields_38_to_48[3]);
        if(button<0){
            ++runtime->input_pointer_unsupported;
            printf("RUNTIME_NATIVE_POINTER_BUTTON_UNSUPPORTED event=%d wire=%d action=%d\n",
                pointer->fields_10_to_28[1],pointer->fields_38_to_48[3],action);
            wlr_seat_pointer_notify_frame(runtime->seat);
            return;
        }
        wlr_seat_pointer_notify_button(runtime->seat,time_msec,(uint32_t)button,
            action==11?WL_POINTER_BUTTON_STATE_PRESSED:WL_POINTER_BUTTON_STATE_RELEASED);
    }
    wlr_seat_pointer_notify_frame(runtime->seat);
    ++runtime->input_pointer_sent;
    printf("RUNTIME_NATIVE_POINTER_DISPATCH event=%d action=%d host_window=%d display_xy=%.0f,%.0f target=%d surface=%p grab=%d ui_confirmed=0\n",
        pointer->fields_10_to_28[1],action,host_window,display_x,display_y,
        have_hit?hit.window->identity.window_id:-1,(void *)hit.surface,implicit_grab);
    if(runtime->wayland)anco_wayland_context_wake(runtime->wayland);
}

static void runtime_activity_created(AncoLinuxCatalog *catalog,
    const AncoLinuxLaunch *activity, void *data)
{
    Runtime *runtime = data;
    RuntimeTask *task = g_hash_table_lookup(runtime->tasks,
        GINT_TO_POINTER(activity->task_id));
    if (!task) {
        task = g_new0(RuntimeTask,1);
        task->id = activity->task_id;
        task->activities = g_array_new(FALSE,FALSE,sizeof(gint32));
        g_hash_table_insert(runtime->tasks,GINT_TO_POINTER(task->id),task);
        /* Actual NEW catalog Task, not a newly-created surface. A distinct
         * Activity in an existing Task does not reorder unrelated Tasks. */
        runtime->task_stack = g_list_append(runtime->task_stack,task);
    }
    g_array_append_val(task->activities,activity->activity_id);
    if(runtime->creating_window_carrier){
        anco_wms_catalog_activity_created(catalog,activity,runtime->wms);
        printf("RUNTIME_WINDOW_ACTIVITY_PUBLISHED task=%d activity=%d real_window_pending=1\n",
            activity->task_id,activity->activity_id);
    }else{
        printf("RUNTIME_LAUNCH_ACTIVITY_DEFERRED task=%d activity=%d host_window=0\n",
            activity->task_id,activity->activity_id);
    }
}

static void handle_launch_switch_ready(Runtime *runtime,RuntimeTask *task,
    const char *source)
{
    if(!runtime||!task||task->semantic_task||!task->retire_requested||
       !task->mission_associated||!task->client_area_valid)return;
    AncoLinuxLaunch launch;
    if(anco_linux_catalog_task(runtime->catalog,task->id,&launch))return;
    /* A repeated launcher click for an application which already owns a real
     * submitted root needs no starting window.  Retire the exec switch by the
     * same original Task chain and let the application's own activation/new-
     * window behavior stand. */
    for(GList *node=runtime->windows;node;node=node->next){
        RuntimeWindow *window=node->data;AncoLinuxLaunch owner;
        if(window->retiring||!window->submitted_geometry_valid||
           !runtime_window_is_toplevel(window)||window->native_system_window||
           direct_parent_window(window)||
           anco_linux_catalog_task(runtime->catalog,
               window->identity.owner.task_id,&owner)||
           strcmp(owner.app.bundle,launch.app.bundle))continue;
        schedule_semantic_task_retire(runtime);
        printf("RUNTIME_LAUNCH_SWITCH_RETIRE_READY task=%d mission=%d native_id=%d existing_real_window=%d source=%s\n",
            task->id,task->mission_id,task->client_native_id,
            window->identity.window_id,source?source:"");
        return;
    }
    const int status=ensure_starting_window(runtime,task);
    if(!status||status==-EAGAIN)return;
    /* Failure to decorate startup must never turn the transient exec switch
     * into a persistent white/ghost Task.  Preserve r108's proven cleanup. */
    schedule_semantic_task_retire(runtime);
    printf("RUNTIME_STARTING_FALLBACK_RETIRE task=%d mission=%d status=%d source=%s persistent_shell=0\n",
        task->id,task->mission_id,status,source?source:"");
}

static void runtime_mission_observed(AncoLinuxCatalog *catalog,gint32 guest_user,
    gint32 mission,gint32 task_id,gboolean associated,void *data)
{
    Runtime *runtime=data;
    if(runtime->catalog!=catalog || guest_user!=anco_linux_catalog_user(catalog))return;
    RuntimeTask *task=live_task_state(runtime,task_id);
    if(!task)return;
    task->mission_associated=associated;
    task->mission_id=associated?mission:-1;
    if(!associated){
        task->frontend_window_registered=FALSE;
        task->graphic_frontend_ready=FALSE;
    }
    refresh_frontend_window_registration(runtime);
    printf("RUNTIME_ORIGINAL_MISSION task=%d mission=%d associated=%d pending_toplevel_gate=1\n",
        task_id,mission,associated);
    if(associated){
        /* The ordinary Home launcher is only an exec switch.  Its host
         * mission and ShellAssistant Rosen Window are created asynchronously
         * and in either order after the selected-Task reply.  Terminating the
         * mission before the original client-properties record is published
         * races a later ShellAssistant connect: SceneBoard accepts the early
         * termination, then the late frontend reconnects the same persistent
         * session and leaves a white launcher window.  Retire only after both
         * original identities exist.  Real Linux roots never reuse this Task
         * and continue to create their own semantic carriers. */
        if(!task->semantic_task && task->retire_requested){
            if(task->client_area_valid){
                handle_launch_switch_ready(runtime,task,"mission-associated");
            }else printf("RUNTIME_LAUNCH_SWITCH_RETIRE_WAIT_FRONTEND task=%d mission=%d original_mission=1 original_frontend=0\n",
                task_id,mission);
        }else{
            schedule_initial_commits_for_task(runtime,task);
            schedule_pending_toplevels(runtime,"mission-associated");
        }
    }
}

/* Actual selected new/reused Task, after catalog validation and before the
 * original start reply. A launch request initializes the Linux Task only until
 * the original host supplies its resolved mode. Host callbacks always prevail,
 * including a synchronous callback received during selection. No reverse mode
 * change, host admission success, focus event or frame readiness is invented. */
static void runtime_task_selected(AncoLinuxCatalog *catalog,
    const AncoAmsWant *want, const AncoLinuxLaunch *selected, void *data)
{
    Runtime *runtime=data;
    if(catalog!=runtime->catalog)return;
    RuntimeTask *task=live_task_state(runtime,selected->task_id);
    if(!task){runtime->failed=TRUE;return;}
    /* An ordinary launcher Task is the host menu's transient exec switch,
     * regardless of the broker session number assigned to this start.  Real
     * XDG/X11 roots are the semantic Tasks created independently by
     * create_window_carrier(); never adopt or reuse this launcher Task.
     * Arm retirement here, but wait for both runtime_mission_observed() and
     * the original ShellAssistant client-properties record.  The original
     * host creates and connects those objects asynchronously; retiring after
     * only the first one leaves a late-connected white shell. */
    if(!task->semantic_task){
        task->retire_requested=TRUE;
        printf("RUNTIME_LAUNCH_SWITCH_RETIRE_ARMED task=%d activity=%d session=%d wait_original_mission=1 wait_original_frontend=1 real_window_reuse=0\n",
            task->id,selected->activity_id,want->session_id);
    }
    if(!want->launch_window_mode_present)return;
    task->launch_mode_present=TRUE;
    task->launch_mode=want->launch_window_mode;
    if(!task->host_mode_known)task->mode=task->launch_mode;
    printf("RUNTIME_LAUNCH_MODE task=%d activity=%d requested=%d effective=%d host_mode_known=%d host_window_ready=0\n",
        task->id,selected->activity_id,task->launch_mode,task->mode,task->host_mode_known);
}

static gboolean task_has_created_root(Runtime *runtime,const RuntimeTask *task)
{
    for(GList *node=runtime->windows;node;node=node->next){
        const RuntimeWindow *window=node->data;
        if(!window->retiring && window->wms_create_entered &&
           window->identity.owner.task_id==task->id &&
           runtime_window_is_toplevel(window))return TRUE;
    }
    return FALSE;
}

static gboolean window_receives_keys(RuntimeWindow *window)
{
    /* Ordinary Linux application Window visibility/attachment. An admitted
     * xdg_toplevel owns its mapped original Task root; XDG popups use the
     * client's standard seat grab in their owning Activity.
     * Insets, pixels and pending native buffers cannot assert visibility. */
    return !window->retiring && window->wms_create_entered &&
        runtime_window_is_toplevel(window) &&
        runtime_window_surface(window)->mapped;
}

static gint32 top_visible_activity(Runtime *runtime, RuntimeTask *task)
{
    if (!live_task_state(runtime,task->id)) return 0;
    for (guint i=task->activities->len;i>0;--i) {
        const gint32 id = g_array_index(task->activities,gint32,i-1);
        AncoLinuxLaunch activity;
        if (anco_linux_catalog_activity(runtime->catalog,id,&activity) ||
            activity.task_id != task->id) continue;
        for (GList *node=g_list_last(runtime->windows);node;node=node->prev) {
            RuntimeWindow *window=node->data;
            if (window->identity.owner.task_id == task->id &&
                window->identity.owner.activity_id == id &&
                window_receives_keys(window)) return id;
        }
    }
    return 0;
}

#if WLR_HAS_XWAYLAND
static RuntimeWindow *top_modal_xwayland_window(Runtime *runtime,
    gint32 task,gint32 activity)
{
    /* XWM has already resolved WM_TRANSIENT_FOR and _NET_WM_STATE_MODAL into
     * parent/modal.  The original host can report only the owning application
     * Task while its TYPE_APPLICATION_ATTACHED_DIALOG has focus.  Preserve the
     * X11 modal focus owner in that case; list order is the compositor's
     * current window stacking/admission order, so the last live modal wins. */
    for(GList *node=g_list_last(runtime->windows);node;node=node->prev){
        RuntimeWindow *window=node->data;
        struct wlr_xwayland_surface *surface=window->xwayland_surface;
        if(!window->native_system_window || !surface || !surface->parent ||
           !surface->modal ||
           window->identity.owner.task_id!=task ||
           window->identity.owner.activity_id!=activity ||
           !window_receives_keys(window))continue;
        return window;
    }
    return NULL;
}
#endif

/* The original input service reports the exact focused native Window ID.
 * Resolve that one-to-one identity before falling back to Task/Activity stack
 * order: an Activity may own several real Linux windows, and its transient
 * system Window must not inherit the parent's X11/Wayland focus merely because
 * both windows share one Activity. */
static RuntimeWindow *window_for_native_focus(Runtime *runtime,gint32 native)
{
    if(native<=0)return NULL;
    RuntimeWindow *system=input_pointer_system_window(runtime,native);
    if(system && window_receives_keys(system))return system;
    RuntimeTask *task=input_pointer_task(runtime,native);
    if(!task)return NULL;
    const gint32 activity=top_visible_activity(runtime,task);
    if(!activity)return NULL;
#if WLR_HAS_XWAYLAND
    RuntimeWindow *modal=top_modal_xwayland_window(runtime,task->id,activity);
    if(modal)return modal;
#endif
    for(GList *node=g_list_last(runtime->windows);node;node=node->prev){
        RuntimeWindow *window=node->data;
        if(window->native_system_window ||
           window->identity.owner.task_id!=task->id ||
           window->identity.owner.activity_id!=activity ||
           !window_receives_keys(window))continue;
        return window;
    }
    return NULL;
}

static void apply_window_focus(Runtime *runtime,RuntimeWindow *selected,
    const char *source)
{
    /* Actual per-Task/per-Activity Window tree. All currently exported normal
     * type2 Windows share the original Activity comparator's equal-type
     * ordering (new normal Window above the old one). An unrelated Task's
     * later wl_surface creation cannot replace the foreground Task. */
    for (GList *node=runtime->windows;node;node=node->next) {
        RuntimeWindow *window=node->data;
        if (window->retiring || !runtime_window_is_toplevel(window) ||
            !runtime_window_surface(window)) continue;
        const gboolean active=window == selected;
        if (window->xdg && window->xdg->toplevel->scheduled.activated == active)
            continue;
        runtime_window_set_activated(window,active);
        printf("RUNTIME_WINDOW_ACTIVATION window=%d task=%d activity=%d active=%d protocol=%s\n",
            window->identity.window_id,window->identity.owner.task_id,
            window->identity.owner.activity_id,active,
            runtime_window_is_xwayland(window)?"x11":"xdg");
        if (runtime->wayland) anco_wayland_context_wake(runtime->wayland);
    }
    RuntimeWindow *seat_selected=selected;
    /* Popup grabs and unmap destroy the wlroots keyboard focus resource while
     * the owning AnCo Task/Activity can remain the same RuntimeWindow.  The
     * original focused-window identity therefore is not sufficient to skip
     * the Wayland enter: only skip when that exact xdg surface still owns the
     * seat focus. */
    if (seat_selected == runtime->focused_window &&
        (!seat_selected || (runtime->seat &&
         runtime->seat->keyboard_state.focused_surface == runtime_window_surface(seat_selected)))) {
        input_key_drain(runtime);return;
    }
    runtime->focused_window=seat_selected;
    if (runtime->seat) {
        if (seat_selected) {
            const struct wlr_keyboard *keyboard=wlr_seat_get_keyboard(runtime->seat);
            const struct wlr_keyboard_modifiers empty={0};
            /* Preserve standard popup grabs. Actual backend keycodes and
             * modifiers are supplied only after the original session opens. */
            wlr_seat_keyboard_notify_enter(runtime->seat,runtime_window_surface(seat_selected),
                keyboard ? keyboard->keycodes : NULL,keyboard ? keyboard->num_keycodes : 0,
                keyboard ? &keyboard->modifiers : &empty);
            if(runtime->text_seat){
                TextSeatBinding binding={runtime->text_seat};
                wl_client_for_each_resource(
                    wl_resource_get_client(runtime_window_surface(seat_selected)->resource),
                    bind_text_seat_resource,&binding);
                const int text_focus=anco_text_seat_focus(runtime->text_seat,
                    runtime_window_surface(seat_selected)->resource);
                printf("RUNTIME_TEXT_FOCUS window=%d status=%d real_seat_bindings=1\n",
                    seat_selected->identity.window_id,text_focus);
            }
        } else {
            /* Original authoritative container/window focus loss. A client's
             * popup grab must not keep a Linux keyboard destination after
             * the host has removed focus; this sends no input or host focus. */
            wlr_seat_keyboard_clear_focus(runtime->seat);
            if(runtime->text_seat)(void)anco_text_seat_focus(runtime->text_seat,NULL);
        }
    }
    printf("RUNTIME_WINDOW_FOCUS window=%d task=%d activity=%d container=%d seat_capabilities=%u source=%s\n",
        seat_selected ? seat_selected->identity.window_id : -1,
        seat_selected ? seat_selected->identity.owner.task_id : -1,
        seat_selected ? seat_selected->identity.owner.activity_id : -1,
        runtime->container_focused,runtime->seat ? runtime->seat->capabilities : 0,
        source?source:"unspecified");
    input_key_drain(runtime);
}

static void recompute_window_focus(Runtime *runtime)
{
    RuntimeWindow *selected = NULL;
    if (!runtime->stopping && runtime->container_focused) {
        selected=window_for_native_focus(runtime,runtime->input_state.host_focus);
        for (GList *t=g_list_last(runtime->task_stack);t && !selected;t=t->prev) {
            RuntimeTask *task=t->data;
            /* Modes whose split/middle/PC focus policies are not connected
             * stay unavailable. Never silently treat them as ordinary mode1.
             * mode is the actual host callback, not a desktop/title guess. */
            if (task->mode != 1 && task->mode != 102) continue;
            const gint32 activity=top_visible_activity(runtime,task);
            if (!activity) continue;
            if (task->mode == 102 && runtime->focused_activity &&
                runtime->focused_activity != activity) continue;
            for (GList *w=g_list_last(runtime->windows);w;w=w->prev) {
                RuntimeWindow *window=w->data;
                if (window->identity.owner.task_id == task->id &&
                    window->identity.owner.activity_id == activity &&
                    window_receives_keys(window)) { selected=window; break; }
            }
        }
    }
    apply_window_focus(runtime,selected,"original-focus-state");
}

/* Standard Linux decoration negotiation. The original native Task owns the
 * root caption. A SERVER-decorated transient is framed by this compositor in
 * that transient's existing original AnCo Window and buffer queue; negotiation
 * never creates a separate host titlebar API or merges it into the parent. */
typedef struct {
    Runtime *runtime;
    struct wlr_xdg_toplevel_decoration_v1 *decoration;
    struct wl_listener request_mode, destroy;
} RuntimeDecoration;

static void update_native_decoration(RuntimeDecoration *binding)
{
    struct wlr_xdg_toplevel_decoration_v1 *decoration=binding->decoration;
    struct wlr_xdg_surface *xdg=decoration->toplevel->base;
    /* First configure belongs to the actual initial surface commit. The
     * library retains requested_mode until then; no early configure/ack. */
    if(!xdg->initialized)return;
    RuntimeWindow *owner=NULL;
    for(GList *node=binding->runtime->windows;node;node=node->next){
        RuntimeWindow *candidate=node->data;
        if(!candidate->retiring && candidate->xdg==xdg){owner=candidate;break;}
    }
    const RuntimeTask *task=owner?live_task_state(binding->runtime,
        owner->identity.owner.task_id):NULL;
    const gboolean managed=task && task->host_mode_known &&
        (task->mode==1 || task->mode==102);
    enum wlr_xdg_toplevel_decoration_v1_mode mode;
    if(decoration->requested_mode==
       WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE)
        mode=WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE;
    else if(decoration->requested_mode==
            WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE)
        mode=WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE;
    else mode=managed?WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE:
        WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE;
    if(decoration->scheduled_mode==mode)return;
    const guint32 serial=wlr_xdg_toplevel_decoration_v1_set_mode(decoration,mode);
    printf("RUNTIME_STANDARD_DECORATION window=%d task=%d requested=%u mode=%d serial=%u native_task_managed=%d ui_confirmed=0\n",
        owner?owner->identity.window_id:-1,task?task->id:-1,
        (guint)decoration->requested_mode,mode,serial,managed);
    if(owner && root_native_title_owner(owner) &&
       !publish_native_metadata(owner)){
        if(owner->submitted_geometry_valid)
            update_original_window_layout_force(owner,
                &owner->submitted_geometry,TRUE);
        else if(owner->pending_buffer)
            update_original_window_layout_force(owner,
                &owner->pending_geometry,TRUE);
    }
    if(binding->runtime->wayland)anco_wayland_context_wake(binding->runtime->wayland);
}
static void decoration_requested(struct wl_listener *listener,void *data)
{
    (void)data;
    RuntimeDecoration *binding=wl_container_of(listener,binding,request_mode);
    update_native_decoration(binding);
}
static void decoration_destroyed(struct wl_listener *listener,void *data)
{
    (void)data;
    RuntimeDecoration *binding=wl_container_of(listener,binding,destroy);
    wl_list_remove(&binding->request_mode.link);
    wl_list_remove(&binding->destroy.link);
    binding->decoration->data=NULL;
    g_free(binding);
}
static void new_decoration(struct wl_listener *listener,void *data)
{
    Runtime *runtime=wl_container_of(listener,runtime,new_decoration);
    struct wlr_xdg_toplevel_decoration_v1 *decoration=data;
    RuntimeDecoration *binding=g_new0(RuntimeDecoration,1);
    binding->runtime=runtime;binding->decoration=decoration;
    decoration->data=binding;
    binding->request_mode.notify=decoration_requested;
    binding->destroy.notify=decoration_destroyed;
    wl_signal_add(&decoration->events.request_mode,&binding->request_mode);
    wl_signal_add(&decoration->events.destroy,&binding->destroy);
    update_native_decoration(binding);
}
static void refresh_native_decorations(Runtime *runtime)
{
    if(!runtime->decoration_manager)return;
    struct wlr_xdg_toplevel_decoration_v1 *decoration;
    wl_list_for_each(decoration,&runtime->decoration_manager->decorations,link){
        if(decoration->data)update_native_decoration(decoration->data);
    }
}

/* Standard wlroots keyboard backend for actual original AnCo callbacks.
 * No /dev/input access, simulated keys, custom wl_keyboard or host ID target. */
static const struct wlr_keyboard_impl input_keyboard_impl={.name="AnCo original callback"};

static void input_key_notify(struct wl_listener *listener,void *data)
{
    Runtime *runtime=wl_container_of(listener,runtime,input_key);
    struct wlr_keyboard_key_event *event=data;
    if(!runtime->input_key_dispatch)return;
    wlr_seat_keyboard_notify_key(runtime->seat,event->time_msec,event->keycode,event->state);
    ++runtime->input_key_sent;
}
static void input_modifiers_notify(struct wl_listener *listener,void *data)
{
    (void)data;
    Runtime *runtime=wl_container_of(listener,runtime,input_modifiers);
    if(runtime->input_key_dispatch)
        wlr_seat_keyboard_notify_modifiers(runtime->seat,&runtime->input_keyboard.modifiers);
}
static int input_keyboard_start(Runtime *runtime)
{
    if(runtime->input_keyboard_ready||!runtime->input_session_open||!runtime->seat)return 0;
    wlr_keyboard_init(&runtime->input_keyboard,&input_keyboard_impl,"AnCo original keyboard callback");
    struct xkb_context *context=xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    const struct xkb_rule_names names={.rules="evdev",.model="pc105",.layout="us"};
    struct xkb_keymap *map=context?xkb_keymap_new_from_names(context,&names,XKB_KEYMAP_COMPILE_NO_FLAGS):NULL;
    const gboolean installed=map&&wlr_keyboard_set_keymap(&runtime->input_keyboard,map);
    xkb_keymap_unref(map);xkb_context_unref(context);
    if(!installed){wlr_keyboard_finish(&runtime->input_keyboard);return -EIO;}
    runtime->input_key.notify=input_key_notify;
    runtime->input_modifiers.notify=input_modifiers_notify;
    wl_signal_add(&runtime->input_keyboard.events.key,&runtime->input_key);
    wl_signal_add(&runtime->input_keyboard.events.modifiers,&runtime->input_modifiers);
    runtime->input_keyboard_ready=TRUE;
    wlr_seat_set_keyboard(runtime->seat,&runtime->input_keyboard);
    wlr_seat_set_capabilities(runtime->seat,runtime->seat->capabilities|WL_SEAT_CAPABILITY_KEYBOARD);
    printf("RUNTIME_NATIVE_KEYBOARD_CONNECTED original_session=1 keymap_bytes=%zu capabilities=%u pointer=0 ime=0\n",
        runtime->input_keyboard.keymap_size,runtime->seat->capabilities);
    if(runtime->wayland)anco_wayland_context_wake(runtime->wayland);
    return 0;
}
static xkb_mod_mask_t input_mod_mask(const struct wlr_keyboard *keyboard,unsigned modifier)
{
    /* WLR_MODIFIER_* are single-bit masks; mod_indexes is ordered by bit. */
    if(!modifier||(modifier&(modifier-1)))return 0;
    unsigned index=0;
    while((modifier>>index)!=1)++index;
    if(index>=WLR_MODIFIER_COUNT)return 0;
    const xkb_mod_index_t value=keyboard->mod_indexes[index];
    return value!=XKB_MOD_INVALID&&value<32?((xkb_mod_mask_t)1<<value):0;
}
static void input_key_dispatch_wayland(Runtime *runtime,const AncoNativeInputKey *key)
{
    const int evdev=anco_linux_keycode(key->android_key);
    if(evdev<0){
        ++runtime->input_key_unmapped;
        printf("RUNTIME_NATIVE_KEY_UNMAPPED event=%d android=%d\n",key->event_id,key->android_key);
        return;
    }
    struct wlr_keyboard *keyboard=&runtime->input_keyboard;
    const uint32_t meta=key->android_meta;
    xkb_mod_mask_t depressed=0,locked=0;
    if(meta&0xc1U)depressed|=input_mod_mask(keyboard,WLR_MODIFIER_SHIFT);
    if(meta&0x32U)depressed|=input_mod_mask(keyboard,WLR_MODIFIER_ALT);
    if(meta&0x7000U)depressed|=input_mod_mask(keyboard,WLR_MODIFIER_CTRL);
    if(meta&0x70000U)depressed|=input_mod_mask(keyboard,WLR_MODIFIER_LOGO);
    if(meta&0x100000U)locked|=input_mod_mask(keyboard,WLR_MODIFIER_CAPS);
    if(meta&0x200000U)locked|=input_mod_mask(keyboard,WLR_MODIFIER_MOD2);
    runtime->input_key_dispatch=TRUE;
    wlr_keyboard_notify_modifiers(keyboard,depressed,0,locked,0);
    const guint64 sent_before=runtime->input_key_sent;
    struct wlr_keyboard_key_event event={
        .time_msec=(uint32_t)((uint64_t)key->action_time_us/1000),
        .keycode=(uint32_t)evdev,.update_state=FALSE,
        .state=key->android_action==0?WL_KEYBOARD_KEY_STATE_PRESSED:
            WL_KEYBOARD_KEY_STATE_RELEASED};
    wlr_keyboard_notify_key(keyboard,&event);
    runtime->input_key_dispatch=FALSE;
    printf("RUNTIME_NATIVE_KEY_DISPATCH event=%d device=%d android=%d evdev=%d action=%d meta=%u seat_notify=%d ui_confirmed=0\n",
        key->event_id,key->device_id,key->android_key,evdev,key->android_action,meta,
        runtime->input_key_sent!=sent_before);
    if(runtime->wayland)anco_wayland_context_wake(runtime->wayland);
}
static void input_imf_key_done(void *data,const AncoNativeInputKey *key,
    bool consumed,bool stale)
{
    Runtime *runtime=data;
    if(stale){++runtime->input_key_imf_stale;return;}
    if(consumed){++runtime->input_key_imf_consumed;return;}
    if(!runtime->stopping&&runtime->input_keyboard_ready&&runtime->focused_window)
        input_key_dispatch_wayland(runtime,key);
}
static void input_key_drain(Runtime *runtime)
{
    /* Original dispatchKeyLocked63f08 first checks IsFocusInContainer. False
     * leaves its pending event queued; target selection uses the actual
     * focused guest Window. Container-focus loss leaves events queued; a
     * focused container with no focused guest Window drops the event as the
     * original findFocusedWindowTargetsLocked 'No focused window' branch.
     * Original device ID/source are not Linux evdev identifiers. */
    if(!runtime->input_keyboard_ready||!runtime->input_state.container)return;
    while(!g_queue_is_empty(&runtime->input_pending_keys)){
        AncoNativeInputKey *key=g_queue_pop_head(&runtime->input_pending_keys);
        if(!runtime->container_focused||!runtime->focused_window||
           !window_receives_keys(runtime->focused_window)||!runtime->seat->keyboard_state.focused_surface){
            ++runtime->input_key_no_focus;
            printf("RUNTIME_NATIVE_KEY_NO_FOCUS event=%d android=%d dropped=1\n",key->event_id,key->android_key);
            g_free(key);continue;
        }
        if(anco_linux_imf_dispatch_key(runtime->imf,key)){
            ++runtime->input_key_imf_queued;
            printf("RUNTIME_NATIVE_KEY_IMF_QUEUED event=%d android=%d action=%d\n",
                key->event_id,key->android_key,key->android_action);
        }else input_key_dispatch_wayland(runtime,key);
        g_free(key);
    }
}
static void input_keyboard_finish(Runtime *runtime)
{
    if(runtime->input_keyboard_ready){
        runtime->input_key_dispatch=FALSE;
        wl_list_remove(&runtime->input_key.link);
        wl_list_remove(&runtime->input_modifiers.link);
        wlr_seat_keyboard_clear_focus(runtime->seat);
        wlr_seat_set_capabilities(runtime->seat,runtime->seat->capabilities&~WL_SEAT_CAPABILITY_KEYBOARD);
        wlr_seat_set_keyboard(runtime->seat,NULL);
        wlr_keyboard_finish(&runtime->input_keyboard);
        runtime->input_keyboard_ready=FALSE;
        printf("RUNTIME_NATIVE_KEYBOARD_CLOSED received=%llu seat_notifies=%llu unmapped=%llu no_focus=%llu pending=%u\n",
            (unsigned long long)runtime->input_key_received,(unsigned long long)runtime->input_key_sent,
            (unsigned long long)runtime->input_key_unmapped,
            (unsigned long long)runtime->input_key_no_focus,runtime->input_pending_keys.length);
    }
    g_queue_clear_full(&runtime->input_pending_keys,g_free);
}

/* A client can commit its natural first buffer and request a frame callback
 * before the original host finishes creating the native Task.  Keep that
 * buffer until it has entered the original queue: standard Wayland mapping
 * retains the last committed buffer while a later configure is outstanding.
 * Dropping the only pre-configure buffer exposes the newly created native
 * Task's blank background when a client keeps rendering against its last
 * acknowledged configure.  Once a real frame has been submitted, a newer
 * pending commit that belongs to a superseded configure may be retired; the
 * already submitted frame remains visible until the replacement arrives. */
static void retire_stale_pending_for_host_configure(RuntimeWindow *window,
    guint32 new_serial,gint32 width,gint32 height)
{
    if(!window->pending_buffer ||
       (window->pending_configure_serial==new_serial &&
        window->pending_surface_width==width &&
        window->pending_surface_height==height))return;
    if(!window->submitted_geometry_valid && window->layout_attempted){
        printf("RUNTIME_PENDING_FIRST_BUFFER_RETAINED window=%d commit=%u old_configure=%u new_configure=%u target=%dx%d standard_last_buffer=1\n",
            window->identity.window_id,window->pending_sequence,
            window->pending_configure_serial,new_serial,width,height);
        return;
    }
    const guint32 sequence=window->pending_sequence;
    const guint32 old_serial=window->pending_configure_serial;
    const gint32 old_width=window->pending_surface_width;
    const gint32 old_height=window->pending_surface_height;
    if(window->submit_idle){
        g_source_remove(window->submit_idle);
        window->submit_idle=0;
    }
    wlr_buffer_unlock(window->pending_buffer);
    window->pending_buffer=NULL;
    window->pending_layer_valid=FALSE;
    window->pending_configure_serial=0;
    window->pending_mapping_epoch=0;
    window->pending_surface_width=0;
    window->pending_surface_height=0;
    struct timespec now;clock_gettime(CLOCK_MONOTONIC,&now);
    wlr_surface_send_frame_done(runtime_window_surface(window),&now);
    if(window->runtime->wayland)anco_wayland_context_wake(window->runtime->wayland);
    printf("RUNTIME_PENDING_BUFFER_OBSOLETE window=%d commit=%u old_configure=%u old_logical=%dx%d new_configure=%u target=%dx%d original_queue_untouched=1 frame_done=1\n",
        window->identity.window_id,sequence,old_serial,old_width,old_height,
        new_serial,width,height);
}

/* Original Task bounds are guest physical coordinates, independent of
 * mHwStackScale. Native VPR converts only the Linux client's logical units.
 * The ordinary mode1/102, scale1 path requests ROOT content only: no
 * transient/popup size inheritance, no native layout change or old-buffer
 * stretch until the client acknowledges and commits its actual new content. */
static gboolean configure_task_roots(Runtime *runtime,RuntimeTask *task)
{
    task->client_area_deferred=FALSE;
    AncoWmsRect bounds;gint32 mode;float host_scale;
    if(!effective_task_geometry(task,&bounds,&mode,&host_scale))return FALSE;
    /* A new native AnCo root is created fullscreen and then Recover() applies
     * the requested freeform mode. Do not turn that host-only transition into
     * a Wayland fullscreen configure: the initial unconstrained XDG configure
     * must yield the client's natural root size, which the unchanged WMS14
     * relayout sends to Recover. Once the real created root reports mode102,
     * later mode1 callbacks are genuine user window-mode changes. */
    if(task->launch_mode_present && task->launch_mode==102 &&
       !task->launch_mode_resolved && mode==1){
        task->client_area_deferred=TRUE;
        printf("RUNTIME_LAUNCH_MODE_TRANSITION task=%d requested=102 host_mode=1 root_created=%d wayland_configure_deferred=1\n",
            task->id,task_has_created_root(runtime,task));
        return FALSE;
    }
    const double host_vpr=runtime->display_info.scale;
    gint64 native_width=(gint64)bounds.right-bounds.left;
    gint64 native_height=(gint64)bounds.bottom-bounds.top;
    if(runtime->client_properties_observer) {
        /* Task callback3 stays OUTER bounds. Native WindowProperties supplies
         * separate drawable size. Its offset is the original stage position
         * beneath the title, not a second root LP/layer translation. */
        if(!task->client_area_valid || task->client_mode!=mode ||
           (gint64)task->client_outer.right-task->client_outer.left!=native_width ||
           (gint64)task->client_outer.bottom-task->client_outer.top!=native_height) {
            task->client_area_deferred=TRUE;
            printf("RUNTIME_CLIENT_AREA_WAIT task=%d mode=%d source_valid=%d\n",
                task->id,mode,task->client_area_valid);
            return FALSE;
        }
        if(fabs(task->client_vpr-host_vpr)>1e-6)return FALSE;
        native_width=(gint64)task->client_area.right-task->client_area.left;
        native_height=(gint64)task->client_area.bottom-task->client_area.top;
    }
    for(GList *node=runtime->windows;node;node=node->next){
        RuntimeWindow *window=node->data;
        if(window->retiring || window->identity.owner.task_id!=task->id ||
            !runtime_window_is_toplevel(window) ||
            !runtime_window_surface(window) || window->native_system_window)continue;
        const double content_scale=runtime_window_content_scale(window);
        const double width=native_width/content_scale;
        const double height=native_height/content_scale;
        if(!isfinite(width)||!isfinite(height)||width<1||height<1 ||
           width>INT32_MAX||height>INT32_MAX)continue;
        const gint32 requested_w=(gint32)lround(width);
        const gint32 requested_h=(gint32)lround(height);
#if WLR_HAS_XWAYLAND
        if(window->xwayland_surface){
            const gboolean fullscreen=task->client_status==1;
            const gboolean maximized=task->client_status==2;
            const gboolean minimized=task->client_status==3;
            wlr_xwayland_surface_configure(window->xwayland_surface,
                window->xwayland_surface->x,window->xwayland_surface->y,
                (uint16_t)MIN(requested_w,UINT16_MAX),
                (uint16_t)MIN(requested_h,UINT16_MAX));
            wlr_xwayland_surface_set_fullscreen(window->xwayland_surface,
                fullscreen);
            wlr_xwayland_surface_set_maximized(window->xwayland_surface,maximized);
            wlr_xwayland_surface_set_minimized(window->xwayland_surface,minimized);
            retire_stale_pending_for_host_configure(window,0,requested_w,
                requested_h);
            printf("RUNTIME_TASK_BOUNDS_CONFIGURE task=%d window=%d native=%d,%d,%d,%d scale=%g logical=%dx%d serial=0 status=0 new_buffer_required=1 fullscreen=%d maximized=%d minimized=%d protocol=x11\n",
                task->id,window->identity.window_id,bounds.left,bounds.top,
                bounds.right,bounds.bottom,host_scale,requested_w,requested_h,
                fullscreen,maximized,minimized);
            if(runtime->wayland)anco_wayland_context_wake(runtime->wayland);
            continue;
        }
#endif
        struct wlr_xdg_toplevel *top=window->xdg->toplevel;
        const gboolean fullscreen=task->client_status==1;
        const gboolean maximized=task->client_status==2;
        const gboolean minimized=task->client_status==3;
        gint32 w=requested_w,h=requested_h;
        /* XDG min/max sizes are client-committed window-geometry constraints.
         * In ordinary freeform mode, keep the original Task/WMS14 relayout
         * path within those constraints instead of waiting forever for an
         * impossible exact-size buffer.  Fullscreen retains the host bounds;
         * the XDG fullscreen contract already defines compositor-side fill
         * when client content does not cover the output. */
        if(!fullscreen){
            const gint32 min_width=MAX(top->current.min_width,1);
            const gint32 min_height=MAX(top->current.min_height,1);
            const gint32 max_width=top->current.max_width?
                top->current.max_width:INT32_MAX;
            const gint32 max_height=top->current.max_height?
                top->current.max_height:INT32_MAX;
            w=CLAMP(w,min_width,max_width);
            h=CLAMP(h,min_height,max_height);
            if(w!=requested_w || h!=requested_h)
                printf("RUNTIME_TASK_BOUNDS_CONSTRAINED task=%d window=%d requested=%dx%d constrained=%dx%d min=%dx%d max=%dx%d original_wms14_relayout=1\n",
                    task->id,window->identity.window_id,requested_w,requested_h,
                    w,h,top->current.min_width,top->current.min_height,
                    top->current.max_width,top->current.max_height);
        }
        if(top->scheduled.width==w && top->scheduled.height==h &&
            top->scheduled.fullscreen==fullscreen &&
            top->scheduled.maximized==maximized &&
            top->scheduled.suspended==minimized)continue;
        guint32 serial=0;
        const int status=anco_wlroots_toplevel_size(runtime->ownership,window->xdg,
            &window->identity,w,h,&serial);
        if(!status){
            /* Original Task configuration mode1 is fullscreen, mode102 is
             * freeform. Preserve the actual host state through standard XDG
             * configure/ack/commit, not size-only or inferred maximization.
             * Upstream coalesces size and state into the same real serial. */
            const guint32 mode_serial=wlr_xdg_toplevel_set_fullscreen(top,fullscreen);
            const guint32 maximize_serial=wlr_xdg_toplevel_set_maximized(top,maximized);
            const guint32 suspend_serial=wlr_xdg_toplevel_set_suspended(top,minimized);
            if(mode_serial!=serial || maximize_serial!=serial || suspend_serial!=serial ||
               !window->xdg->configure_idle)return FALSE;
            retire_stale_pending_for_host_configure(window,serial,w,h);
        }
        printf("RUNTIME_TASK_BOUNDS_CONFIGURE task=%d window=%d native=%d,%d,%d,%d scale=%g logical=%dx%d serial=%u status=%d new_buffer_required=1 fullscreen=%d maximized=%d minimized=%d\n",
            task->id,window->identity.window_id,bounds.left,bounds.top,
            bounds.right,bounds.bottom,host_scale,w,h,serial,status,fullscreen,
            maximized,minimized);
        if(status)return FALSE;
        if(runtime->wayland)anco_wayland_context_wake(runtime->wayland);
    }
    return TRUE;
}

static RuntimeWindow *root_window_for_task(Runtime *runtime,gint32 task)
{
    RuntimeWindow *selected=NULL;
    for(GList *node=runtime->windows;node;node=node->next){
        RuntimeWindow *window=node->data;
        if(window->retiring || window->native_system_window ||
           window->identity.owner.task_id!=task ||
           !runtime_window_is_toplevel(window) ||
           direct_parent_window(window))continue;
        if(selected)return NULL;
        selected=window;
    }
    return selected;
}

static void dispatch_root_close_request(Runtime *runtime,RuntimeTask *task,
    guint64 serial)
{
    if(!task->close_request_serial_known){
        task->close_request_serial_known=TRUE;
        task->close_request_serial=serial;
        return;
    }
    if(serial<=task->close_request_serial){
        task->close_request_serial=serial;
        return;
    }
    task->close_request_serial=serial;
    RuntimeWindow *window=root_window_for_task(runtime,task->id);
    if(!window)return;
    runtime_window_send_close(window);
    printf("RUNTIME_ORIGINAL_WINDOW_CLOSE_REQUEST task=%d window=%d serial=%llu standard_xdg_close=1 task_retired=0\n",
        task->id,window->identity.window_id,(unsigned long long)serial);
    if(runtime->wayland)anco_wayland_context_wake(runtime->wayland);
}

static void bind_native_system_window(Runtime *runtime,
    const AncoClientProperties *p)
{
    RuntimeWindow *selected=NULL;
    for(GList *node=runtime->windows;node;node=node->next){
        RuntimeWindow *window=node->data;
        if(window->retiring || !window->native_system_window ||
           window->identity.window_id!=p->guest_window)continue;
        if(selected)return;
        selected=window;
    }
    if(!selected)return;
    const gboolean changed=!selected->native_system_id_valid ||
        selected->native_system_id!=p->native_id ||
        selected->native_frontend_pid!=p->frontend_pid ||
        selected->native_frontend_start_ticks!=p->frontend_start_ticks;
    selected->native_system_id=p->native_id;
    selected->native_system_id_valid=TRUE;
    selected->native_frontend_pid=p->frontend_pid;
    selected->native_frontend_start_ticks=p->frontend_start_ticks;
    if(changed)selected->native_close_request_serial_known=FALSE;
    if(changed)printf("RUNTIME_ORIGINAL_SYSTEM_WINDOW guest=%d native_id=%d exact_original_name=1\n",
        p->guest_window,p->native_id);
    if(!selected->native_close_request_serial_known){
        selected->native_close_request_serial_known=TRUE;
        selected->native_close_request_serial=p->close_request_serial;
    }else if(p->close_request_serial>selected->native_close_request_serial){
        selected->native_close_request_serial=p->close_request_serial;
        runtime_window_send_close(selected);
        printf("RUNTIME_ORIGINAL_SYSTEM_CLOSE_REQUEST guest=%d native_id=%d serial=%llu standard_xdg_close=1 parent_task_retired=0\n",
            p->guest_window,p->native_id,
            (unsigned long long)p->close_request_serial);
        if(runtime->wayland)anco_wayland_context_wake(runtime->wayland);
    }else selected->native_close_request_serial=p->close_request_serial;
}

static void client_properties_received(const AncoClientProperties *p,gpointer data)
{
    Runtime *runtime=data;
    if(runtime->stopping)return;
    /* The original frontend is a different UID and PID namespace, so the
     * unprivileged Linux runtime cannot re-open its /proc entry.  The observer
     * receives only strict, atomic records from the original-host directory
     * which the runner bind-mounts read-only into the guest.  The
     * frontendPid/startTicks/nativeId tuple is retained below as the exact
     * generation identity for updates and removal; do not apply guest-process
     * ownership checks to this original-host event source. */
    if(p->task==-1 && p->guest_window>0) {
        AncoClientProperties *saved=g_memdup2(p,sizeof(*p));
        saved->bundle=NULL;
        g_hash_table_replace(runtime->native_system_ids,
            GINT_TO_POINTER(p->guest_window),saved);
        bind_native_system_window(runtime,p);
        return;
    }
    RuntimeTask *task=live_task_state(runtime,p->task);AncoLinuxLaunch launch;
    if(!task || anco_linux_catalog_task(runtime->catalog,p->task,&launch) ||
       strcmp(launch.app.bundle,p->bundle))return;
    const gboolean frontend_changed=task->frontend_pid!=p->frontend_pid ||
        task->frontend_start_ticks!=p->frontend_start_ticks;
    task->frontend_pid=p->frontend_pid;
    task->frontend_start_ticks=p->frontend_start_ticks;
    if(frontend_changed){
        task->close_request_serial_known=FALSE;
        printf("RUNTIME_ORIGINAL_FRONTEND task=%d frontend_pid=%d native_id=%d original_window_properties=1\n",
            p->task,p->frontend_pid,p->native_id);
        for(GList *node=runtime->windows;node;node=node->next){
            RuntimeWindow *window=node->data;
            if(!window->retiring && window->identity.owner.task_id==p->task &&
               root_native_title_owner(window))(void)publish_native_metadata(window);
        }
    }
    dispatch_root_close_request(runtime,task,p->close_request_serial);
    const gboolean changed=!task->client_area_valid || task->client_native_id!=p->native_id ||
        task->client_mode!=p->mode || task->client_status!=p->status ||
        task->client_vpr!=p->vpr ||
        memcmp(&task->client_outer,&p->outer,sizeof(p->outer)) ||
        memcmp(&task->client_area,&p->drawable,sizeof(p->drawable));
    task->client_outer=p->outer;task->client_area=p->drawable;
    task->client_native_id=p->native_id;task->client_mode=p->mode;
    task->client_status=p->status;
    task->client_vpr=p->vpr;task->client_area_valid=TRUE;
    if(changed) {
        printf("RUNTIME_ORIGINAL_CLIENT_AREA task=%d native_id=%d mode=%d status=%d outer=%d,%d,%d,%d drawable=%d,%d,%d,%d vpr=%.9g task_bounds_unchanged=1\n",
            p->task,p->native_id,p->mode,p->status,p->outer.left,p->outer.top,p->outer.right,p->outer.bottom,
            p->drawable.left,p->drawable.top,p->drawable.right,p->drawable.bottom,p->vpr);
        if(!task_has_created_root(runtime,task)) {
            task->client_area_deferred=TRUE;
            printf("RUNTIME_CLIENT_AREA_DEFERRED task=%d reason=original-root-not-created original_wms_order=1\n",
                task->id);
        } else if(configure_task_roots(runtime,task))schedule_layer_scene(runtime);
    }
    /* A launcher switch has no Linux root to configure.  This record proves
     * that its original ShellAssistant Rosen Window has connected, so an
     * already-associated mission can now be retired without a later frontend
     * reconnect resurrecting the white shell.  Use the same existing WMS/AMS
     * retirement chain; no timer, synthetic acknowledgement or window reuse. */
    if(!task->semantic_task && task->retire_requested &&
       task->mission_associated){
        handle_launch_switch_ready(runtime,task,"client-properties");
    }
    /* This record is published by the original ShellAssistant Window after
     * its Task page has bound the real Rosen Window.  Resume on the serialized
     * main context; never reverse-call WMS from the property observer stack. */
    schedule_initial_commits_for_task(runtime,task);
}
static void client_properties_removed(gint32 frontend_pid,guint64 frontend_start_ticks,gint32 id,
    gint32 guest_window,gint32 native_id,gpointer data)
{
    Runtime *runtime=data;
    if(id==-1 && guest_window>0) {
        gpointer current=NULL;
        if(g_hash_table_lookup_extended(runtime->native_system_ids,
                GINT_TO_POINTER(guest_window),NULL,&current) &&
           ((AncoClientProperties *)current)->native_id==native_id &&
           ((AncoClientProperties *)current)->frontend_pid==frontend_pid &&
           ((AncoClientProperties *)current)->frontend_start_ticks==frontend_start_ticks)
            g_hash_table_remove(runtime->native_system_ids,
                GINT_TO_POINTER(guest_window));
        for(GList *node=runtime->windows;node;node=node->next){
            RuntimeWindow *window=node->data;
            if(window->native_system_window &&
               window->identity.window_id==guest_window &&
               window->native_system_id==native_id &&
               window->native_frontend_pid==frontend_pid &&
               window->native_frontend_start_ticks==frontend_start_ticks)
                window->native_system_id_valid=FALSE;
        }
        return;
    }
    RuntimeTask *task=live_task_state(runtime,id);
    if(task && task->frontend_pid==frontend_pid &&
       task->frontend_start_ticks==frontend_start_ticks &&
       task->client_native_id==native_id)task->client_area_valid=FALSE;
    // Metadata removal does not manufacture Wayland/Task destruction.
}
static int client_properties_start(Runtime *runtime)
{
    const char *dir=g_getenv("ANCO_NATIVE_WINDOW_PROPERTIES_DIR");
    const char *pid=g_getenv("ANCO_NATIVE_FRONTEND_PID");
    if(!dir && !pid)return 0;
    if(!dir || !pid || dir[0]!='/')return -EINVAL;
    char *end=NULL;errno=0;const long n=strtol(pid,&end,10);
    if(errno || !*pid || *end || n<=0 || n>G_MAXINT32)return -EINVAL;
    GError *error=NULL;
    runtime->client_properties_observer=anco_client_properties_observe(dir,0,
        client_properties_received,client_properties_removed,runtime,&error);
    if(!runtime->client_properties_observer) {
        printf("RUNTIME_CLIENT_AREA_MONITOR_FAILED error=%s\n",error?error->message:"missing");
        g_clear_error(&error);return -EIO;
    }
    printf("RUNTIME_CLIENT_AREA_MONITOR startup_frontend_pid=%ld all_original_frontends=1 directory=%s original_window_properties=1\n",n,dir);
    return 0;
}

static gboolean original_window_callback(const AncoWmsCallbackMessage *message,
    const AncoWmsCallbackHeader *header, void *data)
{
    (void)header; /* Original receiver has already checked actual Binder UID. */
    Runtime *runtime=data;
    switch (message->code) {
    case 1:
        runtime->container_has_focus=message->data.container_focus.focused;
        if (message->data.container_focus.display < 0 ||
            (guint64)message->data.container_focus.display != runtime->display_info.display.id)
            return TRUE; /* Original missing-display void return, not new admission. */
        runtime->container_focused=message->data.container_focus.focused;
        recompute_window_focus(runtime);
        return TRUE;
    case 2: {
        RuntimeTask *task=live_task_state(runtime,message->data.mode.task);
        if (!task) return TRUE; /* Original unknown Task has no side effects. */
        const gint32 mode=message->data.mode.mode;
        const gboolean root_created=task_has_created_root(runtime,task);
        gboolean launch_resolved_now=FALSE;
        /* SceneBoard's ordinary icon-launch path does not duplicate its
         * selected WindowMode in the AMS Want.  The first original WMS mode
         * callback is therefore the authoritative launch-mode source for a
         * Task which has not created a root yet.  Feed that real callback into
         * the same 102 -> construction-mode1 -> post-Recover102 state machine
         * used when an explicit Want mode is present; do not manufacture or
         * reverse-send a mode to the host. */
        if(!task->launch_mode_present && !root_created &&
           (mode==1 || mode==102)){
            task->launch_mode_present=TRUE;
            task->launch_mode=mode;
            printf("RUNTIME_LAUNCH_MODE_CALLBACK_SOURCE task=%d requested=%d effective=%d root_created=0 original_callback=1\n",
                task->id,task->launch_mode,mode);
        }
        if(task->launch_mode_present && mode==task->launch_mode){
            task->launch_mode_observed=TRUE;
            if(!task->launch_mode_resolved && root_created &&
               task->launch_construction_mode_observed){
                task->launch_mode_resolved=TRUE;
                launch_resolved_now=TRUE;
                printf("RUNTIME_LAUNCH_MODE_RESOLVED task=%d requested=%d host_mode=%d real_root=1 construction_mode_observed=1\n",
                    task->id,task->launch_mode,mode);
            }
        }
        if(task->host_mode_known && task->mode==mode && !launch_resolved_now){
            /* Original same-mode return has no traversal/front-order effects.
             * A processed ordinary-mode NOOP is a normal void return, not a
             * claim about full configuration or presentation completion. */
            const gboolean ordinary=mode==1 || mode==102;
            printf("RUNTIME_TASK_MODE_NOOP task=%d mode=%d ordinary_root_mode_consumed=%d full_configuration_consumer_ready=0\n",
                task->id,mode,ordinary);
            return ordinary;
        }
        /* The original freeform launch reports 102 from launch selection and
         * can then report the native ShellAssistant Window's construction
         * mode1 before or just after the guest Window enters WMS.  Preserve
         * the requested mode until the subsequent post-Recover 102 callback;
         * root existence alone cannot distinguish those two real orders. */
        if(task->launch_mode_present && task->launch_mode==102 &&
           task->launch_mode_observed && !task->launch_mode_resolved &&
           mode==1){
            task->launch_construction_mode_observed=TRUE;
            task->host_mode_known=TRUE;
            printf("RUNTIME_LAUNCH_MODE_CONSTRUCTION task=%d callback_mode=1 retained_mode=102 requested=102 root_created=%d original_callback_consumed=1\n",
                task->id,root_created);
            return TRUE;
        }
        task->mode=mode;task->host_mode_known=TRUE;
        /* A freeform launch may report the native Window's temporary
         * fullscreen construction state before Recover() reaches mode102.
         * Resume a root's already-received XDG initial commit only from the
         * actual requested host mode; no synthetic host transition is sent. */
        if(task->launch_mode_present && mode==task->launch_mode)
            schedule_initial_commits_for_task(runtime,task);
        refresh_native_decorations(runtime);
        if(mode==102){
            /* Original onWindowModeChanged: setWindowingMode -> moveToFront
             * -> resume the actual top visible Activity. This is the same
             * real Task ordering used by the original focus callback; no
             * host focus request or fabricated client activation is sent. */
            runtime->task_stack=g_list_remove(runtime->task_stack,task);
            runtime->task_stack=g_list_append(runtime->task_stack,task);
            runtime->focused_activity=top_visible_activity(runtime,task);
            schedule_layer_scene(runtime);
        }
        recompute_window_focus(runtime);
        /* Real Task mode and standard client state are already consumed. No
         * bounds yet is the original asynchronous mode-before-bounds case:
         * keep the actual mode for the subsequent real bounds/initial commit.
         * A failed/unsupported size/scale still reports unavailable, and this
         * finite consumer does not claim split/middle/transient configuration. */
        const gboolean ordinary=mode==1 || mode==102;
        const gboolean configured=ordinary &&
            (!task->host_bounds_valid || configure_task_roots(runtime,task));
        if(configured)schedule_layer_scene(runtime);
        if(mode==102)schedule_pending_toplevels(runtime,"task-mode-102");
        printf("RUNTIME_TASK_MODE_OBSERVED task=%d mode=%d original_front_order=%d ordinary_root_mode_consumed=%d bounds_present=%d full_configuration_consumer_ready=0\n",
            task->id,task->mode,mode==102,configured,task->host_bounds_valid);
        return configured || (ordinary && task->client_area_deferred);
    }
    case 3: {
        const AncoWmsRect bounds={message->data.bounds.left,message->data.bounds.top,
            message->data.bounds.right,message->data.bounds.bottom};
        if(message->data.bounds.task<0 || bounds.left>=bounds.right || bounds.top>=bounds.bottom)
            return TRUE; /* Original negative/Rect.isEmpty void return. */
        RuntimeTask *task=live_task_state(runtime,message->data.bounds.task);
        if(!task)return TRUE;
        /* No claims for unconnected SmartEdge/PC/cast/rotation policies. This
         * actual host ordinary mode1 stream has scale1. Keep the callback
         * state even before a root exists, as the original Task does. */
        task->host_bounds=bounds;task->host_scale=message->data.bounds.scale;
        task->host_bounds_valid=TRUE;
        refresh_native_decorations(runtime);
        const gboolean configured=configure_task_roots(runtime,task);
        /* Original requestTraversal defers native layout work. Even when the
         * host client-area property has not arrived, the shared traversal must
         * publish the committed Linux root geometry that causes that property
         * to be resolved. Do not reverse-call WMS from this Binder callback. */
        if(configured || task->client_area_deferred)schedule_layer_scene(runtime);
        // Real Task state was consumed; original per-window properties may
        // still arrive asynchronously. This is not a presentation receipt.
        return configured || task->client_area_deferred;
    }
    case 4: {
        /* Original false notification is NOOP, not loss of container focus. */
        if (!message->data.task_focus.focused) return TRUE;
        RuntimeTask *task=live_task_state(runtime,message->data.task_focus.task);
        if (!task) return TRUE;
        const gint32 activity=top_visible_activity(runtime,task);
        if (!activity) return TRUE; /* No stale focus replay when a Window maps later. */
        if (task->mode != 1 && task->mode != 102) return FALSE;
        runtime->task_stack=g_list_remove(runtime->task_stack,task);
        runtime->task_stack=g_list_append(runtime->task_stack,task);
        runtime->focused_activity=activity;
        recompute_window_focus(runtime);
        schedule_layer_scene(runtime);
        return TRUE;
    }
    default:return FALSE; /* Bounds/middle/visibility require their real consumers. */
    }
}

static void window_mapped(struct wl_listener *listener, void *data)
{
    (void)data;
    RuntimeWindow *window=wl_container_of(listener,window,map);
    recompute_window_focus(window->runtime);
    schedule_pending_toplevels(window->runtime,"toplevel-map");
}

/* Copies actual protocol state; no borrowed configure survives its signal.
 * A serial is an equality token, not an ordered integer (it can wrap). */
static ClientConfiguration copy_configuration(RuntimeWindow *window,
    const struct wlr_xdg_surface_configure *configure)
{
    ClientConfiguration value = {0};
    if (!configure || configure->surface != window->xdg) return value;
    if (runtime_window_is_toplevel(window) &&
        configure->toplevel_configure) {
        value.role.toplevel = *configure->toplevel_configure;
    } else if (runtime_window_is_popup(window) &&
        configure->popup_configure) {
        value.role.popup = *configure->popup_configure;
    } else return value;
    value.valid = TRUE;
    value.serial = configure->serial;
    return value;
}

static void configuration_sent(struct wl_listener *listener, void *data)
{
    RuntimeWindow *window = wl_container_of(listener, window, configure);
    window->sent_configuration = copy_configuration(window, data);
    const ClientConfiguration *value = &window->sent_configuration;
    if (!value->valid) return;
    const gboolean top = runtime_window_is_toplevel(window);
    printf("RUNTIME_CLIENT_CONFIGURE window=%d epoch=%llu serial=%u role=%d size=%dx%d fullscreen=%d\n",
        window->identity.window_id, (unsigned long long)window->mapping_epoch,
        value->serial, window->xdg->role,
        top ? value->role.toplevel.width : value->role.popup.geometry.width,
        top ? value->role.toplevel.height : value->role.popup.geometry.height,
        top ? value->role.toplevel.fullscreen : FALSE);
}

static void configuration_acknowledged(struct wl_listener *listener, void *data)
{
    RuntimeWindow *window = wl_container_of(listener, window, ack_configure);
    const struct wlr_xdg_surface_configure *configure = data;
    if (!configure || configure->surface != window->xdg) return;
    /* wlroots emits this signal for obsolete entries before setting pending,
     * then for the genuinely accepted configure after setting its serial.
     * Obsolete entries are retired, not individually accepted by the client. */
    const gboolean accepted = window->xdg->configured &&
        window->xdg->pending.configure_serial == configure->serial;
    if (accepted) window->accepted_configuration = copy_configuration(window, configure);
    printf("RUNTIME_CLIENT_CONFIGURE_ACK window=%d epoch=%llu serial=%u accepted=%d pending=%u\n",
        window->identity.window_id, (unsigned long long)window->mapping_epoch,
        configure->serial, accepted, window->xdg->pending.configure_serial);
}

static void window_unmapped(struct wl_listener *listener, void *data)
{
    (void)data;
    RuntimeWindow *window = wl_container_of(listener, window, unmap);
    if(window->wms_show_entered){
        const AncoWmsCreateState state={.anco_enabled=TRUE,
            .window=window->identity.window_id,.type=window->original_type,
            .has_task=TRUE,.task=window->native_system_window ? -1 :
                window->identity.owner.task_id,
            .has_activity=TRUE,.activity=window->identity.owner.activity_id};
        AncoWmsSendResult result;
        const gboolean delivered=anco_wms_broker_set_visibility(
            window->runtime->wms,&state,FALSE,0,&result);
        printf("RUNTIME_WMS_HIDE window=%d activity=%d task=%d reason=0 delivered=%d local=%d transport=%d decoded=%d service=%d\n",
            state.window,state.activity,state.task,delivered,
            result.last.local_error,result.last.transport,
            result.last.decoded,result.last.service_result);
        window->wms_show_entered=FALSE;
    }
    input_pointer_forget_window(window->runtime,window);
    window->sent_configuration = (ClientConfiguration){0};
    window->accepted_configuration = (ClientConfiguration){0};
    window->initial_configure_sent=FALSE;
    window->submitted_geometry_valid=FALSE;
    window->pending_decoration=(AncoWmsRect){0};
    window->submitted_decoration=(AncoWmsRect){0};
    window->submitted_surface_width=0;
    window->submitted_surface_height=0;
    window->pending_surface_width=0;
    window->pending_surface_height=0;
    window->submitted_layer_valid=FALSE;
    window->layout_attempted=FALSE;
    window->manual_task_position_valid=FALSE;
    window->resize_anchor_valid=FALSE;
    window->resize_finalize_serial=0;
    schedule_layer_scene(window->runtime);
    /* An upload not yet accepted by the native queue belongs to the old
     * mapping. Keep native in-flight leases with their original owner until
     * real release, but never submit an old pending buffer after unmap. */
    if (window->submit_idle) {
        g_source_remove(window->submit_idle);
        window->submit_idle = 0;
    }
    if (window->pending_buffer) {
        wlr_buffer_unlock(window->pending_buffer);
        window->pending_buffer = NULL;
    }
    recompute_window_focus(window->runtime);
    printf("RUNTIME_CLIENT_UNMAP window=%d epoch=%llu\n",
        window->identity.window_id, (unsigned long long)window->mapping_epoch);
}

/* Original full-scene metadata, separate from buffer delivery and fences.
 * Only the ordinary root path is closed here. Never send a partial scene when
 * a mapped popup/transient/subsurface still lacks its production geometry. */
static gboolean publish_layer_scene(gpointer data)
{
    Runtime *runtime=data;
    runtime->layer_idle=0;
    if(runtime->stopping)return G_SOURCE_REMOVE;
    GArray *layers=g_array_new(FALSE,FALSE,sizeof(AncoGraphicLayer));
    int status=0;
    guint32 z=0;
    for(GList *t=runtime->task_stack;t && !status;t=t->next){
        RuntimeTask *task=t->data;
        for(guint a=0;a<task->activities->len && !status;++a){
            const gint32 activity=g_array_index(task->activities,gint32,a);
            RuntimeStartingWindow *starting=task->starting_window;
            if(starting&&!starting->retiring&&starting->activity_id==activity){
                AncoGraphicLayer layer=starting->layer;
                AncoWmsRect bounds;gint32 mode;float host_scale;
                if(!effective_task_geometry(task,&bounds,&mode,&host_scale)){
                    status=-EAGAIN;break;
                }
                for(guint edge=0;edge<4;++edge){
                    const gint64 origin=edge%2?bounds.top:bounds.left;
                    const gint64 global=origin+layer.display_frame[edge];
                    if(global<INT32_MIN||global>INT32_MAX){status=-ERANGE;break;}
                    layer.display_frame[edge]=(gint32)global;
                }
                if(status)break;
                layer.z=z++;
                if(starting->submitted){
                    AncoShmSubmission submission;
                    status=anco_shm_queue_submission(starting->queue,&submission);
                    if(status||submission.sequence!=starting->sequence){
                        if(!status)status=-ESTALE;
                        break;
                    }
                    layer.handle=submission.handle;
                    layer.handle_bytes=submission.handle_bytes;
                }
                g_array_append_val(layers,layer);
            }
            for(GList *w=runtime->windows;w;w=w->next){
                RuntimeWindow *window=w->data;
                if(window->retiring || !runtime_window_surface(window)->mapped ||
                    window->identity.owner.task_id!=task->id ||
                    window->identity.owner.activity_id!=activity)continue;
                /* Runs from the shared idle traversal, after callback return.
                 * Movement can update layout without fabricating a new buffer. */
                gboolean metadata_only=FALSE;
                AncoGraphicLayer layer;
                AncoShmSubmission submission={0};
                if(!window->submitted_geometry_valid){
                    /* The first real client buffer can be waiting for native
                     * Task geometry and therefore cannot yet be submitted.
                     * Its locked commit geometry is nevertheless the original
                     * WMS relayout input. Feeding that existing WMS14 path
                     * breaks the native-bounds/first-buffer dependency without
                     * presenting or copying an unaccepted frame. */
                    if(window->pending_buffer)
                        update_original_window_layout(window,&window->pending_geometry);
                    /* Original SerializeLayerVector flag2 carries a genuine
                     * Layer before that Layer has a buffer.  This publishes
                     * the real Window/SurfaceView/geometry so the unchanged
                     * host can create its XComponent and return the real
                     * Delegator code5 client.  The retained wl_buffer remains
                     * unacknowledged and is not copied or presented here. */
                    if(!window->pending_buffer||!window->pending_layer_valid){
                        status=-EAGAIN;break;
                    }
                    layer=window->pending_layer;
                    metadata_only=TRUE;
                }else{
                    update_original_window_layout(window,&window->submitted_geometry);
                    if(!window->submitted_layer_valid){status=-ENOTSUP;break;}
                    status=anco_shm_queue_submission(window->queue,&submission);
                    if(status)break;
                    if(submission.sequence!=window->submitted_sequence){status=-ESTALE;break;}
                    layer=window->submitted_layer;
                }
                AncoWmsRect bounds;gint32 mode;float host_scale;
                if(!effective_task_geometry(task,&bounds,&mode,&host_scale)){
                    status=-EAGAIN;break;
                }
                /* The original HWC display frame is global. The original
                 * host resolves Window -> Task, then removes this Task offset;
                 * keep the cached committed layer local and add it once here. */
                for(guint edge=0;edge<4;++edge){
                    const gint64 origin=edge%2 ? bounds.top : bounds.left;
                    const gint64 global=origin+layer.display_frame[edge];
                    if(global<INT32_MIN || global>INT32_MAX){status=-ERANGE;break;}
                    layer.display_frame[edge]=(gint32)global;
                }
                if(status)break;
                layer.z=z++;
                if(!metadata_only){
                    layer.handle=submission.handle;
                    layer.handle_bytes=submission.handle_bytes;
                }
                g_array_append_val(layers,layer);
            }
        }
    }
    /* Current original receiver rejects count0. Actual WMS lifecycle handles
     * removal; don't invent an empty-scene protocol or duplicate last frame. */
    if(!status && layers->len){
        if(!runtime->layer_stream)status=anco_graphic_layers_open(
            "/dev/cross/uds/virt_graphic_broker/virtgraphic.sock",
            1000,&runtime->layer_stream);
        if(!status){
            const AncoGraphicLayer **scene=g_new(const AncoGraphicLayer *,layers->len);
            for(guint i=0;i<layers->len;++i)scene[i]=&g_array_index(layers,AncoGraphicLayer,i);
            uint32_t sent=0;
            status=anco_graphic_layers_send(runtime->layer_stream,scene,layers->len,1000,&sent);
            printf("RUNTIME_ORIGINAL_LAYER_SCENE layers=%u sent=%u status=%d transport_only=1 presentation_proven=0\n",
                layers->len,sent,status);
            for(guint i=0;!status && i<layers->len;++i){
                const AncoGraphicLayer *layer=scene[i];
                printf("RUNTIME_ORIGINAL_LAYER window=%u surface_view=%llu z=%u opaque=%u snapshot=%d crop=%d,%d,%d,%d frame=%d,%d,%d,%d handle_bytes=%zu metadata_only=%d\n",
                    layer->window,(unsigned long long)layer->surface_view,layer->z,
                    layer->opaque,layer->snapshot_byte,layer->source_crop[0],layer->source_crop[1],
                    layer->source_crop[2],layer->source_crop[3],layer->display_frame[0],
                    layer->display_frame[1],layer->display_frame[2],layer->display_frame[3],
                    layer->handle_bytes,layer->handle==NULL);
            }
            g_free(scene);
        }
    }
    if(status){
        printf("RUNTIME_ORIGINAL_LAYER_SCENE_DEFERRED status=%d mapped_scene_not_sent=1\n",status);
        /* No synthetic transport replay. Next actual state change can attempt
         * a new connection only after disposing the failed original stream. */
        if(status!=-EAGAIN && status!=-ENOTSUP){
            anco_graphic_layers_close(runtime->layer_stream);runtime->layer_stream=NULL;
        }
    }
    g_array_free(layers,TRUE);
    return G_SOURCE_REMOVE;
}
static void schedule_layer_scene(Runtime *runtime)
{
    if(!runtime->stopping && !runtime->layer_idle)
        runtime->layer_idle=g_idle_add(publish_layer_scene,runtime);
}

static gboolean submit_pending(gpointer data)
{
    RuntimeWindow *window=data;
    window->submit_idle=0;
    if(window->retiring||window->submit_failed||!window->queue||!window->pending_buffer)return G_SOURCE_REMOVE;
    /* The real client commit can precede the original Task-bounds callback.
     * Complete that SAME retained commit once its native geometry exists;
     * a size-stable client is not required to repaint merely to repair host
     * ordering.  No buffer, scene or host message is synthesized here. */
    if(!window->pending_layer_valid){
        AncoGraphicLayer layer;
        const int layer_status=committed_original_layer(window,
            &window->pending_geometry,window->pending_buffer,
            &window->pending_decoration,&layer);
        if(!layer_status){
            window->pending_layer=layer;
            window->pending_layer_valid=TRUE;
            printf("RUNTIME_PENDING_LAYER_COMPLETED window=%d commit=%u native_geometry_ready=1 same_client_commit=1\n",
                window->identity.window_id,window->pending_sequence);
            /* The original host cannot return a Delegator client until its
             * real no-buffer Layer (SerializeLayerVector flag2) identifies
             * the SurfaceView.  Publish that existing state before asking the
             * queue for a peer; code5 will schedule this same pending commit. */
            schedule_layer_scene(window->runtime);
        }else{
            printf("RUNTIME_PENDING_LAYER_STILL_UNAVAILABLE window=%d commit=%u status=%d buffer_queue_unchanged=1\n",
                window->identity.window_id,window->pending_sequence,layer_status);
        }
    }
    /* Source transform mapping for non-normal buffers remains unimplemented.
     * Reject rather than stretching/rotating with guessed host conventions. */
    AncoShmDecoration decoration={0};
    int rc=window->pending_transform==WL_OUTPUT_TRANSFORM_NORMAL?
        physical_server_decoration(window,window->pending_buffer,
            &window->pending_decoration,&decoration):-ENOTSUP;
    if(!rc)rc=anco_shm_queue_submit(window->queue,window->pending_buffer,
        window->pending_sequence,rect_has_margins(&window->pending_decoration)?
            &decoration:NULL);
    if(rc==-EAGAIN||rc==-ENOBUFS)return G_SOURCE_REMOVE;
    if(rc){
        window->submit_failed=TRUE;
        printf("RUNTIME_BUFFER_SUBMIT_FAILED window=%d commit=%u status=%d\n",
            window->identity.window_id,window->pending_sequence,rc);
        return G_SOURCE_REMOVE;
    }
    AncoShmSubmission submission;
    const int snapshot_status=anco_shm_queue_submission(window->queue,&submission);
    if(snapshot_status||submission.sequence!=window->pending_sequence){
        window->submit_failed=TRUE;
        printf("RUNTIME_SUBMISSION_GEOMETRY_MISMATCH window=%d status=%d sequence=%u\n",
            window->identity.window_id,snapshot_status,window->pending_sequence);
        return G_SOURCE_REMOVE;
    }
    window->submitted_geometry=window->pending_geometry;
    window->submitted_decoration=window->pending_decoration;
    window->submitted_surface_width=window->pending_surface_width;
    window->submitted_surface_height=window->pending_surface_height;
    window->submitted_geometry_valid=TRUE;
    window->submitted_layer=window->pending_layer;
    window->submitted_layer_valid=window->pending_layer_valid;
    window->submitted_sequence=submission.sequence;
    retire_matching_starting_windows(window);
    printf("RUNTIME_COMMITTED_INPUT_SOURCE window=%d commit=%u epoch=%llu configure=%u logical=%dx%d pointer_dispatch=%d\n",
        window->identity.window_id,submission.sequence,
        (unsigned long long)window->pending_mapping_epoch,window->pending_configure_serial,
        window->submitted_surface_width,window->submitted_surface_height,
        window->runtime->input_pointer_ready);
    schedule_layer_scene(window->runtime);
    printf("RUNTIME_SUBMITTED_GEOMETRY window=%d commit=%u extent=%d,%d,%d,%d geometry=%d,%d,%d,%d native=%ux%u handle_bytes=%zu\n",
        window->identity.window_id,submission.sequence,
        window->submitted_geometry.extent.left,window->submitted_geometry.extent.top,
        window->submitted_geometry.extent.right,window->submitted_geometry.extent.bottom,
        window->submitted_geometry.geometry.left,window->submitted_geometry.geometry.top,
        window->submitted_geometry.geometry.right,window->submitted_geometry.geometry.bottom,
        submission.width,submission.height,submission.handle_bytes);
    wlr_buffer_unlock(window->pending_buffer);window->pending_buffer=NULL;
    struct timespec now;clock_gettime(CLOCK_MONOTONIC,&now);
    /* Standard Wayland pacing after the ORIGINAL queue accepted this upload.
     * Not wp_presentation, NotifyDrawingCompleted, or GPU/fence completion. */
    wlr_surface_send_frame_done(runtime_window_surface(window),&now);
    anco_wayland_context_wake(window->runtime->wayland);
    printf("RUNTIME_BUFFER_QUEUED window=%d commit=%u epoch=%llu configure=%u cpu_source_upload=1 gpu_target_write=1\n",
        window->identity.window_id,window->pending_sequence,
        (unsigned long long)window->pending_mapping_epoch,window->pending_configure_serial);
    return G_SOURCE_REMOVE;
}
static void schedule_submit(void *data)
{
    RuntimeWindow *window=data;
    if(!window->retiring&&!window->submit_failed&&!window->submit_idle)
        window->submit_idle=g_idle_add(submit_pending,window);
}

static gboolean runtime_task_has_windows(Runtime *runtime,gint32 task)
{
    for(GList *node=runtime->windows;node;node=node->next){
        RuntimeWindow *window=node->data;
        if(!window->retiring && window->identity.owner.task_id==task)return TRUE;
    }
    return FALSE;
}

static void retire_semantic_tasks_now(Runtime *runtime)
{
    GArray *ids=g_array_new(FALSE,FALSE,sizeof(gint32));
    GHashTableIter iter;gpointer value;
    g_hash_table_iter_init(&iter,runtime->tasks);
    while(g_hash_table_iter_next(&iter,NULL,&value)){
        RuntimeTask *task=value;
        if(task->retire_requested && !runtime_task_has_windows(runtime,task->id) &&
           (!task->starting_window||runtime->stopping))
            g_array_append_val(ids,task->id);
    }
    for(guint n=0;n<ids->len;++n){
        const gint32 id=g_array_index(ids,gint32,n);
        RuntimeTask *task=g_hash_table_lookup(runtime->tasks,GINT_TO_POINTER(id));
        if(!task || !task->retire_requested || runtime_task_has_windows(runtime,id)||
           (task->starting_window&&!runtime->stopping))continue;
        if(task->starting_window)retire_starting_window(task,"task-retire");
        AncoLinuxLaunch record;gboolean finished=FALSE;
        const int recorded=anco_linux_catalog_task_record(runtime->catalog,id,
            &record,&finished);
        int retired=0,finished_notice=0,removed=0;
        gboolean activities_removed=TRUE;
        if(!recorded){
            for(guint i=0;i<task->activities->len;++i){
                const gint32 activity=g_array_index(task->activities,gint32,i);
                const AncoWmsMessage message={.code=ANCO_WMS_REMOVE_ACTIVITY,
                    .data.activity={activity,id}};
                AncoWmsSendResult result;
                const gboolean delivered=anco_wms_broker_send(runtime->wms,&message,&result);
                printf("RUNTIME_WMS_REMOVE_ACTIVITY activity=%d task=%d delivered=%d local=%d transport=%d decoded=%d service=%d\n",
                    activity,id,delivered,result.last.local_error,result.last.transport,
                    result.last.decoded,result.last.service_result);
                if(!delivered)activities_removed=FALSE;
            }
            if(activities_removed){
                if(!finished){
                    retired=anco_linux_catalog_task_retire(runtime->catalog,id);
                    if(!retired)finished_notice=anco_ams_agent_notify_task_finished(runtime->agent,id);
                }
                if((finished || (!retired && !finished_notice)))
                    removed=anco_ams_agent_notify_task_removed(runtime->agent,id);
            }
        }
        printf("RUNTIME_TASK_RETIRE task=%d recorded=%d already_finished=%d process_exited=%d activity_cleanup=%d catalog=%d original_finished=%d original_removed=%d\n",
            id,recorded,finished,task->process_exited,activities_removed,retired,finished_notice,removed);
        if(!recorded && activities_removed && !retired && !finished_notice && !removed){
            runtime->task_stack=g_list_remove(runtime->task_stack,task);
            g_hash_table_remove(runtime->tasks,GINT_TO_POINTER(id));
        }else runtime->failed=TRUE;
    }
    g_array_free(ids,TRUE);
}

static gboolean retire_semantic_tasks(gpointer data)
{
    Runtime *runtime=data;
    runtime->task_retire_idle=0;
    if(runtime->stopping)return G_SOURCE_REMOVE;
    retire_semantic_tasks_now(runtime);
    return G_SOURCE_REMOVE;
}

static void schedule_semantic_task_retire(Runtime *runtime)
{
    if(!runtime->stopping && !runtime->task_retire_idle)
        runtime->task_retire_idle=g_idle_add(retire_semantic_tasks,runtime);
}

static void retire_window(RuntimeWindow *window,gboolean request_task_retire)
{
    window->retiring=TRUE;
    RuntimeTask *task=g_hash_table_lookup(window->runtime->tasks,
        GINT_TO_POINTER(window->identity.owner.task_id));
    const gboolean task_root=runtime_window_is_toplevel(window) &&
        !window->native_system_window && !direct_parent_window(window);
    /* Every real Linux root owns its AnCo Task, including the first root that
     * adopts the launcher's pre-created Task.  The process is independent: it
     * may remain alive with zero windows and create a fresh Task for a later
     * root.  Retaining the launch Task after its root disappears leaves a
     * host-visible ghost with no Linux surface. */
    if(task && task_root && request_task_retire){
        task->retire_requested=TRUE;
        printf("RUNTIME_TASK_WINDOW_ROOT_RETIRE task=%d process_exited=%d semantic=%d process_preserved=1\n",
            task->id,task->process_exited,task->semantic_task);
    }
    if(window->runtime->text_receiver)
        anco_text_receiver_retire(window->runtime->text_receiver,
            runtime_window_surface(window)->resource);
    input_pointer_forget_window(window->runtime,window);
    window->runtime->windows=g_list_remove(window->runtime->windows,window);
    remove_native_title(window);
    schedule_layer_scene(window->runtime);
    recompute_window_focus(window->runtime);
    if(window->submit_idle)g_source_remove(window->submit_idle);
    printf("RUNTIME_ROLE_RETIRE window=%d task=%d activity=%d\n",
        window->identity.window_id, window->identity.owner.task_id,
        window->identity.owner.activity_id);
    if (window->wms_create_entered) {
        /* Original removeWindow is resetIME -> WMS13 -> native Surface
         * retirement. This runtime has not connected input/IME yet.
         * Do not infer complete Task/Activity retirement from this message. */
        const AncoWmsMessage message = {.code = ANCO_WMS_REMOVE_WINDOW,
            .data.remove = {window->identity.window_id,
                window->identity.owner.activity_id,
                window->native_system_window ? -1 : window->identity.owner.task_id}};
        AncoWmsSendResult result;
        gboolean delivered = anco_wms_broker_send(window->runtime->wms, &message, &result);
        printf("RUNTIME_WMS_REMOVE window=%d delivered=%d local=%d transport=%d decoded=%d service=%d\n",
            window->identity.window_id, delivered, result.last.local_error,
            result.last.transport, result.last.decoded, result.last.service_result);
        if(window->native_system_window && window->runtime->ibus)
            anco_ibus_bridge_host_imf_reset(window->runtime->ibus);
    }
    anco_shm_queue_free(window->queue);
    if (window->producer) {
        AncoDelegatorUnregistration removal;
        const int status = anco_delegator_registry_unregister(
            window->runtime->delegators, window->surface_key,
            window->producer_bound ? window->producer : NULL, &removal);
        printf("RUNTIME_PRODUCER_RETIRE window=%d surface=%llu local=%d found=%d remaining=%u attempted=%d transport=%d decoded=%d service=%d\n",
            window->identity.window_id, (unsigned long long)window->surface_key.second,
            status, removal.record.found, removal.record.remaining,
            removal.remove_attempted, removal.remove.wire.transport,
            removal.remove.wire.decoded, removal.remove.wire.service_result);
        anco_delegator_producer_unref(window->producer);
    }
    wl_list_remove(&window->commit.link);
    wl_list_remove(&window->destroy.link);
    if (window->xdg) {
        wl_list_remove(&window->configure.link);
        wl_list_remove(&window->ack_configure.link);
    }
    wl_list_remove(&window->unmap.link);
    wl_list_remove(&window->map.link);
    if((window->xdg && runtime_window_is_toplevel(window)) ||
       runtime_window_is_xwayland(window))
        wl_list_remove(&window->request_move.link);
    if((window->xdg && runtime_window_is_toplevel(window)) ||
       runtime_window_is_xwayland(window))
        wl_list_remove(&window->request_resize.link);
    if(window->xdg && runtime_window_is_toplevel(window)){
        wl_list_remove(&window->request_maximize.link);
        wl_list_remove(&window->request_minimize.link);
        wl_list_remove(&window->request_fullscreen.link);
    }
    /* Xwayland installs set_title for every managed X surface, including
     * override-redirect menus.  Such a surface is not a toplevel according
     * to runtime_window_is_toplevel(), but its listener still has to leave
     * the XWM signal before the RuntimeWindow storage is released. */
    if(runtime_window_is_toplevel(window) || runtime_window_is_xwayland(window))
        wl_list_remove(&window->set_title.link);
#if WLR_HAS_XWAYLAND
    if(window->xwayland_surface){
        wl_list_remove(&window->set_window_type.link);
        wl_list_remove(&window->set_hints.link);
        wl_list_remove(&window->set_decorations.link);
        wl_list_remove(&window->request_maximize.link);
        wl_list_remove(&window->request_minimize.link);
        wl_list_remove(&window->request_fullscreen.link);
        wl_list_remove(&window->request_configure.link);
        wl_list_remove(&window->request_activate.link);
    }
#endif
    if (window->pending_buffer) wlr_buffer_unlock(window->pending_buffer);
    if(task && task->retire_requested)schedule_semantic_task_retire(window->runtime);
    g_free(window);
}

static void window_destroyed(struct wl_listener *listener, void *data)
{
    (void)data;
    RuntimeWindow *window = wl_container_of(listener, window, destroy);
    retire_window(window,TRUE);
}

static int create_original_window(RuntimeWindow *window)
{
    const AncoLinuxWindowOwner *identity = &window->identity;
    /* Preserve original AnCo WindowState types. A Linux toplevel projected to
     * its own semantic Task is TYPE_APPLICATION even when xdg set_parent names
     * its source toplevel. Only a genuinely same-Task attached toplevel uses
     * TYPE_APPLICATION_ATTACHED_DIALOG; popup stays an application panel. */
    gint32 parent = -1;
    if (runtime_window_is_toplevel(window)) {
#if WLR_HAS_XWAYLAND
        if (window->xwayland_surface) {
            RuntimeWindow *xparent=direct_parent_window(window);
            if (xparent) {
                if (xparent->identity.owner.activity_id==identity->owner.activity_id &&
                    xparent->identity.owner.task_id==identity->owner.task_id) {
                    window->original_type=1003;
                    parent=xparent->identity.window_id;
                } else window->original_type=2;
            } else window->original_type=2;
        } else
#endif
        {
        struct wlr_xdg_toplevel *top=window->xdg->toplevel;
        if(top->parent){
            AncoLinuxWindowOwner parent_owner;
            int status=anco_wayland_ownership_surface_window(
                window->runtime->ownership,top->parent->base->surface->resource,
                &parent_owner);
            if(status)return status;
            if(parent_owner.owner.activity_id==identity->owner.activity_id &&
               parent_owner.owner.task_id==identity->owner.task_id){
                window->original_type=1003;
                parent=parent_owner.window_id;
            }else window->original_type=2;
        }else window->original_type = 2;
        }
    } else if (runtime_window_is_popup(window)) {
        RuntimeWindow *direct_parent=direct_parent_window(window);
        RuntimeWindow *host_parent=original_attached_parent(window);
        if(!direct_parent||!host_parent)return -ENODATA;
        if(direct_parent->identity.owner.activity_id!=identity->owner.activity_id||
           direct_parent->identity.owner.task_id!=identity->owner.task_id||
           host_parent->identity.owner.activity_id!=identity->owner.activity_id||
           host_parent->identity.owner.task_id!=identity->owner.task_id)return -EXDEV;
        window->original_type = 1000;
        /* Android application subwindows attach to the owning application
         * window.  Wayland permits popup -> popup chains, but the original
         * AnCo page inserts every level-3 XComponent below the Activity's
         * direct roof node.  Keep the direct Wayland chain for popup geometry,
         * grabs and destruction while projecting the nearest non-popup owner
         * into the unchanged WMS parentWindowId/roofId field. */
        parent = host_parent->identity.window_id;
    } else return -EPROTOTYPE;
    AncoWmsCreateState state = {.anco_enabled = TRUE,
        .window = identity->window_id, .type = window->original_type,
        .has_task = TRUE,
        .task = window->native_system_window ? -1 : identity->owner.task_id,
        .has_activity = TRUE, .activity = identity->owner.activity_id,
        .has_parent = parent != -1, .parent = parent,
        .last_title = runtime_window_is_toplevel(window) ?
            runtime_window_title(window) : NULL};
    AncoWmsCreateResult result;
    gboolean delivered = anco_wms_broker_create_window(window->runtime->wms, &state, &result);
    window->wms_create_entered = result.disposition == ANCO_WMS_CREATE_SEND_ENTERED;
    printf("RUNTIME_WMS_CREATE window=%d activity=%d task=%d type=%d parent=%d entered=%d delivered=%d local=%d transport=%d decoded=%d service=%d\n",
        identity->window_id, identity->owner.activity_id, state.task,
        state.type, parent, window->wms_create_entered, delivered,
        result.send.last.local_error, result.send.last.transport,
        result.send.last.decoded, result.send.last.service_result);
    if(delivered && window->xdg && runtime_window_is_toplevel(window)){
        schedule_pending_toplevels(window->runtime,"root-wms-create");
    }
    return delivered ? 0 : -EIO;
}

/* Original queue creation and registration followed by an actual Linux SHM
 * buffer consumer. Creation/peer alone is not submission or presentation. */
static void create_original_producer(RuntimeWindow *window)
{
    if (window->producer || window->retiring) return;
    Runtime *runtime = window->runtime;
    const AncoDelegatorSurfaceKey key = window->surface_key;
    if (key.first == G_MAXUINT32 ||
        (runtime->buffershared[0] != '1' && (guint32)key.second == 0)) {
        printf("RUNTIME_PRODUCER_SKIP window=%d surface=%llu\n",
            window->identity.window_id, (unsigned long long)key.second);
        return;
    }
    guint32 count = 0;
    int status = anco_delegator_registry_add(runtime->delegators, key,
        window->identity.owner.task_id, &window->producer, &count);
    if (status) {
        printf("RUNTIME_PRODUCER_ADD window=%d local=%d\n", window->identity.window_id,status);
        return;
    }
    if (count >= 2 && count <= G_MAXINT32) {
        status = anco_delegator_producer_reset(window->producer);
        AncoDelegatorResult clear;
        if (!status) {
            anco_delegator_producer_clear_all(window->producer, &clear);
            window->producer_bound = TRUE;
        }
        printf("RUNTIME_PRODUCER_REUSE window=%d count=%u reset=%d\n",
            window->identity.window_id, count, status);
        if(!status){
            window->queue=anco_shm_queue_new(window->producer,schedule_submit,window);
            if(window->queue && window->pending_buffer)schedule_submit(window);
        }
        return;
    }
    AncoGraphicLookup lookup;
    GBinderRemoteObject *graphic = anco_graphic_lookup_once(runtime->ipc,&lookup);
    GBinderLocalObject *object = anco_delegator_producer_ref_object(window->producer);
    AncoGraphicResult result = {0,-1,FALSE,-1};
    if (graphic && object) {
        const char *title = runtime_window_is_toplevel(window) ?
            runtime_window_title(window) : NULL;
        anco_graphic_create_surface_node_once(graphic,key.second,(gint32)key.first,
            title ? title : "",object,&result);
        /* Preserve original reply default0; do not treat it as delivery proof. */
        window->producer_bound = !result.local_error &&
            (!result.decoded || result.service_result == 0);
    }
    printf("RUNTIME_PRODUCER_CREATE window=%d surface=%llu count=%u lookup=%d resolved=%d object=%d local=%d transport=%d decoded=%d service=%d binding=%d listener=0 host_submitted=0\n",
        window->identity.window_id,(unsigned long long)key.second,count,
        lookup.transport,lookup.resolved,object != NULL,result.local_error,
        result.transport,result.decoded,result.service_result,window->producer_bound);
    gbinder_local_object_unref(object);
    gbinder_remote_object_unref(graphic);
    if(window->producer_bound){
        window->queue=anco_shm_queue_new(window->producer,schedule_submit,window);
        if(window->queue && window->pending_buffer)schedule_submit(window);
    }
}

static void starting_buffer_destroy(struct wlr_buffer *base)
{
    RuntimeStartingBuffer *buffer=wl_container_of(base,buffer,base);
    g_free(buffer->pixels);
    g_free(buffer);
}

static bool starting_buffer_begin(struct wlr_buffer *base,uint32_t flags,
    void **data,uint32_t *format,size_t *stride)
{
    RuntimeStartingBuffer *buffer=wl_container_of(base,buffer,base);
    if(flags&WLR_BUFFER_DATA_PTR_ACCESS_WRITE)return false;
    *data=buffer->pixels;*format=DRM_FORMAT_ARGB8888;*stride=buffer->stride;
    return true;
}

static void starting_buffer_end(struct wlr_buffer *base)
{
    (void)base;
}

static const struct wlr_buffer_impl starting_buffer_impl={
    .destroy=starting_buffer_destroy,
    .begin_data_ptr_access=starting_buffer_begin,
    .end_data_ptr_access=starting_buffer_end
};

static RuntimeStartingBuffer *starting_buffer_new(Runtime *runtime,
    const char *bundle,guint32 width,guint32 height)
{
    if(!runtime||!bundle||!runtime->icon_directory||!width||!height||
       width>3840||height>2160)return NULL;
    RuntimeStartingBuffer *buffer=g_new0(RuntimeStartingBuffer,1);
    buffer->stride=(size_t)width*4;
    if(buffer->stride>SIZE_MAX/height){g_free(buffer);return NULL;}
    buffer->pixels=g_try_malloc(buffer->stride*height);
    if(!buffer->pixels){g_free(buffer);return NULL;}
    for(guint32 y=0;y<height;++y){
        guint8 *row=buffer->pixels+(size_t)y*buffer->stride;
        for(guint32 x=0;x<width;++x){
            row[4*x]=0xf7;row[4*x+1]=0xf7;row[4*x+2]=0xf7;row[4*x+3]=0xff;
        }
    }
    gchar *name=g_strconcat(bundle,".bgra",NULL);
    gchar *path=g_build_filename(runtime->icon_directory,name,NULL);
    gchar *icon=NULL;gsize bytes=0;
    const gboolean loaded=g_file_get_contents(path,&icon,&bytes,NULL) && bytes==64u*64u*4u;
    g_free(path);g_free(name);
    if(loaded){
        const guint32 edge=MIN((guint32)128,MAX((guint32)64,MIN(width,height)/8));
        const guint32 left=(width-edge)/2,top=(height-edge)/2;
        for(guint32 y=0;y<edge;++y){
            const guint32 sy=MIN((guint32)63,(y*64u)/edge);
            for(guint32 x=0;x<edge;++x){
                const guint32 sx=MIN((guint32)63,(x*64u)/edge);
                const guint8 *source=(const guint8 *)icon+((size_t)sy*64u+sx)*4u;
                guint8 *target=buffer->pixels+(size_t)(top+y)*buffer->stride+
                    (size_t)(left+x)*4u;
                const guint32 alpha=source[3],inverse=255u-alpha;
                target[0]=(guint8)((source[0]*alpha+target[0]*inverse+127u)/255u);
                target[1]=(guint8)((source[1]*alpha+target[1]*inverse+127u)/255u);
                target[2]=(guint8)((source[2]*alpha+target[2]*inverse+127u)/255u);
                target[3]=0xff;
            }
        }
    }
    g_free(icon);
    wlr_buffer_init(&buffer->base,&starting_buffer_impl,(int)width,(int)height);
    printf("RUNTIME_STARTING_BUFFER bundle=%s width=%u height=%u icon=%d source=resource-provider-bgra\n",
        bundle,width,height,loaded);
    return buffer;
}

static gboolean submit_starting_window(gpointer data);

static void schedule_starting_submit(void *data)
{
    RuntimeStartingWindow *starting=data;
    if(!starting->retiring&&!starting->submitted&&!starting->submit_idle)
        starting->submit_idle=g_idle_add(submit_starting_window,starting);
}

static gboolean submit_starting_window(gpointer data)
{
    RuntimeStartingWindow *starting=data;
    starting->submit_idle=0;
    if(starting->retiring||starting->submitted||!starting->queue||!starting->buffer)
        return G_SOURCE_REMOVE;
    const int status=anco_shm_queue_submit(starting->queue,&starting->buffer->base,
        starting->sequence,NULL);
    if(status==-EAGAIN||status==-ENOBUFS)return G_SOURCE_REMOVE;
    if(status){
        printf("RUNTIME_STARTING_SUBMIT_FAILED task=%d window=%d status=%d\n",
            starting->task->id,starting->window_id,status);
        return G_SOURCE_REMOVE;
    }
    AncoShmSubmission submission;
    const int snapshot=anco_shm_queue_submission(starting->queue,&submission);
    if(snapshot||submission.sequence!=starting->sequence){
        printf("RUNTIME_STARTING_SUBMISSION_MISMATCH task=%d window=%d status=%d sequence=%u\n",
            starting->task->id,starting->window_id,snapshot,starting->sequence);
        return G_SOURCE_REMOVE;
    }
    starting->submitted=TRUE;
    schedule_layer_scene(starting->runtime);
    printf("RUNTIME_STARTING_SUBMITTED task=%d window=%d sequence=%u width=%u height=%u original_type=4\n",
        starting->task->id,starting->window_id,submission.sequence,
        submission.width,submission.height);
    return G_SOURCE_REMOVE;
}

static int create_starting_producer(RuntimeStartingWindow *starting)
{
    Runtime *runtime=starting->runtime;
    guint32 count=0;
    int status=anco_delegator_registry_add(runtime->delegators,starting->surface_key,
        starting->task->id,&starting->producer,&count);
    if(status)return status;
    AncoGraphicLookup lookup;
    GBinderRemoteObject *graphic=anco_graphic_lookup_once(runtime->ipc,&lookup);
    GBinderLocalObject *object=anco_delegator_producer_ref_object(starting->producer);
    AncoGraphicResult result={0,-1,FALSE,-1};
    if(graphic&&object){
        anco_graphic_create_surface_node_once(graphic,starting->surface_key.second,
            (gint32)starting->surface_key.first,"",object,&result);
        starting->producer_bound=!result.local_error&&
            (!result.decoded||result.service_result==0);
    }
    printf("RUNTIME_STARTING_PRODUCER task=%d window=%d surface=%llu count=%u lookup=%d resolved=%d object=%d local=%d transport=%d decoded=%d service=%d binding=%d\n",
        starting->task->id,starting->window_id,
        (unsigned long long)starting->surface_key.second,count,lookup.transport,
        lookup.resolved,object!=NULL,result.local_error,result.transport,
        result.decoded,result.service_result,starting->producer_bound);
    gbinder_local_object_unref(object);gbinder_remote_object_unref(graphic);
    if(!starting->producer_bound)return -EIO;
    starting->queue=anco_shm_queue_new(starting->producer,
        schedule_starting_submit,starting);
    return starting->queue?0:-ENOMEM;
}

static int ensure_starting_window(Runtime *runtime,RuntimeTask *task)
{
    if(!runtime||!task||task->semantic_task||!task->retire_requested)return -EINVAL;
    if(task->starting_window)return 0;
    if(!task->mission_associated||!task->client_area_valid||
       !task->graphic_frontend_ready)return -EAGAIN;
    AncoLinuxLaunch launch;
    if(anco_linux_catalog_task(runtime->catalog,task->id,&launch))return -ENOENT;
    if(!task->activities->len)return -ENOENT;
    const gint32 activity=g_array_index(task->activities,gint32,
        task->activities->len-1);
    gint32 window_id=0;
    int status=anco_linux_catalog_window_reserve_transient(runtime->catalog,
        activity,&window_id);
    if(status)return status;
    const gint32 width=task->client_area.right-task->client_area.left;
    const gint32 height=task->client_area.bottom-task->client_area.top;
    AncoWmsRect task_bounds;gint32 mode;float scale;
    if(width<=0||height<=0||!effective_task_geometry(task,&task_bounds,&mode,&scale))
        return -ERANGE;
    RuntimeStartingWindow *starting=g_new0(RuntimeStartingWindow,1);
    starting->runtime=runtime;starting->task=task;
    starting->activity_id=activity;starting->window_id=window_id;
    starting->surface_key=(AncoDelegatorSurfaceKey){(guint32)window_id,
        anco_surface_identity_generate(task->id,FALSE,runtime->shared_flag)};
    starting->sequence=1;
    starting->buffer=starting_buffer_new(runtime,launch.app.bundle,
        (guint32)width,(guint32)height);
    if(!starting->buffer){g_free(starting);return -ENOMEM;}
    const AncoWmsCreateState create={.anco_enabled=TRUE,.window=window_id,.type=4,
        .has_task=TRUE,.task=task->id,.has_activity=TRUE,.activity=activity};
    AncoWmsCreateState mutable_create=create;
    AncoWmsCreateResult created;
    const gboolean delivered=anco_wms_broker_create_window(runtime->wms,
        &mutable_create,&created);
    starting->wms_created=created.disposition==ANCO_WMS_CREATE_SEND_ENTERED&&delivered;
    if(!starting->wms_created){
        printf("RUNTIME_STARTING_CREATE_FAILED task=%d window=%d delivered=%d entered=%d local=%d transport=%d decoded=%d service=%d\n",
            task->id,window_id,delivered,
            created.disposition==ANCO_WMS_CREATE_SEND_ENTERED,
            created.send.last.local_error,created.send.last.transport,
            created.send.last.decoded,created.send.last.service_result);
        wlr_buffer_drop(&starting->buffer->base);g_free(starting);return -EIO;
    }
    const AncoWmsLayout attrs={.type=4,.screen_brightness=-1,
        .screen_orientation=-1,.fit_insets_types=7,.fit_insets_sides=15};
    const AncoWmsActivityLayoutState activity_layout={
        .client_visible=TRUE,.visible_requested=TRUE};
    const AncoWmsLayoutState geometry={.anco_enabled=TRUE,.has_frame=TRUE,
        .window_type=4,.frame=task->client_area,.surface_insets={0},
        .has_task=TRUE,.task_bounds=task_bounds,.activities=&activity_layout,
        .activity_count=1};
    AncoWmsSendResult layout;
    const gboolean layout_delivered=anco_wms_broker_update_layout(runtime->wms,
        &create,&attrs,&geometry,&layout);
    if(!layout_delivered){task->starting_window=starting;
        retire_starting_window(task,"layout-failed");return -EIO;}
    status=create_starting_producer(starting);
    if(status){task->starting_window=starting;
        retire_starting_window(task,"producer-failed");return status;}
    starting->layer=(AncoGraphicLayer){.surface_view=starting->surface_key.second,
        .window=(guint32)window_id,.opaque=1,.scale=-1.0f,.composition=2,
        .source_crop={0,0,width,height},
        .display_frame={task->client_area.left-task_bounds.left,
            task->client_area.top-task_bounds.top,
            task->client_area.right-task_bounds.left,
            task->client_area.bottom-task_bounds.top}};
    AncoWmsSendResult visible;
    starting->wms_shown=anco_wms_broker_set_visibility(runtime->wms,&create,
        TRUE,0,&visible);
    task->starting_window=starting;
    if(!starting->wms_shown){
        retire_starting_window(task,"show-failed");
        return -EIO;
    }
    schedule_layer_scene(runtime);
    schedule_starting_submit(starting);
    printf("RUNTIME_STARTING_CREATE task=%d activity=%d window=%d bundle=%s type=4 frame=%d,%d,%d,%d shown=%d original_protocol_only=1\n",
        task->id,activity,window_id,launch.app.bundle,
        task->client_area.left,task->client_area.top,
        task->client_area.right,task->client_area.bottom,starting->wms_shown);
    return 0;
}

static void retire_starting_window(RuntimeTask *task,const char *reason)
{
    RuntimeStartingWindow *starting=task?task->starting_window:NULL;
    if(!starting||starting->retiring)return;
    starting->retiring=TRUE;task->starting_window=NULL;
    if(starting->submit_idle)g_source_remove(starting->submit_idle);
    if(starting->wms_created){
        const AncoWmsMessage message={.code=ANCO_WMS_REMOVE_WINDOW,
            .data.remove={starting->window_id,starting->activity_id,task->id}};
        AncoWmsSendResult result;
        const gboolean removed=anco_wms_broker_send(starting->runtime->wms,
            &message,&result);
        printf("RUNTIME_STARTING_REMOVE task=%d window=%d reason=%s delivered=%d local=%d transport=%d decoded=%d service=%d\n",
            task->id,starting->window_id,reason?reason:"",removed,
            result.last.local_error,result.last.transport,result.last.decoded,
            result.last.service_result);
    }
    anco_shm_queue_free(starting->queue);
    if(starting->producer){
        AncoDelegatorUnregistration removal;
        const int status=anco_delegator_registry_unregister(
            starting->runtime->delegators,starting->surface_key,
            starting->producer_bound?starting->producer:NULL,&removal);
        printf("RUNTIME_STARTING_PRODUCER_RETIRE task=%d window=%d status=%d remaining=%u\n",
            task->id,starting->window_id,status,removal.record.remaining);
        anco_delegator_producer_unref(starting->producer);
    }
    if(starting->buffer)wlr_buffer_drop(&starting->buffer->base);
    schedule_layer_scene(starting->runtime);
    g_free(starting);
}

static void retire_matching_starting_windows(RuntimeWindow *window)
{
    if(!window||!runtime_window_is_toplevel(window)||window->native_system_window||
       direct_parent_window(window))return;
    AncoLinuxLaunch root;
    if(anco_linux_catalog_task(window->runtime->catalog,
        window->identity.owner.task_id,&root))return;
    GHashTableIter iter;gpointer value;
    g_hash_table_iter_init(&iter,window->runtime->tasks);
    while(g_hash_table_iter_next(&iter,NULL,&value)){
        RuntimeTask *task=value;AncoLinuxLaunch launch;
        if(task->semantic_task||!task->retire_requested||!task->starting_window||
           anco_linux_catalog_task(window->runtime->catalog,task->id,&launch)||
           strcmp(root.app.bundle,launch.app.bundle))continue;
        retire_starting_window(task,"first-real-root-buffer");
        schedule_semantic_task_retire(window->runtime);
        printf("RUNTIME_LAUNCH_SWITCH_RETIRE_READY task=%d mission=%d native_id=%d original_starting_window_removed=1 real_window_task=%d\n",
            task->id,task->mission_id,task->client_native_id,
            window->identity.owner.task_id);
    }
}

static gboolean resume_graphic_producers_idle(gpointer data)
{
    Runtime *runtime=data;
    runtime->producer_resume_idle=0;
    if(runtime->stopping)return G_SOURCE_REMOVE;
    for(GList *node=runtime->windows;node;node=node->next){
        RuntimeWindow *window=node->data;
        RuntimeTask *task=live_task_state(runtime,
            window->identity.owner.task_id);
        if(!window->retiring && window->wms_create_entered && !window->producer &&
           task && task->graphic_frontend_ready)create_original_producer(window);
    }
    return G_SOURCE_REMOVE;
}

static void schedule_graphic_producers_for_task(Runtime *runtime,RuntimeTask *task)
{
    if(runtime->stopping || runtime->producer_resume_idle)return;
    runtime->producer_resume_idle=g_idle_add(resume_graphic_producers_idle,runtime);
    printf("RUNTIME_PRODUCER_RESUME_SCHEDULED task=%d source=%u original_callback_unwound=1\n",
        task->id,runtime->producer_resume_idle);
}

static gboolean root_initial_commit_must_wait(RuntimeWindow *window)
{
    if(!runtime_window_is_toplevel(window))return FALSE;
    RuntimeTask *task=live_task_state(window->runtime,
        window->identity.owner.task_id);
    /* A root belongs to the original Task page.  Wait for the mission, real
     * host mode, passWindowInfoToMM registration and the original GraphicBroker
     * frontend listener before CreateWindow.  A client-area record may arrive
     * first, but its Wayland configure is deferred until this root is created. */
    return !task || !window->pending_buffer || !task->mission_associated ||
        !task->host_mode_known || !task->frontend_window_registered ||
        !task->graphic_frontend_ready;
}

static void schedule_standard_initial_configure(RuntimeWindow *window)
{
    if(window->initial_configure_sent)return;
    window->initial_configure_sent=TRUE;
#if WLR_HAS_XWAYLAND
    if (window->xwayland_surface) {
        printf("RUNTIME_INITIAL_CONFIGURE window=%d protocol=x11 serial=0\n",
            window->identity.window_id);
        return;
    }
#endif
    const uint32_t serial=wlr_xdg_surface_schedule_configure(window->xdg);
    printf("RUNTIME_INITIAL_CONFIGURE window=%d serial=%u\n",
        window->identity.window_id,serial);
    if(window->runtime->wayland)
        anco_wayland_context_wake(window->runtime->wayland);
}

static gboolean initialize_initial_commit(RuntimeWindow *window)
{
    if(!window->initial_commit_pending)return FALSE;
    if(root_initial_commit_must_wait(window)){
        RuntimeTask *task=live_task_state(window->runtime,
            window->identity.owner.task_id);
        printf("RUNTIME_INITIAL_CONFIGURE_DEFERRED window=%d task=%d mission_associated=%d host_mode_known=%d host_mode=%d frontend_window_registered=%d graphic_frontend_ready=%d client_area_valid=%d natural_buffer_ready=%d original_window_create_deferred=1\n",
            window->identity.window_id,task?task->id:-1,
            task?task->mission_associated:FALSE,
            task?task->host_mode_known:FALSE,task?task->mode:-1,
            task?task->frontend_window_registered:FALSE,
            task?task->graphic_frontend_ready:FALSE,
            task?task->client_area_valid:FALSE,
            window->pending_buffer!=NULL);
        /* XDG's initial no-size configure is independent of native Window
         * creation.  Send it now so every toolkit can commit its real natural
         * first buffer while the temporary host mode1 is being recovered.
         * The existing pending-buffer traversal will feed that geometry into
         * original WMS14 only after CreateWindow enters in real mode102. */
        schedule_standard_initial_configure(window);
        return FALSE;
    }
    window->initial_commit_pending=FALSE;
    if (!window->wms_create_attempted) {
        window->wms_create_attempted = TRUE;
        /* WindowToken.addWindow/ActivityRecord's same-type comparator orders
         * normal Windows when actually added. Remap/redraw never reorders. */
        window->runtime->windows=g_list_remove(window->runtime->windows,window);
        window->runtime->windows=g_list_append(window->runtime->windows,window);
        schedule_layer_scene(window->runtime);
        int status=create_original_window(window);
        if(status)printf("RUNTIME_WMS_CREATE_FAILED window=%d status=%d\n",
            window->identity.window_id,status);
        if(!status)create_original_producer(window);
    }
    RuntimeTask *task=live_task_state(window->runtime,
        window->identity.owner.task_id);
    AncoWmsRect bounds;gint32 mode;float scale;
    if(effective_task_geometry(task,&bounds,&mode,&scale))
        configure_task_roots(window->runtime,task);
    /* This is the same standard handshake. It may already have been sent
     * while only original native creation was deferred. */
    schedule_standard_initial_configure(window);
    return TRUE;
}

static gboolean resume_initial_commits_idle(gpointer data)
{
    Runtime *runtime=data;
    runtime->initial_commit_resume_idle=0;
    if(runtime->stopping)return G_SOURCE_REMOVE;
    for(GList *node=runtime->windows;node;){
        GList *next=node->next;
        RuntimeWindow *window=node->data;
        if(!window->retiring && window->initial_commit_pending)
            initialize_initial_commit(window);
        node=next;
    }
    return G_SOURCE_REMOVE;
}

static void schedule_initial_commits_for_task(Runtime *runtime,RuntimeTask *task)
{
    if(runtime->stopping || runtime->initial_commit_resume_idle)return;
    /* The original mode callback runs on its Binder/GLib delivery stack.
     * Never reverse-call original CreateWindow from that callback: Recover()
     * has not finished applying the freeform mode yet.  Resume the already
     * received XDG commit on the shared serialized main context after the
     * callback returns, matching the normal Wayland initial-commit path. */
    runtime->initial_commit_resume_idle=g_idle_add(resume_initial_commits_idle,runtime);
    printf("RUNTIME_INITIAL_CONFIGURE_RESUME_SCHEDULED task=%d source=%u original_callback_unwound=1\n",
        task->id,runtime->initial_commit_resume_idle);
}

static RuntimeWindow *runtime_window_for_xdg(Runtime *runtime,
    struct wlr_xdg_surface *xdg)
{
    for(GList *node=runtime->windows;node;node=node->next){
        RuntimeWindow *candidate=node->data;
        if(!candidate->retiring && candidate->xdg==xdg)return candidate;
    }
    return NULL;
}

static RuntimeWindow *runtime_parent_for_xdg(Runtime *runtime,
    struct wlr_xdg_surface *xdg)
{
    struct wlr_xdg_surface *parent=NULL;
    if(xdg->role==WLR_XDG_SURFACE_ROLE_TOPLEVEL){
        if(xdg->toplevel->parent)parent=xdg->toplevel->parent->base;
    }else if(xdg->role==WLR_XDG_SURFACE_ROLE_POPUP){
        parent=wlr_xdg_surface_try_from_wlr_surface(xdg->popup->parent);
    }
    return parent?runtime_window_for_xdg(runtime,parent):NULL;
}

#if WLR_HAS_XWAYLAND
static RuntimeWindow *runtime_window_for_xwayland(Runtime *runtime,
    struct wlr_xwayland_surface *surface)
{
    for (GList *node=runtime->windows; node; node=node->next) {
        RuntimeWindow *candidate=node->data;
        if (!candidate->retiring && candidate->xwayland_surface==surface)
            return candidate;
    }
    return NULL;
}

static RuntimeWindow *runtime_parent_for_xwayland(Runtime *runtime,
    struct wlr_xwayland_surface *surface)
{
    return surface && surface->parent ?
        runtime_window_for_xwayland(runtime,surface->parent) : NULL;
}
#endif

static RuntimeWindow *original_attached_parent(RuntimeWindow *window)
{
    if(!window)return NULL;
    RuntimeWindow *parent=direct_parent_window(window);
    guint depth=0;
    while(parent&&runtime_window_is_popup(parent)){
        if(parent->identity.owner.activity_id!=window->identity.owner.activity_id||
           parent->identity.owner.task_id!=window->identity.owner.task_id)return NULL;
        RuntimeWindow *next=direct_parent_window(parent);
        if(!next||next==parent||++depth>64)return NULL;
        parent=next;
    }
    if(parent&&(parent->identity.owner.activity_id!=window->identity.owner.activity_id||
       parent->identity.owner.task_id!=window->identity.owner.task_id))return NULL;
    return parent;
}

static RuntimeWindow *direct_parent_window(RuntimeWindow *window)
{
    if (!window) return NULL;
    if (window->xdg) return runtime_parent_for_xdg(window->runtime,window->xdg);
#if WLR_HAS_XWAYLAND
    if (window->xwayland_surface)
        return runtime_parent_for_xwayland(window->runtime,
            window->xwayland_surface);
#endif
    return NULL;
}

static gboolean same_task_toplevel_parent(RuntimeWindow *window)
{
    if(!window || !runtime_window_is_toplevel(window) ||
       !direct_parent_window(window))return FALSE;
    RuntimeWindow *parent=direct_parent_window(window);
    return parent && parent->identity.owner.task_id==window->identity.owner.task_id;
}

static gboolean rect_has_margins(const AncoWmsRect *rect)
{
    return rect && (rect->left||rect->top||rect->right||rect->bottom);
}

static gboolean negotiated_server_decoration(RuntimeWindow *window)
{
    if(!window->xdg || !same_task_toplevel_parent(window))return FALSE;
    struct wlr_xdg_toplevel_decoration_v1 *xdg_decoration;
    wl_list_for_each(xdg_decoration,&window->runtime->decoration_manager->decorations,link){
        if(xdg_decoration->toplevel!=window->xdg->toplevel)continue;
        enum wlr_xdg_toplevel_decoration_v1_mode mode=xdg_decoration->current.mode;
        if(mode==WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_NONE)
            mode=xdg_decoration->scheduled_mode;
        return mode==WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE;
    }
    struct wlr_server_decoration *legacy;
    wl_list_for_each(legacy,&window->runtime->legacy_decoration_manager->decorations,link){
        if(legacy->surface==runtime_window_surface(window))
            return legacy->mode==WLR_SERVER_DECORATION_MANAGER_MODE_SERVER;
    }
    /* XDG clients are allowed not to bind either optional protocol. The
     * compositor's declared policy for an ordinary decorated transient is
     * SERVER; popups and root Task windows were excluded above. */
    return TRUE;
}

static AncoWmsRect logical_server_decoration(RuntimeWindow *window)
{
    if(!negotiated_server_decoration(window))return (AncoWmsRect){0};
    return (AncoWmsRect){1,36,1,1};
}

static int physical_server_decoration(const RuntimeWindow *window,
    const struct wlr_buffer *buffer,const AncoWmsRect *logical,
    AncoShmDecoration *out)
{
    if(!out)return -EINVAL;
    *out=(AncoShmDecoration){0};
    if(!rect_has_margins(logical))return 0;
    if(!window||!buffer||window->pending_surface_width<=0||
        window->pending_surface_height<=0)return -EINVAL;
    const double sx=(double)buffer->width/window->pending_surface_width;
    const double sy=(double)buffer->height/window->pending_surface_height;
    const double values[]={logical->left*sx,logical->top*sy,
        logical->right*sx,logical->bottom*sy};
    guint32 rounded[4];
    for(guint i=0;i<4;++i){
        if(!isfinite(values[i])||values[i]<0||values[i]>UINT32_MAX)return -ERANGE;
        rounded[i]=(guint32)lround(values[i]);
    }
    if(!rounded[1])return -ERANGE;
    *out=(AncoShmDecoration){rounded[0],rounded[1],rounded[2],rounded[3],
        runtime_window_title(window)};
    return 0;
}

/* Actual ordinary Window frame in the original Task coordinate space.
 * Root toplevels use the real Task origin. A transient toplevel has no XDG
 * position, so this compositor supplies the standard guest-WM placement by
 * centering its client geometry over the direct parent. A popup's committed
 * xdg_positioner geometry is already relative to the direct parent's window
 * geometry. These semantic inputs feed the unchanged original fromLp path;
 * no host window protocol, captured canvas or toolkit-specific coordinate is
 * introduced. */
static void update_original_window_layout_force(RuntimeWindow *window,
    const AncoWaylandGeometry *logical,gboolean force)
{
    if (!window->wms_create_entered || !logical) return;
    const RuntimeTask *task=live_task_state(window->runtime,window->identity.owner.task_id);
    AncoWmsRect task_bounds;gint32 task_mode;float task_scale;
    if(!effective_task_geometry(task,&task_bounds,&task_mode,&task_scale))return;
    /* The original AnCo WMS owns Task bounds independently from the graphic
     * producer.  Standard Wayland keeps the last committed buffer visible
     * while a later configure is outstanding.  Admit a commit only when its
     * own configure was acknowledged; never require it to acknowledge the
     * newest configure first.  The latter deadlocks clients which legitimately
     * keep rendering against their last accepted state.  Toplevel configure
     * width/height remain window-geometry hints for ordinary floating windows;
     * the real committed geometry enters the unchanged AnCo UPDATE_LAYOUT. */
    if(window->xdg && runtime_window_is_toplevel(window) &&
       !same_task_toplevel_parent(window) && task && task->host_bounds_valid &&
       window->sent_configuration.valid &&
       window->sent_configuration.role.toplevel.width>0 &&
       window->sent_configuration.role.toplevel.height>0){
        const guint32 committed_serial=window->xdg->current.configure_serial;
        const gboolean commit_accepted=window->accepted_configuration.valid &&
            window->accepted_configuration.serial==committed_serial;
        if(!commit_accepted){
            const gint32 logical_width=logical->geometry.right-logical->geometry.left;
            const gint32 logical_height=logical->geometry.bottom-logical->geometry.top;
            printf("RUNTIME_ORIGINAL_WINDOW_LAYOUT_DEFERRED window=%d reason=client-commit-not-accepted committed=%u sent=%u accepted=%u logical=%dx%d target=%dx%d original_task_bounds_unchanged=1\n",
                window->identity.window_id,
                committed_serial,
                window->sent_configuration.serial,
                window->accepted_configuration.valid?
                    window->accepted_configuration.serial:0,
                logical_width,logical_height,
                window->sent_configuration.role.toplevel.width,
                window->sent_configuration.role.toplevel.height);
            return;
        }
    }
    struct wlr_surface *surface = runtime_window_surface(window);
    const double scale=runtime_window_content_scale(window);
    const AncoWmsRect decoration=logical_server_decoration(window);
    const double client_width = ((gint64)logical->geometry.right-logical->geometry.left)*scale;
    const double client_height = ((gint64)logical->geometry.bottom-logical->geometry.top)*scale;
    if (!surface->mapped || client_width <= 0 || client_height <= 0 ||
        !isfinite(client_width) || !isfinite(client_height) ||
        client_width > G_MAXINT32 || client_height > G_MAXINT32) return;
    const gint32 client_w = (gint32)lround(client_width);
    const gint32 client_h = (gint32)lround(client_height);
    const gint64 native_decor64[]={lround(decoration.left*scale),
        lround(decoration.top*scale),lround(decoration.right*scale),
        lround(decoration.bottom*scale)};
    for(guint i=0;i<4;++i)if(native_decor64[i]<0||native_decor64[i]>INT32_MAX)return;
    const gint32 native_decor[]={native_decor64[0],native_decor64[1],
        native_decor64[2],native_decor64[3]};
    const gint32 w=client_w,h=client_h;
    /* Project actual surface extent relative to the client's WINDOW geometry.
     * Round endpoints once; rounding each inset independently can change the
     * total surface size at fractional native VPR. Buffer scale is sampling
     * density and does not supply any of these logical positions. */
    const gint64 logical_edges[]={
        (gint64)logical->extent.left-logical->geometry.left,
        (gint64)logical->extent.top-logical->geometry.top,
        (gint64)logical->extent.right-logical->geometry.left,
        (gint64)logical->extent.bottom-logical->geometry.top};
    gint32 extent[4];
    for(guint i=0;i<4;++i){
        const double edge=logical_edges[i]*scale;
        if(!isfinite(edge)||edge<G_MININT32||edge>G_MAXINT32)return;
        extent[i]=(gint32)lround(edge);
    }
    const gint64 margins[]={native_decor[0]-(gint64)extent[0],
        native_decor[1]-(gint64)extent[1],
        (gint64)extent[2]-client_w+native_decor[2],
        (gint64)extent[3]-client_h+native_decor[3]};
    for(guint i=0;i<4;++i)if(margins[i]<0||margins[i]>G_MAXINT32)return;
    RuntimeWindow *parent=direct_parent_window(window);
    RuntimeWindow *host_parent=runtime_window_is_popup(window)?
        original_attached_parent(window):parent;
    gint64 left=task_bounds.left,top=task_bounds.top;
    if(same_task_toplevel_parent(window)){
        if(!parent || !parent->layout_attempted)return;
        if(window->manual_task_position_valid){
            left=window->resize_anchor_valid &&
                (window->resize_anchor_edges&WLR_EDGE_LEFT) ?
                (gint64)task_bounds.left+window->resize_anchor_right_task-w :
                (gint64)task_bounds.left+window->manual_task_x;
            top=window->resize_anchor_valid &&
                (window->resize_anchor_edges&WLR_EDGE_TOP) ?
                (gint64)task_bounds.top+window->resize_anchor_bottom_task-h :
                (gint64)task_bounds.top+window->manual_task_y;
        }else{
            const gint64 parent_width=(gint64)parent->layout_frame.right-parent->layout_frame.left;
            const gint64 parent_height=(gint64)parent->layout_frame.bottom-parent->layout_frame.top;
            left=(gint64)parent->layout_frame.left+(parent_width-w)/2;
            top=(gint64)parent->layout_frame.top+(parent_height-h)/2;
        }
    }else if(runtime_window_is_popup(window)){
        if(!parent || !parent->layout_attempted)return;
        double popup_x=0,popup_y=0;
        if (window->xdg) {
            popup_x=(double)window->xdg->popup->current.geometry.x*scale;
            popup_y=(double)window->xdg->popup->current.geometry.y*scale;
        }
#if WLR_HAS_XWAYLAND
        else if (window->xwayland_surface) {
            popup_x=(double)window->xwayland_surface->x*scale;
            popup_y=(double)window->xwayland_surface->y*scale;
        }
#endif
        if(!isfinite(popup_x)||!isfinite(popup_y)||popup_x<G_MININT32||popup_x>G_MAXINT32||
            popup_y<G_MININT32||popup_y>G_MAXINT32)return;
        left=(gint64)parent->layout_frame.left+(gint32)lround(popup_x);
        top=(gint64)parent->layout_frame.top+(gint32)lround(popup_y);
    }else if(!runtime_window_is_toplevel(window))return;
    if(window->native_system_window && !window->manual_task_position_valid){
        const gint64 display_width=window->runtime->display_info.display.width;
        const gint64 display_height=window->runtime->display_info.display.height;
        const gint64 max_left=MAX((gint64)0,display_width-w);
        const gint64 max_top=MAX((gint64)0,display_height-h);
        left=MIN(MAX(left,(gint64)0),max_left);
        top=MIN(MAX(top,(gint64)0),max_top);
    }
    const gint64 right=left+w;
    const gint64 bottom=top+h;
    if(left<INT32_MIN || left>INT32_MAX || top<INT32_MIN || top>INT32_MAX ||
        right<INT32_MIN || right>INT32_MAX || bottom<INT32_MIN || bottom>INT32_MAX)return;
    const AncoWmsRect frame={(gint32)left,(gint32)top,(gint32)right,(gint32)bottom};
#if WLR_HAS_XWAYLAND
    if(window->native_system_window && window->xwayland_surface &&
       same_task_toplevel_parent(window) && parent && parent->layout_attempted &&
       parent->xwayland_surface){
        /* Xwayland still performs X11 pointer routing in its root coordinate
         * space.  The original AnCo Window carries this transient at `frame`,
         * so retain the same window-to-window placement in X coordinates too.
         * Without this, managed transient X Windows remain stacked at 0,0
         * while Rosen displays them elsewhere and X events hit the parent. */
        const double x=(left-parent->layout_frame.left)/scale+
            parent->xwayland_surface->x;
        const double y=(top-parent->layout_frame.top)/scale+
            parent->xwayland_surface->y;
        if(isfinite(x)&&isfinite(y)&&x>=INT16_MIN&&x<=INT16_MAX&&
           y>=INT16_MIN&&y<=INT16_MAX){
            const gint32 x11_x=(gint32)lround(x),x11_y=(gint32)lround(y);
            if(window->xwayland_surface->x!=x11_x ||
               window->xwayland_surface->y!=x11_y){
                wlr_xwayland_surface_configure(window->xwayland_surface,
                    (int16_t)x11_x,(int16_t)x11_y,
                    window->xwayland_surface->width,
                    window->xwayland_surface->height);
                printf("RUNTIME_XWAYLAND_TRANSIENT_GEOMETRY window=%d xid=%u x=%d y=%d parent=%d standard_x_root_coordinates=1\n",
                    window->identity.window_id,
                    window->xwayland_surface->window_id,x11_x,x11_y,
                    parent->identity.window_id);
            }
        }
    }
#endif
    if(same_task_toplevel_parent(window) && window->manual_task_position_valid){
        const gint64 task_x=left-task_bounds.left,task_y=top-task_bounds.top;
        if(task_x<INT32_MIN||task_x>INT32_MAX||task_y<INT32_MIN||task_y>INT32_MAX)return;
        window->manual_task_x=(gint32)task_x;
        window->manual_task_y=(gint32)task_y;
    }
    const AncoWmsRect insets={(gint32)margins[0],(gint32)margins[1],
        (gint32)margins[2],(gint32)margins[3]};
    if(!force&&window->layout_attempted&&
        !memcmp(&frame,&window->layout_frame,sizeof(frame))&&
        !memcmp(&insets,&window->layout_insets,sizeof(insets)))return;
    /* Original WindowManager.LayoutParams constructor: brightness/orientation
     * -1, other scalar defaults0, systemBars7, Side.all15. The frame is global;
     * the original projector supplies Task-local position and direct attached
     * application-window/popup parent linkage. */
    const AncoWmsLayout attrs = {.type = window->original_type,
        .screen_brightness = -1, .screen_orientation = -1,
        .fit_insets_types = 7, .fit_insets_sides = 15};
    const AncoWmsActivityLayoutState activity = {
        .client_visible = surface->mapped, .visible_requested = surface->mapped};
    /* fromLp subtracts Task bounds for every has_task Window. The original
     * system task has no application Task offset, so its native system window
     * receives the already-global Wayland frame against a zero-origin screen
     * coordinate space. */
    const AncoWmsRect native_task_bounds=window->native_system_window ?
        (AncoWmsRect){0,0,window->runtime->display_info.display.width,
            window->runtime->display_info.display.height} : task_bounds;
    const AncoWmsLayoutState geometry = {.anco_enabled = TRUE,
        .has_frame = TRUE, .window_type = window->original_type,
        .frame = frame, .surface_insets = insets,
        .has_parent = (window->original_type==1000 ||
            window->original_type==1003) && host_parent,
        .parent_window_id = host_parent ? host_parent->identity.window_id : -1,
        .parent_global_scale = 1.0f, .has_task = TRUE,
        .task_bounds = native_task_bounds,
        .activities = &activity, .activity_count = 1};
    const AncoWmsCreateState state = {.anco_enabled = TRUE,
        .window = window->identity.window_id, .type = window->original_type,
        .has_task = TRUE,
        .task = window->native_system_window ? -1 :
            window->identity.owner.task_id,
        .has_activity = TRUE, .activity = window->identity.owner.activity_id,
        .has_parent = (window->original_type==1000 ||
            window->original_type==1003) && host_parent,
        .parent = host_parent ? host_parent->identity.window_id : -1};
    AncoWmsSendResult result;
    const gboolean delivered = anco_wms_broker_update_layout(window->runtime->wms,
        &state,&attrs,&geometry,&result);
    /* Record this actual layout attempt, not an ack, avoiding frame-driven
     * repeated identical requests. Original reply remains independently logged. */
    window->layout_frame=frame;window->layout_insets=insets;window->layout_attempted=TRUE;
    printf("RUNTIME_ORIGINAL_WINDOW_LAYOUT window=%d role=%d parent=%d host_parent=%d frame=%d,%d,%d,%d logical=%dx%d buffer_scale=%d native=%dx%d insets=%d,%d,%d,%d task_source=%s delivered=%d local=%d transport=%d decoded=%d service=%d\n",
        window->identity.window_id,runtime_window_is_popup(window)?2:1,
        parent?parent->identity.window_id:-1,
        host_parent?host_parent->identity.window_id:-1,
        frame.left,frame.top,frame.right,frame.bottom,
        surface->current.width,surface->current.height,
        surface->current.scale,w,h,insets.left,insets.top,insets.right,insets.bottom,
        task->host_bounds_valid?"wms-callback":"original-client-window",
        delivered,result.last.local_error,
        result.last.transport,result.last.decoded,result.last.service_result);
    if(surface->mapped && window->wms_create_entered &&
       !window->wms_show_entered){
        AncoWmsSendResult visibility;
        window->wms_show_entered=TRUE;
        const gboolean shown=anco_wms_broker_set_visibility(
            window->runtime->wms,&state,TRUE,0,&visibility);
        printf("RUNTIME_WMS_SHOW window=%d activity=%d task=%d reason=0 delivered=%d local=%d transport=%d decoded=%d service=%d\n",
            state.window,state.activity,state.task,shown,
            visibility.last.local_error,visibility.last.transport,
            visibility.last.decoded,visibility.last.service_result);
        /* The original task-focus callback can precede CreateWindow/SHOW.  At
         * that point the Wayland role is deliberately not eligible for focus.
         * Once original WMS has made the window visible, replay the existing
         * authoritative Task stack into the ordinary Wayland activation path. */
        if(shown)recompute_window_focus(window->runtime);
    }
    /* The real Linux first commit can arrive before the original AnCo Task has
     * completed CreateWindow/UPDATE_LAYOUT.  submit_pending() retained that
     * exact wl_buffer and returned without acknowledging it.  This real WMS
     * layout is the missing host-side readiness event: retry the existing
     * queue path now, without creating a Task, buffer, frame or protocol. */
    if(window->pending_buffer && !window->pending_layer_valid){
        printf("RUNTIME_PENDING_LAYER_RESUME window=%d commit=%u source=original-wms-layout same_client_commit=1\n",
            window->identity.window_id,window->pending_sequence);
        schedule_submit(window);
    }
}

static void update_original_window_layout(RuntimeWindow *window,
    const AncoWaylandGeometry *logical)
{
    update_original_window_layout_force(window,logical,FALSE);
}

static void toplevel_title_changed(struct wl_listener *listener,void *data)
{
    (void)data;
    RuntimeWindow *window=wl_container_of(listener,window,set_title);
    if(!root_native_title_owner(window) || publish_native_metadata(window))return;
    /* Android WindowManager relayout carries mutable window metadata along the
     * unchanged WMS path. Re-enter that same existing call with the retained
     * real committed geometry so the owning frontend applies the standard
     * WindowSessionImpl::SetAPPWindowLabel on its UI thread. */
    if(window->submitted_geometry_valid)
        update_original_window_layout_force(window,&window->submitted_geometry,TRUE);
    else if(window->pending_buffer)
        update_original_window_layout_force(window,&window->pending_geometry,TRUE);
}

#if WLR_HAS_XWAYLAND
static void xwayland_decoration_changed(RuntimeWindow *window)
{
    if(!root_native_title_owner(window) || publish_native_metadata(window))return;
    printf("RUNTIME_XWAYLAND_DECORATION window=%d xid=%u decor_visible=%d decorations=%u dynamic=1\n",
        window->identity.window_id,window->xwayland_surface->window_id,
        runtime_window_decor_visible(window),window->xwayland_surface->decorations);
    if(window->submitted_geometry_valid)
        update_original_window_layout_force(window,&window->submitted_geometry,TRUE);
    else if(window->pending_buffer)
        update_original_window_layout_force(window,&window->pending_geometry,TRUE);
}

static void xwayland_window_type_changed(struct wl_listener *listener,void *data)
{
    (void)data;
    RuntimeWindow *window=wl_container_of(listener,window,set_window_type);
    xwayland_decoration_changed(window);
}

static void xwayland_decorations_changed(struct wl_listener *listener,void *data)
{
    (void)data;
    RuntimeWindow *window=wl_container_of(listener,window,set_decorations);
    xwayland_decoration_changed(window);
}

static void xwayland_request_move(struct wl_listener *listener,void *data)
{
    (void)data;
    RuntimeWindow *window=wl_container_of(listener,window,request_move);
    (void)request_native_move(window,"x11");
}

static void xwayland_request_maximize(struct wl_listener *listener,void *data)
{
    (void)data;
    RuntimeWindow *window=wl_container_of(listener,window,request_maximize);
    const gboolean maximize=window->xwayland_surface->maximized_horz ||
        window->xwayland_surface->maximized_vert;
    (void)request_native_window(window,maximize?NATIVE_WINDOW_REQUEST_MAXIMIZE:
        NATIVE_WINDOW_REQUEST_RECOVER,"x11-maximize");
}

static void xwayland_request_minimize(struct wl_listener *listener,void *data)
{
    RuntimeWindow *window=wl_container_of(listener,window,request_minimize);
    const struct wlr_xwayland_minimize_event *event=data;
    (void)request_native_window(window,event&&event->minimize?
        NATIVE_WINDOW_REQUEST_MINIMIZE:NATIVE_WINDOW_REQUEST_RECOVER,
        "x11-minimize");
}

static void xwayland_request_fullscreen(struct wl_listener *listener,void *data)
{
    (void)data;
    RuntimeWindow *window=wl_container_of(listener,window,request_fullscreen);
    (void)request_native_window(window,window->xwayland_surface->fullscreen?
        NATIVE_WINDOW_REQUEST_FULLSCREEN:NATIVE_WINDOW_REQUEST_EXIT_FULLSCREEN,
        "x11-fullscreen");
}

static gboolean xwayland_begin_native_resize(RuntimeWindow *window,guint32 edges)
{
    Runtime *runtime=window->runtime;
    RuntimeTask *task=live_task_state(runtime,window->identity.owner.task_id);
    struct wlr_xwayland_surface *surface=window->xwayland_surface;
    const gboolean edges_valid=edges && !(edges&~(WLR_EDGE_TOP|WLR_EDGE_BOTTOM|
        WLR_EDGE_LEFT|WLR_EDGE_RIGHT)) &&
        !((edges&WLR_EDGE_TOP)&&(edges&WLR_EDGE_BOTTOM)) &&
        !((edges&WLR_EDGE_LEFT)&&(edges&WLR_EDGE_RIGHT));
    const gint64 logical_width=surface?surface->width:0;
    const gint64 logical_height=surface?surface->height:0;
    if(!root_native_title_owner(window) || !edges_valid || !task ||
       !task->client_area_valid || task->client_status!=4 ||
       !isfinite(runtime_window_content_scale(window)) ||
       runtime_window_content_scale(window)<=0 ||
       !runtime->input_pointer_position_valid ||
       runtime->input_pointer_window!=window || runtime->interactive_move_window ||
       runtime->interactive_resize_window || logical_width<1 || logical_height<1)
        return FALSE;
    const gint64 outer_width=(gint64)task->client_outer.right-task->client_outer.left;
    const gint64 outer_height=(gint64)task->client_outer.bottom-task->client_outer.top;
    const double scale=runtime_window_content_scale(window);
    const gint64 frame_width=outer_width-lround(logical_width*scale);
    const gint64 frame_height=outer_height-lround(logical_height*scale);
    if(frame_width<0 || frame_height<0 || frame_width>INT32_MAX ||
       frame_height>INT32_MAX)return FALSE;
    runtime->interactive_resize_window=window;
    runtime->interactive_resize_server_owned=FALSE;
    runtime->interactive_resize_native_root=TRUE;
    runtime->interactive_resize_start_x=runtime->input_pointer_display_x;
    runtime->interactive_resize_start_y=runtime->input_pointer_display_y;
    runtime->interactive_resize_start_width=(gint32)logical_width;
    runtime->interactive_resize_start_height=(gint32)logical_height;
    runtime->interactive_resize_width=(gint32)logical_width;
    runtime->interactive_resize_height=(gint32)logical_height;
    runtime->interactive_resize_edges=edges;
    runtime->interactive_resize_start_outer=task->client_outer;
    runtime->interactive_resize_frame_width=(gint32)frame_width;
    runtime->interactive_resize_frame_height=(gint32)frame_height;
    printf("RUNTIME_XWAYLAND_NATIVE_RESIZE_BEGIN window=%d xid=%u edges=%u display_xy=%.0f,%.0f logical=%dx%d outer=%d,%d,%d,%d frame=%d,%d\n",
        window->identity.window_id,surface->window_id,edges,
        runtime->interactive_resize_start_x,runtime->interactive_resize_start_y,
        (gint32)logical_width,(gint32)logical_height,task->client_outer.left,
        task->client_outer.top,task->client_outer.right,task->client_outer.bottom,
        (gint32)frame_width,(gint32)frame_height);
    return TRUE;
}

static void xwayland_request_resize(struct wl_listener *listener,void *data)
{
    RuntimeWindow *window=wl_container_of(listener,window,request_resize);
    const struct wlr_xwayland_resize_event *event=data;
    const gboolean accepted=event&&xwayland_begin_native_resize(window,event->edges);
    printf("RUNTIME_XWAYLAND_RESIZE_REQUEST window=%d xid=%u edges=%u accepted=%d\n",
        window->identity.window_id,window->xwayland_surface->window_id,
        event?event->edges:0,accepted);
}

static void xwayland_request_configure(struct wl_listener *listener,void *data)
{
    RuntimeWindow *window=wl_container_of(listener,window,request_configure);
    const struct wlr_xwayland_surface_configure_event *event=data;
    RuntimeTask *task=live_task_state(window->runtime,window->identity.owner.task_id);
    struct wlr_xwayland_surface *surface=window->xwayland_surface;
    gboolean accepted=FALSE;
    if(event && root_native_title_owner(window) && task && task->client_area_valid &&
       task->client_status==4 && isfinite(task->client_vpr) && task->client_vpr>0){
        const double vpr=runtime_window_content_scale(window);
        const gint64 outer_width=(gint64)task->client_outer.right-task->client_outer.left;
        const gint64 outer_height=(gint64)task->client_outer.bottom-task->client_outer.top;
        const gint64 frame_width=outer_width-lround(surface->width*vpr);
        const gint64 frame_height=outer_height-lround(surface->height*vpr);
        const gint64 offset_x=(gint64)task->client_outer.left-lround(surface->x*vpr);
        const gint64 offset_y=(gint64)task->client_outer.top-lround(surface->y*vpr);
        AncoWmsRect target=task->client_outer;
        if(event->mask&XCB_CONFIG_WINDOW_X){
            const gint64 left=lround(event->x*vpr)+offset_x;
            const gint64 width=(gint64)target.right-target.left;
            if(left>=INT32_MIN && left<=INT32_MAX && left+width<=INT32_MAX){
                target.left=(gint32)left;target.right=(gint32)(left+width);
            }
        }
        if(event->mask&XCB_CONFIG_WINDOW_Y){
            const gint64 top=lround(event->y*vpr)+offset_y;
            const gint64 height=(gint64)target.bottom-target.top;
            if(top>=INT32_MIN && top<=INT32_MAX && top+height<=INT32_MAX){
                target.top=(gint32)top;target.bottom=(gint32)(top+height);
            }
        }
        if((event->mask&XCB_CONFIG_WINDOW_WIDTH) && frame_width>=0){
            const gint64 width=lround(event->width*vpr)+frame_width;
            if(width>0 && width<=16384)target.right=(gint32)((gint64)target.left+width);
        }
        if((event->mask&XCB_CONFIG_WINDOW_HEIGHT) && frame_height>=0){
            const gint64 height=lround(event->height*vpr)+frame_height;
            if(height>0 && height<=16384)target.bottom=(gint32)((gint64)target.top+height);
        }
        accepted=request_native_resize(window,&target);
    }
    if(!accepted)
        wlr_xwayland_surface_configure(surface,surface->x,surface->y,
            surface->width,surface->height);
    printf("RUNTIME_XWAYLAND_CONFIGURE_REQUEST window=%d xid=%u mask=%u requested=%d,%d,%u,%u native_accepted=%d\n",
        window->identity.window_id,surface->window_id,event?event->mask:0,
        event?event->x:0,event?event->y:0,event?event->width:0,
        event?event->height:0,accepted);
}

static gboolean xwayland_apply_retained_size_hints(RuntimeWindow *window,
    const char *reason)
{
    struct wlr_xwayland_surface *surface=window->xwayland_surface;
    const xcb_size_hints_t *hints=surface?surface->size_hints:NULL;
    if(!window->native_system_window || !surface || !hints)return FALSE;
    gint32 width=surface->width,height=surface->height;
    if((hints->flags&(XCB_ICCCM_SIZE_HINT_US_SIZE|
            XCB_ICCCM_SIZE_HINT_P_SIZE)) &&
       hints->width>0 && hints->height>0){
        width=hints->width;height=hints->height;
    }
    if(hints->flags&XCB_ICCCM_SIZE_HINT_P_MIN_SIZE){
        width=MAX(width,hints->min_width);height=MAX(height,hints->min_height);
    }
    if(hints->flags&XCB_ICCCM_SIZE_HINT_P_MAX_SIZE){
        if(hints->max_width>0)width=MIN(width,hints->max_width);
        if(hints->max_height>0)height=MIN(height,hints->max_height);
    }
    if(width<1 || height<1 || width>UINT16_MAX || height>UINT16_MAX ||
       (width==surface->width && height==surface->height))return FALSE;
    const gint32 old_width=surface->width,old_height=surface->height;
    wlr_xwayland_surface_configure(surface,surface->x,surface->y,
        (uint16_t)width,(uint16_t)height);
    printf("RUNTIME_XWAYLAND_SIZE_HINT_CONFIGURE window=%d xid=%u old=%dx%d target=%dx%d flags=%u reason=%s standard_icccm=1\n",
        window->identity.window_id,surface->window_id,old_width,old_height,
        width,height,hints->flags,reason?reason:"unspecified");
    return TRUE;
}

static void xwayland_hints_changed(struct wl_listener *listener,void *data)
{
    (void)data;
    RuntimeWindow *window=wl_container_of(listener,window,set_hints);
    (void)publish_native_metadata(window);
    (void)xwayland_apply_retained_size_hints(window,"set-hints");
}

static void xwayland_request_activate(struct wl_listener *listener,void *data)
{
    (void)data;
    RuntimeWindow *window=wl_container_of(listener,window,request_activate);
    (void)request_native_window(window,NATIVE_WINDOW_REQUEST_FOCUS,"x11-activate");
}
#endif

static void count_mapped_surface(struct wlr_surface *surface,int sx,int sy,void *data)
{
    (void)surface;(void)sx;(void)sy;
    ++*(guint *)data;
}
static int committed_original_layer(RuntimeWindow *window,const AncoWaylandGeometry *geometry,
    struct wlr_buffer *buffer,const AncoWmsRect *decoration,AncoGraphicLayer *out)
{
    struct wlr_surface *surface=runtime_window_surface(window);
    const RuntimeTask *task=live_task_state(window->runtime,window->identity.owner.task_id);
    AncoWmsRect task_bounds;gint32 task_mode;float task_scale;
    if(!effective_task_geometry(task,&task_bounds,&task_mode,&task_scale) ||
       !window->layout_attempted)return -EAGAIN;
    guint surfaces=0;
    wlr_surface_for_each_surface(surface,count_mapped_surface,&surfaces);
    if(surfaces!=1 || surface->current.transform!=WL_OUTPUT_TRANSFORM_NORMAL)return -ENOTSUP;
    AncoShmDecoration physical={0};
    const int decoration_status=physical_server_decoration(window,buffer,decoration,&physical);
    if(decoration_status)return decoration_status;
    struct wlr_fbox crop;
    wlr_surface_get_buffer_source_box(surface,&crop);
    const double sources[]={crop.x,crop.y,crop.x+crop.width,crop.y+crop.height};
    const gint64 edges[]={-(gint64)geometry->geometry.left,-(gint64)geometry->geometry.top,
        (gint64)surface->current.width-geometry->geometry.left,
        (gint64)surface->current.height-geometry->geometry.top};
    AncoGraphicLayer layer={.window=(guint32)window->identity.window_id,
        /* Original setSurfaceViewID ignores low32==0. This ordinary root's
         * SF identity remains0; native producer still uses its real64bit key. */
        .surface_view=(guint32)window->surface_key.second?window->surface_key.second:0,
        .scale=-1.0f,.composition=2};
    if(rect_has_margins(decoration)){
        const guint64 crop_width=(guint64)buffer->width+physical.left+physical.right;
        const guint64 crop_height=(guint64)buffer->height+physical.top+physical.bottom;
        const double scale=runtime_window_content_scale(window);
        const gint64 native_left=lround(decoration->left*scale);
        const gint64 native_top=lround(decoration->top*scale);
        const gint64 native_right=lround(decoration->right*scale);
        const gint64 native_bottom=lround(decoration->bottom*scale);
        const gint64 client_width=(gint64)window->layout_frame.right-window->layout_frame.left;
        const gint64 client_height=(gint64)window->layout_frame.bottom-window->layout_frame.top;
        const gint64 origin_x=(gint64)window->layout_frame.left-task_bounds.left-native_left;
        const gint64 origin_y=(gint64)window->layout_frame.top-task_bounds.top-native_top;
        const gint64 frame_width=client_width+native_left+native_right;
        const gint64 frame_height=client_height+native_top+native_bottom;
        if(crop_width>INT32_MAX||crop_height>INT32_MAX||origin_x<INT32_MIN||
            origin_x>INT32_MAX||origin_y<INT32_MIN||origin_y>INT32_MAX||
            frame_width<1||frame_width>INT32_MAX||frame_height<1||frame_height>INT32_MAX||
            origin_x+frame_width>INT32_MAX||origin_y+frame_height>INT32_MAX)return -ERANGE;
        layer.source_crop[0]=0;layer.source_crop[1]=0;
        layer.source_crop[2]=(gint32)crop_width;layer.source_crop[3]=(gint32)crop_height;
        layer.display_frame[0]=(gint32)origin_x;layer.display_frame[1]=(gint32)origin_y;
        layer.display_frame[2]=(gint32)(origin_x+frame_width);
        layer.display_frame[3]=(gint32)(origin_y+frame_height);
        const pixman_box32_t whole={0,0,surface->current.width,surface->current.height};
        layer.opaque=pixman_region32_contains_rectangle(&surface->opaque_region,&whole)==
            PIXMAN_REGION_IN;
        *out=layer;
        return 0;
    }
    for(guint i=0;i<4;++i){
        /* Original source crop FCVTZS operates on float32. The destination is
         * this Window's own surface extent in Task-local native coordinates. */
        const float source=(float)sources[i];
        const gint64 origin=i%2 ?
            (gint64)window->layout_frame.top-task_bounds.top :
            (gint64)window->layout_frame.left-task_bounds.left;
        const double dest=origin+edges[i]*runtime_window_content_scale(window);
        if(!isfinite(source)||source<INT32_MIN||source>=2147483648.0f ||
            !isfinite(dest)||dest<INT32_MIN||dest>INT32_MAX)return -ERANGE;
        layer.source_crop[i]=(gint32)source;
        layer.display_frame[i]=(gint32)lround(dest);
    }
    if(layer.source_crop[2]<=layer.source_crop[0]||layer.source_crop[3]<=layer.source_crop[1]||
        layer.display_frame[2]<=layer.display_frame[0]||layer.display_frame[3]<=layer.display_frame[1])
        return -ERANGE;
    const pixman_box32_t whole={0,0,surface->current.width,surface->current.height};
    /* Actual committed wl_surface opacity, with no Linux opacity effect or
     * rounded-corner decoration applied. Host retains its own original policy.
     * Ordinary application surfaces aren't Android snapshot-window layers. */
    layer.opaque=pixman_region32_contains_rectangle(&surface->opaque_region,&whole)==PIXMAN_REGION_IN;
    *out=layer;
    return 0;
}

static gboolean layer_can_translate(const AncoGraphicLayer *layer,gint64 dx,gint64 dy)
{
    for(guint i=0;i<4;++i){
        const gint64 delta=i%2?dy:dx;
        const gint64 value=(gint64)layer->display_frame[i]+delta;
        if(value<INT32_MIN || value>INT32_MAX)return FALSE;
    }
    return TRUE;
}

static void layer_translate(AncoGraphicLayer *layer,gint64 dx,gint64 dy)
{
    layer->display_frame[0]=(gint32)((gint64)layer->display_frame[0]+dx);
    layer->display_frame[2]=(gint32)((gint64)layer->display_frame[2]+dx);
    layer->display_frame[1]=(gint32)((gint64)layer->display_frame[1]+dy);
    layer->display_frame[3]=(gint32)((gint64)layer->display_frame[3]+dy);
}

static void interactive_move_finish(Runtime *runtime,const char *reason)
{
    RuntimeWindow *window=runtime->interactive_move_window;
    if(!window)return;
    runtime->interactive_move_window=NULL;
    runtime->interactive_move_server_owned=FALSE;
    printf("RUNTIME_STANDARD_MOVE_END window=%d reason=%s final_frame=%d,%d,%d,%d\n",
        window->identity.window_id,reason,
        window->layout_frame.left,window->layout_frame.top,
        window->layout_frame.right,window->layout_frame.bottom);
}

static gboolean server_decoration_begin_move(RuntimeWindow *window)
{
    Runtime *runtime=window->runtime;
    RuntimeTask *task=live_task_state(runtime,window->identity.owner.task_id);
    AncoWmsRect bounds;gint32 mode;float host_scale;
    if(window->retiring||!runtime_window_surface(window)->mapped||!window->layout_attempted||
       !window->submitted_geometry_valid||!window->submitted_layer_valid||
       !runtime->input_pointer_position_valid||runtime->interactive_move_window||
       runtime->interactive_resize_window||
       !effective_task_geometry(task,&bounds,&mode,&host_scale))return FALSE;
    const gint64 task_x=(gint64)window->layout_frame.left-bounds.left;
    const gint64 task_y=(gint64)window->layout_frame.top-bounds.top;
    if(task_x<INT32_MIN||task_x>INT32_MAX||task_y<INT32_MIN||task_y>INT32_MAX)
        return FALSE;
    window->manual_task_position_valid=TRUE;
    window->manual_task_x=(gint32)task_x;window->manual_task_y=(gint32)task_y;
    runtime->interactive_move_window=window;
    runtime->interactive_move_server_owned=TRUE;
    runtime->interactive_move_start_x=runtime->input_pointer_display_x;
    runtime->interactive_move_start_y=runtime->input_pointer_display_y;
    runtime->interactive_move_start_task_x=window->manual_task_x;
    runtime->interactive_move_start_task_y=window->manual_task_y;
    printf("RUNTIME_SERVER_DECORATION_MOVE_BEGIN window=%d display_xy=%.0f,%.0f task_xy=%d,%d original_update_layout=1\n",
        window->identity.window_id,runtime->interactive_move_start_x,
        runtime->interactive_move_start_y,window->manual_task_x,window->manual_task_y);
    return TRUE;
}

static gboolean interactive_move_update(Runtime *runtime,double display_x,double display_y)
{
    RuntimeWindow *window=runtime->interactive_move_window;
    if(!window || window->retiring || !runtime_window_surface(window)->mapped ||
        !window->layout_attempted || !window->submitted_layer_valid)return FALSE;
    const double delta_x=display_x-runtime->interactive_move_start_x;
    const double delta_y=display_y-runtime->interactive_move_start_y;
    if(!isfinite(delta_x)||!isfinite(delta_y) ||
        delta_x<LONG_MIN||delta_x>LONG_MAX || delta_y<LONG_MIN||delta_y>LONG_MAX)
        return FALSE;
    const long rounded_x=lround(delta_x),rounded_y=lround(delta_y);
    const gint64 task_x=(gint64)runtime->interactive_move_start_task_x+rounded_x;
    const gint64 task_y=(gint64)runtime->interactive_move_start_task_y+rounded_y;
    if(task_x<INT32_MIN||task_x>INT32_MAX||task_y<INT32_MIN||task_y>INT32_MAX)
        return FALSE;
    const AncoWmsRect old=window->layout_frame;
    const gint32 old_task_x=window->manual_task_x;
    const gint32 old_task_y=window->manual_task_y;
    const gint64 prospective_dx=task_x-window->manual_task_x;
    const gint64 prospective_dy=task_y-window->manual_task_y;
    if((window->submitted_layer_valid && !layer_can_translate(&window->submitted_layer,
            prospective_dx,prospective_dy)) ||
       (window->pending_layer_valid && !layer_can_translate(&window->pending_layer,
            prospective_dx,prospective_dy)))return FALSE;
    window->manual_task_position_valid=TRUE;
    window->manual_task_x=(gint32)task_x;
    window->manual_task_y=(gint32)task_y;
    update_original_window_layout(window,&window->submitted_geometry);
    const gint64 moved_x=(gint64)window->layout_frame.left-old.left;
    const gint64 moved_y=(gint64)window->layout_frame.top-old.top;
    if(!moved_x&&!moved_y){
        window->manual_task_x=old_task_x;
        window->manual_task_y=old_task_y;
        return FALSE;
    }
    if(window->submitted_layer_valid)
        layer_translate(&window->submitted_layer,moved_x,moved_y);
    if(window->pending_layer_valid)
        layer_translate(&window->pending_layer,moved_x,moved_y);
    schedule_layer_scene(runtime);
    return TRUE;
}

static void toplevel_request_move(struct wl_listener *listener,void *data)
{
    RuntimeWindow *window=wl_container_of(listener,window,request_move);
    Runtime *runtime=window->runtime;
    const struct wlr_xdg_toplevel_move_event *event=data;
    struct wlr_surface *focused=runtime->seat?
        runtime->seat->pointer_state.focused_surface:NULL;
    const gboolean transient=same_task_toplevel_parent(window);
    const gboolean same_client=event && event->seat && runtime->seat &&
        event->seat==runtime->seat->pointer_state.focused_client;
    const gboolean same_root=focused &&
        wlr_surface_get_root_surface(focused)==runtime_window_surface(window);
    const gboolean serial_valid=same_client && same_root &&
        wlr_seat_validate_pointer_grab_serial(runtime->seat,NULL,event->serial);
    const gboolean native_root=root_native_title_owner(window);
    if(native_root){
        const gboolean available=!window->retiring &&
            runtime_window_surface(window)->mapped &&
            runtime->input_pointer_position_valid &&
            runtime->input_pointer_window==window &&
            !runtime->interactive_move_window &&
            !runtime->interactive_resize_window &&
            !wlr_seat_pointer_has_grab(runtime->seat);
        const gboolean requested=serial_valid && available &&
            request_native_move(window,"xdg");
        printf("RUNTIME_STANDARD_NATIVE_MOVE window=%d serial=%u same_client=%d same_root=%d serial_valid=%d available=%d requested=%d\n",
            window->identity.window_id,event?event->serial:0,same_client,
            same_root,serial_valid,available,requested);
        return;
    }
    const gboolean available=transient && !window->retiring &&
        runtime_window_surface(window)->mapped && window->layout_attempted &&
        window->submitted_geometry_valid && window->submitted_layer_valid &&
        runtime->input_pointer_position_valid && runtime->input_pointer_window==window &&
        !runtime->interactive_move_window && !runtime->interactive_resize_window &&
        !wlr_seat_pointer_has_grab(runtime->seat);
    RuntimeTask *task=live_task_state(runtime,window->identity.owner.task_id);
    AncoWmsRect bounds;gint32 mode;float host_scale;
    const gboolean task_ready=effective_task_geometry(task,&bounds,&mode,&host_scale);
    if(!serial_valid || !available || !task_ready){
        printf("RUNTIME_STANDARD_MOVE_REJECT window=%d transient=%d same_client=%d same_root=%d serial=%d available=%d task_ready=%d\n",
            window->identity.window_id,transient,same_client,same_root,serial_valid,
            available,task_ready);
        return;
    }
    const gint64 task_x=(gint64)window->layout_frame.left-bounds.left;
    const gint64 task_y=(gint64)window->layout_frame.top-bounds.top;
    if(task_x<INT32_MIN||task_x>INT32_MAX||task_y<INT32_MIN||task_y>INT32_MAX)return;
    window->manual_task_position_valid=TRUE;
    window->manual_task_x=(gint32)task_x;
    window->manual_task_y=(gint32)task_y;
    runtime->interactive_move_window=window;
    runtime->interactive_move_server_owned=FALSE;
    runtime->interactive_move_start_x=runtime->input_pointer_display_x;
    runtime->interactive_move_start_y=runtime->input_pointer_display_y;
    runtime->interactive_move_start_task_x=window->manual_task_x;
    runtime->interactive_move_start_task_y=window->manual_task_y;
    printf("RUNTIME_STANDARD_MOVE_BEGIN window=%d serial=%u display_xy=%.0f,%.0f task_xy=%d,%d original_update_layout=1\n",
        window->identity.window_id,event->serial,runtime->interactive_move_start_x,
        runtime->interactive_move_start_y,window->manual_task_x,window->manual_task_y);
}

static void toplevel_request_maximize(struct wl_listener *listener,void *data)
{
    (void)data;
    RuntimeWindow *window=wl_container_of(listener,window,request_maximize);
    const gboolean accepted=request_native_window(window,
        window->xdg->toplevel->requested.maximized?
        NATIVE_WINDOW_REQUEST_MAXIMIZE:NATIVE_WINDOW_REQUEST_RECOVER,
        "xdg-maximize");
    if(!accepted){wlr_xdg_surface_schedule_configure(window->xdg);
        if(window->runtime->wayland)anco_wayland_context_wake(window->runtime->wayland);}
}

static void toplevel_request_minimize(struct wl_listener *listener,void *data)
{
    (void)data;
    RuntimeWindow *window=wl_container_of(listener,window,request_minimize);
    const gboolean accepted=request_native_window(window,NATIVE_WINDOW_REQUEST_MINIMIZE,
        "xdg-minimize");
    if(!accepted){wlr_xdg_surface_schedule_configure(window->xdg);
        if(window->runtime->wayland)anco_wayland_context_wake(window->runtime->wayland);}
}

static void toplevel_request_fullscreen(struct wl_listener *listener,void *data)
{
    (void)data;
    RuntimeWindow *window=wl_container_of(listener,window,request_fullscreen);
    const gboolean accepted=request_native_window(window,
        window->xdg->toplevel->requested.fullscreen?
        NATIVE_WINDOW_REQUEST_FULLSCREEN:NATIVE_WINDOW_REQUEST_EXIT_FULLSCREEN,
        "xdg-fullscreen");
    if(!accepted){wlr_xdg_surface_schedule_configure(window->xdg);
        if(window->runtime->wayland)anco_wayland_context_wake(window->runtime->wayland);}
}

static gboolean interactive_resize_update(Runtime *runtime,double display_x,double display_y)
{
    RuntimeWindow *window=runtime->interactive_resize_window;
    if(!window || window->retiring || !runtime_window_surface(window)->mapped)return FALSE;
    const double scale=runtime_window_content_scale(window);
    const double dx=(display_x-runtime->interactive_resize_start_x)/scale;
    const double dy=(display_y-runtime->interactive_resize_start_y)/scale;
    if(!isfinite(dx)||!isfinite(dy)||dx<LONG_MIN||dx>LONG_MAX||
        dy<LONG_MIN||dy>LONG_MAX)return FALSE;
    gint64 width=runtime->interactive_resize_start_width;
    gint64 height=runtime->interactive_resize_start_height;
    const long rounded_x=lround(dx),rounded_y=lround(dy);
    if(runtime->interactive_resize_edges&WLR_EDGE_LEFT)width-=rounded_x;
    else if(runtime->interactive_resize_edges&WLR_EDGE_RIGHT)width+=rounded_x;
    if(runtime->interactive_resize_edges&WLR_EDGE_TOP)height-=rounded_y;
    else if(runtime->interactive_resize_edges&WLR_EDGE_BOTTOM)height+=rounded_y;
    gint32 min_width=1,min_height=1,max_width=INT32_MAX,max_height=INT32_MAX;
    struct wlr_xdg_toplevel *top=NULL;
    if(window->xdg){
        top=window->xdg->toplevel;
        min_width=MAX(top->current.min_width,1);
        min_height=MAX(top->current.min_height,1);
        max_width=top->current.max_width?top->current.max_width:INT32_MAX;
        max_height=top->current.max_height?top->current.max_height:INT32_MAX;
    }
#if WLR_HAS_XWAYLAND
    else if(window->xwayland_surface && window->xwayland_surface->size_hints){
        const xcb_size_hints_t *hints=window->xwayland_surface->size_hints;
        if(hints->flags&XCB_ICCCM_SIZE_HINT_P_MIN_SIZE){
            min_width=MAX(hints->min_width,1);min_height=MAX(hints->min_height,1);
        }
        if(hints->flags&XCB_ICCCM_SIZE_HINT_P_MAX_SIZE){
            if(hints->max_width>0)max_width=hints->max_width;
            if(hints->max_height>0)max_height=hints->max_height;
        }
    }
#endif
    width=CLAMP(width,min_width,max_width);height=CLAMP(height,min_height,max_height);
    if(width==runtime->interactive_resize_width &&
        height==runtime->interactive_resize_height)return FALSE;
    runtime->interactive_resize_width=(gint32)width;
    runtime->interactive_resize_height=(gint32)height;
    if(runtime->interactive_resize_native_root){
        RuntimeTask *task=live_task_state(runtime,window->identity.owner.task_id);
        const double content_scale=runtime_window_content_scale(window);
        if(!task || !task->client_area_valid || !isfinite(content_scale) ||
           content_scale<=0)return FALSE;
        const gint64 outer_width=lround(width*content_scale)+
            runtime->interactive_resize_frame_width;
        const gint64 outer_height=lround(height*content_scale)+
            runtime->interactive_resize_frame_height;
        if(outer_width<1 || outer_height<1 || outer_width>16384 ||
           outer_height>16384)return FALSE;
        const AncoWmsRect start=runtime->interactive_resize_start_outer;
        AncoWmsRect target={start.left,start.top,
            (gint32)((gint64)start.left+outer_width),
            (gint32)((gint64)start.top+outer_height)};
        if(runtime->interactive_resize_edges&WLR_EDGE_LEFT){
            target.left=(gint32)((gint64)start.right-outer_width);
            target.right=start.right;
        }
        if(runtime->interactive_resize_edges&WLR_EDGE_TOP){
            target.top=(gint32)((gint64)start.bottom-outer_height);
            target.bottom=start.bottom;
        }
        return request_native_resize(window,&target);
    }
    const guint32 size_serial=wlr_xdg_toplevel_set_size(top,(gint32)width,(gint32)height);
    const guint32 state_serial=wlr_xdg_toplevel_set_resizing(top,TRUE);
    printf("RUNTIME_STANDARD_RESIZE_CONFIGURE window=%d edges=%u logical=%dx%d serial=%u coalesced=%d real_buffer_required=1\n",
        window->identity.window_id,runtime->interactive_resize_edges,
        (gint32)width,(gint32)height,state_serial,size_serial==state_serial);
    return TRUE;
}

static void interactive_resize_finish(Runtime *runtime,gboolean final_configure,
    const char *reason)
{
    RuntimeWindow *window=runtime->interactive_resize_window;
    if(!window)return;
    runtime->interactive_resize_window=NULL;
    runtime->interactive_resize_server_owned=FALSE;
    const gboolean native_root=runtime->interactive_resize_native_root;
    runtime->interactive_resize_native_root=FALSE;
    guint32 serial=0;gboolean coalesced=FALSE;
    if(final_configure && !window->retiring && runtime_window_surface(window)->mapped){
        if(window->xdg){
            struct wlr_xdg_toplevel *top=window->xdg->toplevel;
            const guint32 size_serial=native_root?
                wlr_xdg_surface_schedule_configure(window->xdg):
                wlr_xdg_toplevel_set_size(top,runtime->interactive_resize_width,
                    runtime->interactive_resize_height);
            serial=wlr_xdg_toplevel_set_resizing(top,FALSE);
            coalesced=size_serial==serial;
            window->resize_finalize_serial=serial;
            if(runtime->wayland)anco_wayland_context_wake(runtime->wayland);
        }else window->resize_finalize_serial=0;
    }else{
        window->resize_anchor_valid=FALSE;
        window->resize_finalize_serial=0;
    }
    printf("RUNTIME_STANDARD_RESIZE_END window=%d reason=%s logical=%dx%d final_serial=%u coalesced=%d waits_for_real_commit=%d\n",
        window->identity.window_id,reason,runtime->interactive_resize_width,
        runtime->interactive_resize_height,serial,coalesced,serial!=0);
}

static gboolean server_decoration_begin_resize(RuntimeWindow *window,guint32 edges)
{
    Runtime *runtime=window->runtime;
    RuntimeTask *task=live_task_state(runtime,window->identity.owner.task_id);
    AncoWmsRect bounds;gint32 mode;float host_scale;
    const gboolean edges_valid=edges && !(edges&~(WLR_EDGE_TOP|WLR_EDGE_BOTTOM|
        WLR_EDGE_LEFT|WLR_EDGE_RIGHT)) &&
        !((edges&WLR_EDGE_TOP)&&(edges&WLR_EDGE_BOTTOM)) &&
        !((edges&WLR_EDGE_LEFT)&&(edges&WLR_EDGE_RIGHT));
    const gint64 logical_width=(gint64)window->submitted_geometry.geometry.right-
        window->submitted_geometry.geometry.left;
    const gint64 logical_height=(gint64)window->submitted_geometry.geometry.bottom-
        window->submitted_geometry.geometry.top;
    if(!edges_valid||window->retiring||!runtime_window_surface(window)->mapped||
       !window->layout_attempted||!window->submitted_geometry_valid||
       !window->submitted_layer_valid||!runtime->input_pointer_position_valid||
       runtime->interactive_move_window||runtime->interactive_resize_window||
       !effective_task_geometry(task,&bounds,&mode,&host_scale)||logical_width<1||
       logical_width>INT32_MAX||logical_height<1||logical_height>INT32_MAX)return FALSE;
    const gint64 task_x=(gint64)window->layout_frame.left-bounds.left;
    const gint64 task_y=(gint64)window->layout_frame.top-bounds.top;
    const gint64 task_right=(gint64)window->layout_frame.right-bounds.left;
    const gint64 task_bottom=(gint64)window->layout_frame.bottom-bounds.top;
    if(task_x<INT32_MIN||task_x>INT32_MAX||task_y<INT32_MIN||task_y>INT32_MAX||
       task_right<INT32_MIN||task_right>INT32_MAX||task_bottom<INT32_MIN||
       task_bottom>INT32_MAX)return FALSE;
    window->manual_task_position_valid=TRUE;
    window->manual_task_x=(gint32)task_x;window->manual_task_y=(gint32)task_y;
    window->resize_anchor_valid=TRUE;window->resize_anchor_edges=edges;
    window->resize_anchor_right_task=(gint32)task_right;
    window->resize_anchor_bottom_task=(gint32)task_bottom;
    window->resize_finalize_serial=0;
    runtime->interactive_resize_window=window;
    runtime->interactive_resize_server_owned=TRUE;
    runtime->interactive_resize_native_root=FALSE;
    runtime->interactive_resize_start_x=runtime->input_pointer_display_x;
    runtime->interactive_resize_start_y=runtime->input_pointer_display_y;
    runtime->interactive_resize_start_width=(gint32)logical_width;
    runtime->interactive_resize_start_height=(gint32)logical_height;
    runtime->interactive_resize_width=(gint32)logical_width;
    runtime->interactive_resize_height=(gint32)logical_height;
    runtime->interactive_resize_edges=edges;
    const guint32 size_serial=wlr_xdg_toplevel_set_size(window->xdg->toplevel,
        (gint32)logical_width,(gint32)logical_height);
    const guint32 state_serial=wlr_xdg_toplevel_set_resizing(window->xdg->toplevel,TRUE);
    printf("RUNTIME_SERVER_DECORATION_RESIZE_BEGIN window=%d edges=%u display_xy=%.0f,%.0f logical=%dx%d configure=%u coalesced=%d\n",
        window->identity.window_id,edges,runtime->interactive_resize_start_x,
        runtime->interactive_resize_start_y,(gint32)logical_width,
        (gint32)logical_height,state_serial,size_serial==state_serial);
    if(runtime->wayland)anco_wayland_context_wake(runtime->wayland);
    return TRUE;
}

static void toplevel_request_resize(struct wl_listener *listener,void *data)
{
    RuntimeWindow *window=wl_container_of(listener,window,request_resize);
    Runtime *runtime=window->runtime;
    const struct wlr_xdg_toplevel_resize_event *event=data;
    struct wlr_surface *focused=runtime->seat?
        runtime->seat->pointer_state.focused_surface:NULL;
    const gboolean transient=same_task_toplevel_parent(window);
    const gboolean same_client=event && event->seat && runtime->seat &&
        event->seat==runtime->seat->pointer_state.focused_client;
    const gboolean same_root=focused &&
        wlr_surface_get_root_surface(focused)==runtime_window_surface(window);
    const gboolean serial_valid=same_client && same_root &&
        wlr_seat_validate_pointer_grab_serial(runtime->seat,NULL,event->serial);
    const guint32 edges=event?event->edges:WLR_EDGE_NONE;
    const gboolean edges_valid=edges && !(edges&~(WLR_EDGE_TOP|WLR_EDGE_BOTTOM|
        WLR_EDGE_LEFT|WLR_EDGE_RIGHT)) &&
        !((edges&WLR_EDGE_TOP)&&(edges&WLR_EDGE_BOTTOM)) &&
        !((edges&WLR_EDGE_LEFT)&&(edges&WLR_EDGE_RIGHT));
    const gboolean native_root=root_native_title_owner(window);
    const gboolean available=(transient||native_root) && edges_valid && !window->retiring &&
        runtime_window_surface(window)->mapped && window->layout_attempted &&
        window->submitted_geometry_valid && window->submitted_layer_valid &&
        runtime->input_pointer_position_valid && runtime->input_pointer_window==window &&
        !runtime->interactive_move_window && !runtime->interactive_resize_window &&
        !wlr_seat_pointer_has_grab(runtime->seat);
    RuntimeTask *task=live_task_state(runtime,window->identity.owner.task_id);
    AncoWmsRect bounds;gint32 mode;float host_scale;
    const gboolean task_ready=effective_task_geometry(task,&bounds,&mode,&host_scale);
    const gint64 logical_width=(gint64)window->submitted_geometry.geometry.right-
        window->submitted_geometry.geometry.left;
    const gint64 logical_height=(gint64)window->submitted_geometry.geometry.bottom-
        window->submitted_geometry.geometry.top;
    const gboolean native_ready=!native_root || (task && task->client_area_valid &&
        task->client_status==4 && isfinite(task->client_vpr) && task->client_vpr>0);
    if(!serial_valid || !available || !task_ready || logical_width<1 ||
        logical_width>INT32_MAX || logical_height<1 || logical_height>INT32_MAX ||
        !native_ready){
        printf("RUNTIME_STANDARD_RESIZE_REJECT window=%d transient=%d native_root=%d edges=%u edges_valid=%d same_client=%d same_root=%d serial=%d available=%d task_ready=%d native_ready=%d\n",
            window->identity.window_id,transient,native_root,edges,edges_valid,
            same_client,same_root,serial_valid,available,task_ready,native_ready);
        return;
    }
    if(native_root){
        const gint64 outer_width=(gint64)task->client_outer.right-task->client_outer.left;
        const gint64 outer_height=(gint64)task->client_outer.bottom-task->client_outer.top;
        const double content_scale=runtime_window_content_scale(window);
        const gint64 content_width=lround(logical_width*content_scale);
        const gint64 content_height=lround(logical_height*content_scale);
        const gint64 frame_width=outer_width-content_width;
        const gint64 frame_height=outer_height-content_height;
        if(frame_width<0 || frame_height<0 || frame_width>INT32_MAX ||
           frame_height>INT32_MAX)return;
        runtime->interactive_resize_window=window;
        runtime->interactive_resize_server_owned=FALSE;
        runtime->interactive_resize_native_root=TRUE;
        runtime->interactive_resize_start_x=runtime->input_pointer_display_x;
        runtime->interactive_resize_start_y=runtime->input_pointer_display_y;
        runtime->interactive_resize_start_width=(gint32)logical_width;
        runtime->interactive_resize_start_height=(gint32)logical_height;
        runtime->interactive_resize_width=(gint32)logical_width;
        runtime->interactive_resize_height=(gint32)logical_height;
        runtime->interactive_resize_edges=edges;
        runtime->interactive_resize_start_outer=task->client_outer;
        runtime->interactive_resize_frame_width=(gint32)frame_width;
        runtime->interactive_resize_frame_height=(gint32)frame_height;
        const guint32 schedule_serial=wlr_xdg_surface_schedule_configure(window->xdg);
        const guint32 state_serial=wlr_xdg_toplevel_set_resizing(
            window->xdg->toplevel,TRUE);
        printf("RUNTIME_STANDARD_NATIVE_RESIZE_BEGIN window=%d serial=%u edges=%u display_xy=%.0f,%.0f logical=%dx%d outer=%d,%d,%d,%d frame=%d,%d configure=%u coalesced=%d\n",
            window->identity.window_id,event->serial,edges,
            runtime->interactive_resize_start_x,runtime->interactive_resize_start_y,
            (gint32)logical_width,(gint32)logical_height,
            task->client_outer.left,task->client_outer.top,
            task->client_outer.right,task->client_outer.bottom,
            (gint32)frame_width,(gint32)frame_height,state_serial,
            schedule_serial==state_serial);
        if(runtime->wayland)anco_wayland_context_wake(runtime->wayland);
        return;
    }
    const gint64 task_x=(gint64)window->layout_frame.left-bounds.left;
    const gint64 task_y=(gint64)window->layout_frame.top-bounds.top;
    const gint64 task_right=(gint64)window->layout_frame.right-bounds.left;
    const gint64 task_bottom=(gint64)window->layout_frame.bottom-bounds.top;
    if(task_x<INT32_MIN||task_x>INT32_MAX||task_y<INT32_MIN||task_y>INT32_MAX||
        task_right<INT32_MIN||task_right>INT32_MAX||
        task_bottom<INT32_MIN||task_bottom>INT32_MAX)return;
    window->manual_task_position_valid=TRUE;
    window->manual_task_x=(gint32)task_x;window->manual_task_y=(gint32)task_y;
    window->resize_anchor_valid=TRUE;window->resize_anchor_edges=edges;
    window->resize_anchor_right_task=(gint32)task_right;
    window->resize_anchor_bottom_task=(gint32)task_bottom;
    window->resize_finalize_serial=0;
    runtime->interactive_resize_window=window;
    runtime->interactive_resize_server_owned=FALSE;
    runtime->interactive_resize_native_root=FALSE;
    runtime->interactive_resize_start_x=runtime->input_pointer_display_x;
    runtime->interactive_resize_start_y=runtime->input_pointer_display_y;
    runtime->interactive_resize_start_width=(gint32)logical_width;
    runtime->interactive_resize_start_height=(gint32)logical_height;
    runtime->interactive_resize_width=(gint32)logical_width;
    runtime->interactive_resize_height=(gint32)logical_height;
    runtime->interactive_resize_edges=edges;
    const guint32 size_serial=wlr_xdg_toplevel_set_size(window->xdg->toplevel,
        (gint32)logical_width,(gint32)logical_height);
    const guint32 state_serial=wlr_xdg_toplevel_set_resizing(window->xdg->toplevel,TRUE);
    printf("RUNTIME_STANDARD_RESIZE_BEGIN window=%d serial=%u edges=%u display_xy=%.0f,%.0f logical=%dx%d configure=%u coalesced=%d old_buffer_unchanged=1\n",
        window->identity.window_id,event->serial,edges,
        runtime->interactive_resize_start_x,runtime->interactive_resize_start_y,
        (gint32)logical_width,(gint32)logical_height,state_serial,
        size_serial==state_serial);
    if(runtime->wayland)anco_wayland_context_wake(runtime->wayland);
}

static void window_committed(struct wl_listener *listener, void *data)
{
    (void)data;
    RuntimeWindow *window = wl_container_of(listener, window, commit);
    refresh_native_decorations(window->runtime);
    if (window->xdg && window->xdg->initial_commit) {
        ++window->mapping_epoch;
        window->sent_configuration = (ClientConfiguration){0};
        window->accepted_configuration = (ClientConfiguration){0};
        /* Remapping repeats XDG's initial handshake while the admitted role
         * and original Window ID remain the same. A requested freeform root
         * waits out only the original host's temporary construction mode1. */
        window->initial_commit_pending=TRUE;
        initialize_initial_commit(window);
    }
#if WLR_HAS_XWAYLAND
    if (window->xwayland_surface && window->initial_commit_pending) {
        ++window->mapping_epoch;
        initialize_initial_commit(window);
    }
#endif
    if(window->xdg && runtime_window_is_toplevel(window)){
        guint32 min_width=0,min_height=0,max_width=0,max_height=0;
        runtime_window_size_limits(window,&min_width,&min_height,&max_width,&max_height);
        if(!window->native_size_limits_known ||
           window->native_min_width!=min_width ||
           window->native_min_height!=min_height ||
           window->native_max_width!=max_width ||
           window->native_max_height!=max_height){
            window->native_size_limits_known=TRUE;
            window->native_min_width=min_width;
            window->native_min_height=min_height;
            window->native_max_width=max_width;
            window->native_max_height=max_height;
            (void)publish_native_metadata(window);
        }
    }
    struct wlr_buffer *buffer = NULL;
    guint32 sequence;
    const int status = window->xdg ?
        anco_wlroots_lock_commit_buffer(window->runtime->ownership,
            window->xdg,&window->identity,&buffer,&sequence) :
#if WLR_HAS_XWAYLAND
        anco_wlroots_lock_xwayland_buffer(window->runtime->ownership,
            window->xwayland_surface,&window->identity,&buffer,&sequence);
#else
        -EPROTOTYPE;
#endif
    if (status == -ENODATA) return;
    if (status) {
        printf("RUNTIME_BUFFER_REJECT window=%d status=%d\n",
            window->identity.window_id, status);
        return;
    }
    AncoWaylandGeometry committed_geometry;
    int geometry_status=0;
    if(window->xdg)
        geometry_status=anco_wayland_geometry_read(window->xdg,&committed_geometry);
    else {
        struct wlr_surface *surface=runtime_window_surface(window);
        if(!surface || surface->current.width<=0 || surface->current.height<=0)
            geometry_status=-ENODATA;
        else committed_geometry=(AncoWaylandGeometry){
            .extent={0,0,surface->current.width,surface->current.height},
            .geometry={0,0,surface->current.width,surface->current.height}};
    }
    if(geometry_status){
        wlr_buffer_unlock(buffer);
        printf("RUNTIME_GEOMETRY_REJECT window=%d commit=%u status=%d\n",
            window->identity.window_id,sequence,geometry_status);
        return;
    }
    window->pending_geometry = committed_geometry;
    window->pending_decoration=logical_server_decoration(window);
    window->pending_surface_width=runtime_window_surface(window)->current.width;
    window->pending_surface_height=runtime_window_surface(window)->current.height;
    update_original_window_layout(window,&committed_geometry);
    AncoGraphicLayer committed_layer;
    const int layer_status=committed_original_layer(window,&committed_geometry,buffer,
        &window->pending_decoration,&committed_layer);
    window->pending_layer_valid=layer_status==0;
    if(layer_status)
        printf("RUNTIME_LAYER_GEOMETRY_UNAVAILABLE window=%d commit=%u status=%d buffer_queue_unchanged=1\n",
            window->identity.window_id,sequence,layer_status);
    window->pending_layer=layer_status?(AncoGraphicLayer){0}:committed_layer;
    /* Actual application buffer, not a captured/composited desktop frame. */
    if (window->pending_buffer) wlr_buffer_unlock(window->pending_buffer);
    window->pending_buffer = buffer;
    window->pending_sequence = sequence;
    window->pending_configure_serial = window->xdg ?
        window->xdg->current.configure_serial : 0;
    window->pending_mapping_epoch = window->mapping_epoch;
    window->pending_transform = runtime_window_surface(window)->current.transform;
    if(window->xdg && window->resize_anchor_valid && window->resize_finalize_serial &&
        window->pending_configure_serial==window->resize_finalize_serial){
        printf("RUNTIME_STANDARD_RESIZE_COMMITTED window=%d serial=%u frame=%d,%d,%d,%d real_buffer_locked=1 anchor_released=1\n",
            window->identity.window_id,window->pending_configure_serial,
            window->layout_frame.left,window->layout_frame.top,
            window->layout_frame.right,window->layout_frame.bottom);
        window->resize_anchor_valid=FALSE;
        window->resize_finalize_serial=0;
    }
    struct wlr_box geometry={committed_geometry.geometry.left,
        committed_geometry.geometry.top,
        committed_geometry.geometry.right-committed_geometry.geometry.left,
        committed_geometry.geometry.bottom-committed_geometry.geometry.top};
    const gboolean accepted = window->accepted_configuration.valid &&
        window->accepted_configuration.serial == window->pending_configure_serial;
#if WLR_HAS_XWAYLAND
    if(window->xwayland_surface && g_getenv("ANCO_SHM_DIAGNOSTIC_LAYOUT"))
        printf("RUNTIME_XWAYLAND_BUFFER_SEMANTICS window=%d has_alpha=%d override_redirect=%d\n",
            window->identity.window_id,window->xwayland_surface->has_alpha,
            window->xwayland_surface->override_redirect);
#endif
    printf("RUNTIME_CLIENT_BUFFER_CONFIGURATION window=%d epoch=%llu commit=%u configure=%u accepted=%d geometry=%d,%d,%d,%d logical=%dx%d buffer=%dx%d client_fullscreen=%d\n",
        window->identity.window_id, (unsigned long long)window->mapping_epoch,
        sequence,window->pending_configure_serial,accepted,
        geometry.x,geometry.y,geometry.width,geometry.height,
        runtime_window_surface(window)->current.width,runtime_window_surface(window)->current.height,
        buffer->width,buffer->height,
        window->xdg && runtime_window_is_toplevel(window) ?
            window->xdg->toplevel->current.fullscreen :
#if WLR_HAS_XWAYLAND
            (window->xwayland_surface ? window->xwayland_surface->fullscreen : FALSE));
#else
            FALSE);
#endif
    if(window->initial_commit_pending){
        RuntimeTask *task=live_task_state(window->runtime,
            window->identity.owner.task_id);
        if(task)schedule_initial_commits_for_task(window->runtime,task);
    }
    schedule_submit(window);
    if (!window->reported_buffer) {
        window->reported_buffer = TRUE;
        printf("RUNTIME_FIRST_BUFFER window=%d task=%d activity=%d seq=%u width=%d height=%d host_submitted=0\n",
            window->identity.window_id, window->identity.owner.task_id,
            window->identity.owner.activity_id, sequence, buffer->width, buffer->height);
    }
}

static int process_for_xdg(struct wlr_xdg_surface *xdg,AncoLinuxProcess *process)
{
    if(!xdg || !process)return -EINVAL;
    struct wl_client *client=wl_resource_get_client(xdg->surface->resource);
    pid_t pid=-1;uid_t uid=(uid_t)-1;gid_t gid=(gid_t)-1;
    wl_client_get_credentials(client,&pid,&uid,&gid);
    int status=pid>0?anco_linux_process_read(pid,process):-ESRCH;
    if(!status && (process->uid!=uid || process->gid!=gid))status=-EPERM;
    return status;
}

static int create_window_carrier(Runtime *runtime,
    const AncoLinuxProcess *process,AncoLinuxLaunch *out)
{
    if(!runtime || !process || !out)return -EINVAL;
    int status=0;
    AncoLinuxLaunch launch={0};
    runtime->creating_window_carrier=TRUE;
    status=anco_linux_catalog_task_create_for_process(runtime->catalog,
        process,&launch);
    if(status==-ESRCH)
        status=anco_linux_catalog_task_adopt_registered_process(
            runtime->catalog,process,&launch);
    runtime->creating_window_carrier=FALSE;
    RuntimeTask *task=!status ? g_hash_table_lookup(runtime->tasks,
        GINT_TO_POINTER(launch.task_id)) : NULL;
    if(!status && !task)status=-ENOENT;
    if(!status){
        task->semantic_task=TRUE;
        const AncoAmsCaller caller={launch.app.guest_uid,launch.app.bundle};
        AncoAmsActivityStartResult notification={.transport=-ENOTCONN};
        const int notified=anco_ams_agent_notify_runtime_start(runtime->agent,
            launch.activity_id,-1,0,&caller,FALSE,&notification);
        printf("RUNTIME_WINDOW_CARRIER_CREATE task=%d activity=%d pid=%ld caller_uid=%d caller_package=%s source_activity=-1 launch_behind=0 original_activity_start=%d code=%u suppressed=%d attempted=%d transport=%d decoded=%d service=%d window_stack_policy=0\n",
            launch.task_id,launch.activity_id,(long)process->pid,caller.uid,
            caller.package,notified,notification.code,notification.suppressed,
            notification.attempted,notification.transport,
            notification.decoded,notification.result);
        if(notified)status=notified;
    }
    if(task && status){
        task->retire_requested=TRUE;
        schedule_semantic_task_retire(runtime);
    }
    if(!status)*out=launch;
    return status;
}

static int admit_role(Runtime *runtime, struct wlr_xdg_surface *xdg)
{
    AncoLinuxSurfaceOwner surface;
    RuntimeWindow *window = g_new0(RuntimeWindow, 1);
    RuntimeWindow *parent=runtime_parent_for_xdg(runtime,xdg);
    int status=0;
    const gboolean attached_toplevel=
        xdg->role==WLR_XDG_SURFACE_ROLE_TOPLEVEL && xdg->toplevel->parent;
    if(xdg->role==WLR_XDG_SURFACE_ROLE_POPUP){
        if(!parent)status=-ENODATA;
        if(!status)status=anco_wayland_ownership_track_activity(runtime->ownership,
            xdg->surface->resource,parent->identity.owner.activity_id,&surface);
    }else if(attached_toplevel){
        /* A desktop Linux transient is independently movable and may extend
         * outside its source Task. Preserve its real process/Task/Activity
         * ownership for input, IME, clipboard and producer lineage, but use
         * the original AnCo task=-1 createSysWindow carrier at WMS calls. */
        if(!parent)status=-ENODATA;
        if(!status)status=anco_wayland_ownership_track_activity(runtime->ownership,
            xdg->surface->resource,parent->identity.owner.activity_id,&surface);
        if(!status)window->native_system_window=TRUE;
    }else{
        AncoLinuxProcess process;AncoLinuxLaunch launch={0};
        status=process_for_xdg(xdg,&process);
        if(!status)status=create_window_carrier(runtime,&process,&launch);
        if(!status)status=anco_wayland_ownership_track_activity(
            runtime->ownership,xdg->surface->resource,
            launch.activity_id,&surface);
        if(status && launch.task_id>0){
            RuntimeTask *task=g_hash_table_lookup(runtime->tasks,
                GINT_TO_POINTER(launch.task_id));
            if(task){
                task->retire_requested=TRUE;
                schedule_semantic_task_retire(runtime);
            }
        }
    }
    if (!status) status = anco_wlroots_admit(runtime->ownership, xdg,
        surface.activity_id, &window->identity);
    if (status) {
        printf(status==-EAGAIN ?
            "RUNTIME_ROLE_DEFER role=%d status=%d original_state_wait=1\n" :
            "RUNTIME_ROLE_REJECT role=%d status=%d\n",xdg->role,status);
        g_free(window);
        return status;
    }
    window->runtime = runtime;
    window->xdg = xdg;
    runtime->windows=g_list_append(runtime->windows,window);
    if(window->native_system_window){
        gpointer native=NULL;
        if(g_hash_table_lookup_extended(runtime->native_system_ids,
                GINT_TO_POINTER(window->identity.window_id),NULL,&native))
            bind_native_system_window(runtime,native);
    }
    window->surface_key = (AncoDelegatorSurfaceKey){
        (guint32)window->identity.window_id,
        anco_surface_identity_generate(window->identity.owner.task_id,
            FALSE, runtime->shared_flag)};
    wlr_surface_send_enter(xdg->surface, runtime->output);
    wlr_surface_set_preferred_buffer_scale(xdg->surface,
        (int)ceil(runtime->display_info.scale));
    wlr_fractional_scale_v1_notify_scale(xdg->surface, runtime->display_info.scale);
    window->commit.notify = window_committed;
    window->destroy.notify = window_destroyed;
    window->configure.notify = configuration_sent;
    window->ack_configure.notify = configuration_acknowledged;
    window->map.notify = window_mapped;
    window->unmap.notify = window_unmapped;
    wl_signal_add(&xdg->surface->events.commit, &window->commit);
    wl_signal_add(&xdg->events.configure, &window->configure);
    wl_signal_add(&xdg->events.ack_configure, &window->ack_configure);
    wl_signal_add(&xdg->surface->events.unmap, &window->unmap);
    wl_signal_add(&xdg->surface->events.map, &window->map);
    if(xdg->role==WLR_XDG_SURFACE_ROLE_TOPLEVEL){
        window->request_move.notify=toplevel_request_move;
        wl_signal_add(&xdg->toplevel->events.request_move,&window->request_move);
        window->request_resize.notify=toplevel_request_resize;
        wl_signal_add(&xdg->toplevel->events.request_resize,&window->request_resize);
        window->request_maximize.notify=toplevel_request_maximize;
        wl_signal_add(&xdg->toplevel->events.request_maximize,&window->request_maximize);
        window->request_minimize.notify=toplevel_request_minimize;
        wl_signal_add(&xdg->toplevel->events.request_minimize,&window->request_minimize);
        window->request_fullscreen.notify=toplevel_request_fullscreen;
        wl_signal_add(&xdg->toplevel->events.request_fullscreen,&window->request_fullscreen);
        window->set_title.notify=toplevel_title_changed;
        wl_signal_add(&xdg->toplevel->events.set_title,&window->set_title);
        /* Deferred toplevel admission intentionally observes the standard
         * xdg_toplevel.set_parent ordering after the initial commit.  The
         * client may have sent its initial title before this listener existed;
         * publish wlroots' retained value through the same original title
         * path once, while later changes continue through set_title. */
        (void)publish_native_metadata(window);
    }
    struct wl_signal *destroy = xdg->role == WLR_XDG_SURFACE_ROLE_TOPLEVEL ?
        &xdg->toplevel->events.destroy : &xdg->popup->events.destroy;
    wl_signal_add(destroy, &window->destroy);
    printf("RUNTIME_ROLE_ADMIT role=%d window=%d task=%d activity=%d pid=%ld\n",
        xdg->role, window->identity.window_id, surface.task_id,
        surface.activity_id, (long)surface.process.pid);
    return 0;
}

static void pending_toplevel_unlink(RuntimePendingToplevel *pending)
{
    pending->runtime->pending_toplevels=
        g_list_remove(pending->runtime->pending_toplevels,pending);
    wl_list_remove(&pending->commit.link);
    wl_list_remove(&pending->destroy.link);
}

static gboolean admit_pending_toplevel(gpointer data);

static void schedule_pending_toplevels(Runtime *runtime,const char *reason)
{
    for(GList *node=runtime->pending_toplevels;node;node=node->next){
        RuntimePendingToplevel *pending=node->data;
        if(!pending->initial_commit_received || pending->admit_idle)continue;
        pending->admit_idle=g_idle_add(admit_pending_toplevel,pending);
        printf("RUNTIME_TOPLEVEL_ADMIT_RETRY_QUEUED source=%u reason=%s\n",
            pending->admit_idle,reason);
    }
}

static gboolean admit_pending_toplevel(gpointer data)
{
    RuntimePendingToplevel *pending=data;
    Runtime *runtime=pending->runtime;
    struct wlr_xdg_surface *xdg=pending->xdg;
    pending->admit_idle=0;
    if(runtime->stopping){
        pending_toplevel_unlink(pending);
        g_free(pending);
        return G_SOURCE_REMOVE;
    }
    /* xdg_toplevel.set_parent is ordered before the role's first surface
     * commit.  Admit after that commit so the existing original AnCo
     * Task/Activity start receives the real caller, while the standard
     * no-buffer initial configure is still produced by the same commit. */
    RuntimeWindow *parent=runtime_parent_for_xdg(runtime,xdg);
    printf("RUNTIME_TOPLEVEL_ADMIT_AFTER_INITIAL_COMMIT parent_window=%d standard_xdg_order=1\n",
        parent?parent->identity.window_id:-1);
    const int status=admit_role(runtime,xdg);
    if(status==-EAGAIN){
        puts("RUNTIME_TOPLEVEL_ADMIT_WAITING original_visible_freeform_not_ready=1");
        return G_SOURCE_REMOVE;
    }
    pending_toplevel_unlink(pending);
    g_free(pending);
    if(status)return G_SOURCE_REMOVE;
    RuntimeWindow *window=runtime_window_for_xdg(runtime,xdg);
    if(window)window_committed(&window->commit,NULL);
    return G_SOURCE_REMOVE;
}

static void pending_toplevel_committed(struct wl_listener *listener,void *data)
{
    (void)data;
    RuntimePendingToplevel *pending=
        wl_container_of(listener,pending,commit);
    pending->initial_commit_received=TRUE;
    if(pending->admit_idle)return;
    pending->admit_idle=g_idle_add(admit_pending_toplevel,pending);
    printf("RUNTIME_TOPLEVEL_INITIAL_COMMIT_QUEUED source=%u parent_present=%d\n",
        pending->admit_idle,pending->xdg->toplevel->parent!=NULL);
}

static void pending_toplevel_destroyed(struct wl_listener *listener,void *data)
{
    (void)data;
    RuntimePendingToplevel *pending=
        wl_container_of(listener,pending,destroy);
    if(pending->admit_idle)g_source_remove(pending->admit_idle);
    pending_toplevel_unlink(pending);
    g_free(pending);
}

static void new_toplevel(struct wl_listener *listener, void *data)
{
    Runtime *runtime = wl_container_of(listener, runtime, new_toplevel);
    struct wlr_xdg_toplevel *toplevel = data;
    /* Wait only for the protocol-defined initial commit.  By then the client
     * has either supplied xdg_toplevel.set_parent or declared an independent
     * root.  Never infer that relationship from process identity or focus. */
    RuntimePendingToplevel *pending=g_new0(RuntimePendingToplevel,1);
    pending->runtime=runtime;
    pending->xdg=toplevel->base;
    pending->commit.notify=pending_toplevel_committed;
    pending->destroy.notify=pending_toplevel_destroyed;
    wl_signal_add(&toplevel->base->surface->events.commit,&pending->commit);
    wl_signal_add(&toplevel->events.destroy,&pending->destroy);
    runtime->pending_toplevels=g_list_append(runtime->pending_toplevels,pending);
    puts("RUNTIME_TOPLEVEL_WAIT_INITIAL_COMMIT protocol_role_only=1");
}

static void new_popup(struct wl_listener *listener, void *data)
{
    Runtime *runtime = wl_container_of(listener, runtime, new_popup);
    struct wlr_xdg_popup *popup = data;
    (void)admit_role(runtime, popup->base);
}

#if WLR_HAS_XWAYLAND
static void pending_xwayland_unlink(RuntimePendingXwayland *pending)
{
    pending->runtime->pending_xwayland=g_list_remove(
        pending->runtime->pending_xwayland,pending);
    wl_list_remove(&pending->associate.link);
    wl_list_remove(&pending->destroy.link);
}

static void xwayland_dissociated(struct wl_listener *listener,void *data)
{
    (void)data;
    RuntimeWindow *window=wl_container_of(listener,window,destroy);
    Runtime *runtime=window->runtime;
    struct wlr_xwayland_surface *surface=window->xwayland_surface;
    RuntimeWindow *parent=direct_parent_window(window);
    const gboolean task_root=runtime_window_is_toplevel(window) &&
        !window->native_system_window && !parent;
    /* The stock system-window teardown does not remember its semantic Linux
     * parent. SceneBoard therefore chooses another native session when the
     * independently rendered transient disappears, even after XWM has already
     * focused the parent. Reuse the existing native-window request path only
     * when Linux focus actually returned to that live parent; this preserves
     * normal cross-application focus changes and adds no host-side protocol. */
    const gboolean restore_parent_focus=window->native_system_window && parent &&
        !parent->retiring && runtime->container_focused &&
        runtime->focused_window==parent;
    const gint32 retire_task=task_root ? window->identity.owner.task_id : 0;
    const guint32 xid=surface->window_id;
    /* wlroots emits dissociate while surface is still valid, then clears it.
     * The mapped X11 root is the Linux window lifecycle boundary: retire its
     * exact AnCo Window and semantic Task now, while leaving the Linux process
     * and X Window alive.  If that X Window later associates another real
     * wl_surface, normal admission creates a fresh window-owned Task instead
     * of resurrecting a host Task whose visible Linux window has gone away. */
    retire_window(window,task_root);
    if(restore_parent_focus)
        (void)request_native_window(parent,NATIVE_WINDOW_REQUEST_FOCUS,
            "x11-transient-retire");
    track_xwayland_surface(runtime,surface,0,FALSE);
    printf("RUNTIME_XWAYLAND_DISSOCIATE xid=%u task=%d semantic_retire=%d await_reassociate=1\n",
        xid,retire_task,task_root);
}

static int admit_xwayland_role(Runtime *runtime,
    struct wlr_xwayland_surface *surface)
{
    if (!surface || !surface->surface || !surface->surface->resource)
        return -EINVAL;
    RuntimeWindow *parent=runtime_parent_for_xwayland(runtime,surface);
    AncoLinuxProcess process={0};
    int status=anco_linux_process_read(surface->pid,&process);
    AncoLinuxLaunch launch={0};
    gboolean created_task=FALSE;
    RuntimeWindow *window=g_new0(RuntimeWindow,1);
    gint32 activity=parent ? parent->identity.owner.activity_id : 0;
    if(!status && !parent){
        status=create_window_carrier(runtime,&process,&launch);
        if(!status){activity=launch.activity_id;created_task=TRUE;}
    }
    if(!status)status=anco_wlroots_admit_xwayland(runtime->ownership,surface,
        activity,&window->identity);
    if(status){
        if(created_task){
            RuntimeTask *task=g_hash_table_lookup(runtime->tasks,
                GINT_TO_POINTER(launch.task_id));
            if(task){task->retire_requested=TRUE;schedule_semantic_task_retire(runtime);}
        }
        g_free(window);return status;
    }
    window->runtime=runtime;
    window->xwayland_surface=surface;
    window->native_system_window=parent && !surface->override_redirect;
    window->surface_key=(AncoDelegatorSurfaceKey){
        (guint32)window->identity.window_id,
        anco_surface_identity_generate(window->identity.owner.task_id,
            FALSE,runtime->shared_flag)};
    runtime->windows=g_list_append(runtime->windows,window);
    /* Xwayland initially places every managed X11 window at the bottom of
     * the X stack.  Preserve the client's WM_TRANSIENT_FOR relationship as
     * soon as both Linux windows have been admitted: otherwise a fully
     * covering parent can keep the transient obscured before its first
     * wl_surface buffer arrives, which in turn defers creation of the exact
     * AnCo child Window indefinitely. */
    if(parent && parent->xwayland_surface && !surface->override_redirect)
        wlr_xwayland_surface_restack(surface,parent->xwayland_surface,
            XCB_STACK_MODE_ABOVE);
    wlr_surface_send_enter(surface->surface,runtime->output);
    wlr_surface_set_preferred_buffer_scale(surface->surface,1);
    wlr_fractional_scale_v1_notify_scale(surface->surface,1.0);
    window->commit.notify=window_committed;
    window->destroy.notify=xwayland_dissociated;
    window->map.notify=window_mapped;
    window->unmap.notify=window_unmapped;
    window->set_title.notify=toplevel_title_changed;
    window->set_window_type.notify=xwayland_window_type_changed;
    window->set_hints.notify=xwayland_hints_changed;
    window->set_decorations.notify=xwayland_decorations_changed;
    window->request_move.notify=xwayland_request_move;
    window->request_maximize.notify=xwayland_request_maximize;
    window->request_minimize.notify=xwayland_request_minimize;
    window->request_fullscreen.notify=xwayland_request_fullscreen;
    window->request_resize.notify=xwayland_request_resize;
    window->request_configure.notify=xwayland_request_configure;
    window->request_activate.notify=xwayland_request_activate;
    wl_signal_add(&surface->surface->events.commit,&window->commit);
    wl_signal_add(&surface->events.dissociate,&window->destroy);
    wl_signal_add(&surface->surface->events.map,&window->map);
    wl_signal_add(&surface->surface->events.unmap,&window->unmap);
    wl_signal_add(&surface->events.set_title,&window->set_title);
    wl_signal_add(&surface->events.set_window_type,&window->set_window_type);
    wl_signal_add(&surface->events.set_hints,&window->set_hints);
    wl_signal_add(&surface->events.set_decorations,&window->set_decorations);
    wl_signal_add(&surface->events.request_move,&window->request_move);
    wl_signal_add(&surface->events.request_maximize,&window->request_maximize);
    wl_signal_add(&surface->events.request_minimize,&window->request_minimize);
    wl_signal_add(&surface->events.request_fullscreen,&window->request_fullscreen);
    wl_signal_add(&surface->events.request_resize,&window->request_resize);
    wl_signal_add(&surface->events.request_configure,&window->request_configure);
    wl_signal_add(&surface->events.request_activate,&window->request_activate);
    window->initial_commit_pending=TRUE;
    (void)publish_native_metadata(window);
    (void)xwayland_apply_retained_size_hints(window,"admission-retained");
    printf("RUNTIME_XWAYLAND_ROLE_ADMIT xid=%u window=%d task=%d activity=%d pid=%ld parent=%d override_redirect=%d window_carrier_created=%d pixel_capture=0\n",
        surface->window_id,window->identity.window_id,
        window->identity.owner.task_id,window->identity.owner.activity_id,
        (long)window->identity.owner.process.pid,
        parent?parent->identity.window_id:-1,surface->override_redirect,
        created_task);
    return 0;
}

static void pending_xwayland_associated(struct wl_listener *listener,void *data)
{
    (void)data;
    RuntimePendingXwayland *pending=wl_container_of(listener,pending,associate);
    struct wlr_xwayland_surface *surface=pending->surface;
    Runtime *runtime=pending->runtime;
    pending_xwayland_unlink(pending);
    g_free(pending);
    const int status=admit_xwayland_role(runtime,surface);
    printf("RUNTIME_XWAYLAND_ASSOCIATE xid=%u status=%d surface=%p\n",
        surface->window_id,status,(void *)surface->surface);
}

static void pending_xwayland_destroyed(struct wl_listener *listener,void *data)
{
    (void)data;
    RuntimePendingXwayland *pending=wl_container_of(listener,pending,destroy);
    Runtime *runtime=pending->runtime;
    const gint32 retire_task=pending->retire_task;
    pending_xwayland_unlink(pending);
    g_free(pending);
    if(retire_task){
        RuntimeTask *task=g_hash_table_lookup(runtime->tasks,
            GINT_TO_POINTER(retire_task));
        if(task){
            task->retire_requested=TRUE;
            schedule_semantic_task_retire(runtime);
        }
    }
    printf("RUNTIME_XWAYLAND_DESTROY task=%d semantic_retire=%d\n",
        retire_task,retire_task!=0);
}

static void track_xwayland_surface(Runtime *runtime,
    struct wlr_xwayland_surface *surface,gint32 retire_task,
    gboolean admit_if_associated)
{
    if(!runtime || !surface)return;
    RuntimePendingXwayland *pending=g_new0(RuntimePendingXwayland,1);
    pending->runtime=runtime;pending->surface=surface;
    pending->retire_task=retire_task;
    pending->associate.notify=pending_xwayland_associated;
    pending->destroy.notify=pending_xwayland_destroyed;
    wl_signal_add(&surface->events.associate,&pending->associate);
    wl_signal_add(&surface->events.destroy,&pending->destroy);
    runtime->pending_xwayland=g_list_append(runtime->pending_xwayland,pending);
    if(admit_if_associated && surface->surface)
        pending_xwayland_associated(&pending->associate,NULL);
}
#endif

static gboolean start_new_task(void *data)
{
    Runtime *runtime = data;
    runtime->startup_idle = 0;
    if (runtime->stopping) return G_SOURCE_REMOVE;
    AncoLinuxProcess process = {0};
    const int status = anco_linux_catalog_start_process(runtime->catalog,
        runtime->startup_bundle, runtime->startup_ability, &process);
    runtime->startup_task = -1;
    printf("RUNTIME_EXPLICIT_PROCESS_START status=%d task=-1 activity=-1 pid=%ld headless_launcher=1 window_driven_task=1\n",
        status, status ? -1L : (long)process.pid);
    if (status) {
        runtime->failed = TRUE;
        g_main_loop_quit(runtime->loop);
    }
    return G_SOURCE_REMOVE;
}

typedef struct {
    Runtime *runtime;
    int fd;
    guint source;
} RuntimeLaunchClient;

static void launch_client_free(RuntimeLaunchClient *client)
{
    if (!client) return;
    if (client->source) g_source_remove(client->source);
    if (client->fd >= 0) close(client->fd);
    g_free(client);
}

static gboolean launch_client_received(gint fd, GIOCondition condition, gpointer data)
{
    RuntimeLaunchClient *client = data;
    if (condition & (G_IO_ERR | G_IO_NVAL)) {
        client->source = 0;
        launch_client_free(client);
        return G_SOURCE_REMOVE;
    }
    char request[4096];
    const ssize_t length = recv(fd, request, sizeof(request) - 1, 0);
    if (length <= 0) {
        client->source = 0;
        launch_client_free(client);
        return G_SOURCE_REMOVE;
    }
    request[length] = 0;
    struct ucred credentials = {0};
    socklen_t credential_length = sizeof(credentials);
    int status = -EPERM;
    pid_t child = -1;
    char *bundle = NULL, *ability = NULL;
    if (!getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &credentials, &credential_length) &&
        credential_length == sizeof(credentials) && credentials.uid == 0) {
        JsonParser *parser = json_parser_new();
        GError *error = NULL;
        if (json_parser_load_from_data(parser, request, length, &error)) {
            JsonNode *root = json_parser_get_root(parser);
            if (root && JSON_NODE_HOLDS_OBJECT(root)) {
                JsonObject *object = json_node_get_object(root);
                if (json_object_has_member(object, "bundle"))
                    bundle = g_strdup(json_object_get_string_member(object, "bundle"));
                if (json_object_has_member(object, "ability"))
                    ability = g_strdup(json_object_get_string_member(object, "ability"));
                if (bundle && ability && strlen(bundle) <= 255 && strlen(ability) <= 255) {
                    AncoLinuxProcess process = {0};
                    status = anco_linux_catalog_start_process(client->runtime->catalog,
                        bundle, ability, &process);
                    if(status==-ENOENT){
                        GBinderRemoteObject *bms=find_bms(client->runtime->ipc);
                        const gboolean loaded=bms && load_catalog_bundle(
                            client->runtime->catalog,bms,
                            client->runtime->catalog_directory,
                            client->runtime->catalog_decoder,bundle);
                        if(bms)gbinder_remote_object_unref(bms);
                        if(loaded)status=anco_linux_catalog_start_process(
                            client->runtime->catalog,bundle,ability,&process);
                    }
                    if (!status) child = process.pid;
                } else status = -EINVAL;
            } else status = -EINVAL;
        } else status = -EINVAL;
        g_clear_error(&error);
        g_object_unref(parser);
    }
    char response[160];
    const int response_length = snprintf(response, sizeof(response),
        "{\"status\":%d,\"pid\":%ld}\n", status, (long)child);
    if (response_length > 0) (void)send(fd, response, (size_t)response_length, MSG_NOSIGNAL);
    printf("RUNTIME_LAUNCH_REQUEST peer_uid=%ld bundle=%s ability=%s status=%d pid=%ld process_only=1 task=-1\n",
        (long)credentials.uid, bundle ? bundle : "", ability ? ability : "", status, (long)child);
    g_free(bundle);
    g_free(ability);
    client->source = 0;
    launch_client_free(client);
    return G_SOURCE_REMOVE;
}

static gboolean launch_listener_received(gint fd, GIOCondition condition, gpointer data)
{
    Runtime *runtime = data;
    if (condition & (G_IO_ERR | G_IO_HUP | G_IO_NVAL)) {
        runtime->failed = TRUE;
        stop_runtime(runtime);
        return G_SOURCE_REMOVE;
    }
    for (;;) {
        const int client_fd = accept4(fd, NULL, NULL, SOCK_CLOEXEC | SOCK_NONBLOCK);
        if (client_fd < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            runtime->failed = TRUE;
            stop_runtime(runtime);
            return G_SOURCE_REMOVE;
        }
        RuntimeLaunchClient *client = g_new0(RuntimeLaunchClient, 1);
        client->runtime = runtime;
        client->fd = client_fd;
        client->source = g_unix_fd_add(client_fd, G_IO_IN | G_IO_HUP | G_IO_ERR | G_IO_NVAL,
            launch_client_received, client);
    }
    return G_SOURCE_CONTINUE;
}

static int launch_control_start(Runtime *runtime)
{
    static const char name[] = "l410-anco-launch-r1";
    const int fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0) return -errno;
    struct sockaddr_un address = {.sun_family = AF_UNIX};
    memcpy(address.sun_path + 1, name, sizeof(name) - 1);
    const socklen_t address_length = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + 1 + sizeof(name) - 1);
    if (bind(fd, (struct sockaddr *)&address, address_length) || listen(fd, 8)) {
        const int error = -errno;
        close(fd);
        return error;
    }
    runtime->launch_listener = fd;
    runtime->launch_source = g_unix_fd_add(fd, G_IO_IN | G_IO_ERR | G_IO_HUP | G_IO_NVAL,
        launch_listener_received, runtime);
    printf("RUNTIME_LAUNCH_CONTROL_READY endpoint=@%s peer_uid=0 process_only=1 window_creation=wayland_role\n", name);
    return runtime->launch_source ? 0 : -ENOMEM;
}

static void launch_control_finish(Runtime *runtime)
{
    if (runtime->launch_source) {
        g_source_remove(runtime->launch_source);
        runtime->launch_source = 0;
    }
    if (runtime->launch_listener >= 0) {
        close(runtime->launch_listener);
        runtime->launch_listener = -1;
    }
}

static void child_exited(AncoLinuxCatalog *catalog, const AncoLinuxLaunch *launch,
    gint wait_status, void *data)
{
    Runtime *runtime = data;
    RuntimeTask *task=launch->task_id>0 ?
        g_hash_table_lookup(runtime->tasks,GINT_TO_POINTER(launch->task_id)) : NULL;
    if (task) {
        task->process_exited=TRUE;
        task->retire_requested=TRUE;
        if(task->starting_window)retire_starting_window(task,"process-exit");
        for (guint i=0;i<task->activities->len;++i)
            if (g_array_index(task->activities,gint32,i) == runtime->focused_activity)
                runtime->focused_activity=0;
    }
    recompute_window_focus(runtime);
    if(task)schedule_semantic_task_retire(runtime);
    printf("RUNTIME_CHILD_EXIT task=%d pid=%ld wait_status=%d\n",
        launch->task_id, (long)launch->process.pid, wait_status);
    if (runtime->stopping && !anco_linux_catalog_children(catalog))
        g_main_loop_quit(runtime->loop);
}

static GBinderRemoteObject *find_bms(GBinderIpc *ipc)
{
    /* BMS is an original on-demand SA. Check/Get cannot start a NOT_STARTED
     * process. Use original LoadSystemAbility transaction 7 and its actual
     * success/failure callback, as for the required host manager; retain the
     * returned Binder reference for catalog admission, never synthesize BMS. */
    GBinderRemoteObject *object = anco_host_dependency_load(ipc, ANCO_BMS_SA);
    printf("RUNTIME_BMS_LOOKUP id=%d original_demand=1 object=%d\n",
        ANCO_BMS_SA, object != NULL);
    return object;
}

static gboolean load_catalog(AncoLinuxCatalog *catalog, GBinderIpc *ipc,
    const char *directory, const char *decoder)
{
    (void)ipc;
    (void)decoder;
    gchar *path = g_build_filename(directory, "packages.json", NULL);
    JsonParser *parser = json_parser_new();
    GError *error=NULL;
    gboolean ok = json_parser_load_from_file(parser, path, &error);
    if(!ok)printf("RUNTIME_CATALOG_READ_FAILED path=%s error=%s\n",path,error?error->message:"<missing>");
    g_clear_error(&error);
    g_free(path);
    JsonNode *root = ok ? json_parser_get_root(parser) : NULL;
    if (!root || !JSON_NODE_HOLDS_OBJECT(root)) {
        puts("RUNTIME_CATALOG_ROOT_INVALID");g_object_unref(parser);return FALSE;
    }
    JsonObject *packages = json_node_get_object(root);
    JsonObjectIter iter; const char *key; JsonNode *node;
    json_object_iter_init(&iter, packages);
    while (json_object_iter_next(&iter, &key, &node)) {
        if (!JSON_NODE_HOLDS_OBJECT(node)) { ok = FALSE; break; }
        JsonObject *record = json_node_get_object(node);
        const char *bundle = json_object_get_string_member(record, "bundle");
        const char *ability = json_object_get_string_member(record, "ability");
        const char *cwd = json_object_get_string_member(record, "cwd");
        JsonArray *args = json_object_get_array_member(record, "argv");
        if (!bundle || strcmp(key, bundle) || strchr(bundle, '/') || !ability || !cwd ||
            !args || !json_array_get_length(args)) { ok = FALSE; break; }
        gchar *filename = g_strconcat(bundle, ".json", NULL);
        path = g_build_filename(directory, filename, NULL);
        g_free(filename);
        gchar *payload = NULL; gsize length = 0;
        ok = g_file_get_contents(path, &payload, &length, NULL);
        g_free(path);
        if (!ok) break;
        int result = anco_linux_catalog_stage(catalog, payload, length);
        g_free(payload);
        const guint argc = json_array_get_length(args);
        const char **argv = g_new0(const char *, argc + 1);
        for (guint i = 0; i < argc; ++i) {
            argv[i] = json_array_get_string_element(args, i);
            if (!argv[i]) ok = FALSE;
        }
        if (!result && ok) result = anco_linux_catalog_command(catalog, bundle, ability, argv, cwd);
        g_free(argv);
        /* Catalog discovery is local metadata loading, not an application
         * launch.  Do not make the resident compositor depend on BMS recovery
         * for every installed desktop entry.  The existing lazy launch path
         * performs the original BMS recovery when that bundle is requested. */
        ok = ok && !result;
        printf("RUNTIME_CATALOG bundle=%s command=%d startup_local=1\n",
            bundle, result);
        if (!ok) break;
    }
    g_object_unref(parser);
    return ok;
}

static gboolean load_catalog_bundle(AncoLinuxCatalog *catalog,
    GBinderRemoteObject *bms,const char *directory,const char *decoder,
    const char *requested_bundle)
{
    if(!catalog || !bms || !directory || !decoder || !requested_bundle ||
       strchr(requested_bundle,'/'))return FALSE;
    gchar *path=g_build_filename(directory,"packages.json",NULL);
    JsonParser *parser=json_parser_new();GError *error=NULL;
    gboolean ok=json_parser_load_from_file(parser,path,&error);
    g_clear_error(&error);g_free(path);
    JsonNode *root=ok?json_parser_get_root(parser):NULL;
    JsonObject *packages=(root && JSON_NODE_HOLDS_OBJECT(root))?
        json_node_get_object(root):NULL;
    JsonNode *node=packages?json_object_get_member(packages,requested_bundle):NULL;
    if(!node || !JSON_NODE_HOLDS_OBJECT(node)){g_object_unref(parser);return FALSE;}
    JsonObject *record=json_node_get_object(node);
    const char *bundle=json_object_get_string_member(record,"bundle");
    const char *ability=json_object_get_string_member(record,"ability");
    const char *cwd=json_object_get_string_member(record,"cwd");
    JsonArray *args=json_object_get_array_member(record,"argv");
    if(!bundle || strcmp(bundle,requested_bundle) || !ability || !cwd || !args ||
       !json_array_get_length(args)){g_object_unref(parser);return FALSE;}
    gchar *filename=g_strconcat(bundle,".json",NULL);
    path=g_build_filename(directory,filename,NULL);g_free(filename);
    gchar *payload=NULL;gsize length=0;
    ok=g_file_get_contents(path,&payload,&length,NULL);g_free(path);
    int result=ok?anco_linux_catalog_stage(catalog,payload,length):-EIO;
    g_free(payload);
    const gboolean already_loaded=result==-EEXIST;
    if(already_loaded)result=0;
    const guint argc=json_array_get_length(args);
    const char **argv=g_new0(const char *,argc+1);
    for(guint i=0;ok && i<argc;++i){argv[i]=json_array_get_string_element(args,i);if(!argv[i])ok=FALSE;}
    if(!result && ok && !already_loaded)result=anco_linux_catalog_command(catalog,bundle,ability,argv,cwd);
    g_free(argv);
    AncoBmsRegistrationResult registration={0};
    gboolean recovered=FALSE;
    gboolean published=FALSE;
    if(ok && !result){
        recovered=anco_linux_catalog_recover(catalog,bms,bundle,100,decoder,
            &registration);
        if(!recovered)
            published=anco_linux_catalog_publish(catalog,bms,bundle,&registration);
        ok=recovered || published;
    }else ok=FALSE;
    printf("RUNTIME_CATALOG_LAZY bundle=%s command=%d recovered=%d published=%d accepted=%d local=%d transport=%d service=%d\n",
        bundle,result,recovered,published,ok,registration.local_error,
        registration.transport,registration.service_result);
    g_object_unref(parser);
    return ok;
}

static int publish_original_wms_callback(Runtime *runtime)
{
    /* Original CheckSystemAbility before Add: never replace another endpoint.
     * The same sole session owns this publication until its explicit teardown. */
    GBinderLocalRequest *request=gbinder_local_request_new(gbinder_ipc_io(runtime->ipc),
        gbinder_ipc_protocol(runtime->ipc),NULL);
    if(!request)return -ENOMEM;
    gbinder_local_request_append_int32(request,0x100);
    gbinder_local_request_append_int32(request,0);
    gbinder_local_request_append_string16(request,"ohos.samgr.accessToken");
    gbinder_local_request_append_int32(request,ANCO_WMS_CALLBACK_SA);
    int status=-1;
    GBinderRemoteReply *reply=gbinder_ipc_transact_sync_reply_main(runtime->ipc,0,2,request,&status);
    printf("RUNTIME_WMS_CALLBACK_LOOKUP id=%d transport=%d\n",ANCO_WMS_CALLBACK_SA,status);
    if(reply)gbinder_remote_reply_unref(reply);
    gbinder_local_request_unref(request);
    if(status!=7)return status?status:-EADDRINUSE;
    /* Real ordinary-window focus/mode/bounds consumers. Full configuration,
     * SmartEdge and input/
     * IME support are not manufactured from this publication or a decode. */
    const AncoWmsCallbackReceiverOps ops={.window=original_window_callback};
    runtime->wms_callback=anco_wms_callback_receiver_new(runtime->ipc,&ops,runtime);
    if(!runtime->wms_callback)return -ENOMEM;
    AncoWmsPublicationResult result;
    int rc=anco_wms_callback_receiver_publish(runtime->wms_callback,&result);
    printf("RUNTIME_WMS_CALLBACK_PUBLISH local=%d attempted=%d transport=%d decoded=%d service=%d focus_consumer=1 ordinary_root_mode_consumer=1 bounds_consumer=1 full_configuration_consumer=0 input_connected=0 ime_connected=0\n",
        rc,result.attempted,result.transport_status,result.reply_decoded,result.service_status);
    return rc;
}

static int retire_original_wms_callback(Runtime *runtime)
{
    if(!runtime->wms_callback)return 0;
    AncoWmsPublicationResult result;
    int rc=anco_wms_callback_receiver_withdraw(runtime->wms_callback,&result);
    printf("RUNTIME_WMS_CALLBACK_WITHDRAW local=%d attempted=%d transport=%d decoded=%d service=%d\n",
        rc,result.attempted,result.transport_status,result.reply_decoded,result.service_status);
    if(!rc)rc=anco_wms_callback_receiver_free(runtime->wms_callback);
    if(!rc)runtime->wms_callback=NULL;
    return rc;
}

static void unlock_session(AncoLinuxCatalog *catalog, gint32 user, void *data)
{
    (void)catalog;
    Runtime *runtime = data;
    if (runtime->display || runtime->failed) return;
    runtime->display = wl_display_create();
    const uint32_t shm_formats[] = {DRM_FORMAT_ARGB8888, DRM_FORMAT_XRGB8888};
    if (!runtime->display ||
        !(runtime->compositor=wlr_compositor_create(runtime->display, 5, NULL)) ||
        !wlr_subcompositor_create(runtime->display) ||
        /* GTK's standard seat closure requires both compositor and data
         * manager globals. This is the real wlroots manager, not a fake seat
         * or a claim that the original host Pasteboard bridge is ready. */
        !wlr_data_device_manager_create(runtime->display) ||
        !(runtime->shell = wlr_xdg_shell_create(runtime->display, 6)) ||
        !(runtime->decoration_manager=wlr_xdg_decoration_manager_v1_create(runtime->display)) ||
        !(runtime->legacy_decoration_manager=wlr_server_decoration_manager_create(runtime->display)) ||
        !wlr_shm_create(runtime->display, 1, shm_formats, G_N_ELEMENTS(shm_formats)) ||
        !(runtime->output_layout = wlr_output_layout_create(runtime->display)) ||
        !wlr_xdg_output_manager_v1_create(runtime->display, runtime->output_layout) ||
        !wlr_fractional_scale_manager_v1_create(runtime->display, 1) ||
        !wlr_viewporter_create(runtime->display) ||
        !(runtime->output = anco_wayland_display_create(runtime->display,
            runtime->output_layout, &runtime->display_info)) ||
        !(runtime->seat=wlr_seat_create(runtime->display, "anco")) ||
        wl_display_add_socket(runtime->display, "wayland-anco") ||
        l410_egl_open(runtime->display)) goto failure;
#if WLR_HAS_XWAYLAND
    if(g_strcmp0(g_getenv("ANCO_XWAYLAND"),"1")==0){
        runtime->xwayland=wlr_xwayland_create(runtime->display,
            runtime->compositor,FALSE);
        if(!runtime->xwayland)goto failure;
        wlr_xwayland_set_seat(runtime->xwayland,runtime->seat);
        runtime->xwayland_ready.notify=xwayland_ready;
        runtime->xwayland_new_surface.notify=xwayland_new_surface;
        wl_signal_add(&runtime->xwayland->events.ready,
            &runtime->xwayland_ready);
        wl_signal_add(&runtime->xwayland->events.new_surface,
            &runtime->xwayland_new_surface);
        puts("RUNTIME_XWAYLAND_START standard_rootless=1 standard_xwm=1 pixel_capture=0");
    }
#endif
    /* GTK3's standard decoration preference is negotiated before realizing
     * its toplevel. The original AnCo Task is the window-frame owner; use the
     * upstream legacy protocol manager, preserving explicit client requests
     * and undecorated popup semantics rather than forcing GTK_CSD or margins.
     * This announces frame ownership, not completed transient Task support. */
    wlr_server_decoration_manager_set_default_mode(runtime->legacy_decoration_manager,
        WLR_SERVER_DECORATION_MANAGER_MODE_SERVER);
    puts("RUNTIME_STANDARD_LEGACY_DECORATION default_mode=2 native_task_frame_owner=1 toplevel_task_ready=1");
    /* Seat existence only; capabilities stay zero until the actual original
     * input receiver is connected. Never advertise fabricated input support. */
    if(input_keyboard_start(runtime)||input_pointer_start(runtime))goto failure;
    runtime->wayland = anco_wayland_context_new(runtime->display);
    if (!runtime->wayland) goto failure;
    runtime->pasteboard=anco_linux_pasteboard_new(runtime->ipc,
        runtime->display,runtime->seat);
    if(!runtime->pasteboard)goto failure;
    puts("RUNTIME_WAYLAND_DATA_MANAGER standard_global=1 host_clipboard_connected=1 selection_consumer=1 drag_consumer=0 original_sa=66871,66823");
    printf("RUNTIME_WAYLAND_SEAT capabilities=%u native_state_session=%d keyboard_connected=%d pointer_connected=%d\n",
        runtime->seat->capabilities,runtime->input_session_open,runtime->input_keyboard_ready,
        runtime->input_pointer_ready);
    runtime->text_receiver=anco_text_receiver_new(runtime->display,
        runtime->ownership,text_changed,runtime);
    runtime->text_seat=runtime->text_receiver?
        anco_text_receiver_seat(runtime->text_receiver):NULL;
    if(!runtime->text_receiver||!runtime->text_seat||
       anco_text_receiver_panel_handler(runtime->text_receiver,text_panel))goto failure;
    runtime->new_client.notify=text_client_created;
    wl_display_add_client_created_listener(runtime->display,&runtime->new_client);
    runtime->imf=anco_linux_imf_runtime_new(runtime->ipc,runtime->text_seat,
        runtime->wayland,input_imf_key_done,runtime);
    if(!runtime->imf)goto failure;
    puts("RUNTIME_WAYLAND_TEXT_INPUT v2=1 v3=1 original_imf_callback=1 key_consumption_gate=1");
    if(g_strcmp0(g_getenv("ANCO_IBUS_BRIDGE"),"1")==0){
        const AncoIbusBridgeOps ibus_ops={.focus=ibus_focus,.panel=ibus_panel,.cursor=ibus_cursor,
            .surrounding=ibus_surrounding};
        runtime->ibus=anco_ibus_bridge_new(runtime->imf,&ibus_ops,runtime);
        if(!runtime->ibus)goto failure;
    }
    runtime->new_decoration.notify=new_decoration;
    wl_signal_add(&runtime->decoration_manager->events.new_toplevel_decoration,
        &runtime->new_decoration);
    runtime->new_toplevel.notify = new_toplevel;
    runtime->new_popup.notify = new_popup;
    wl_signal_add(&runtime->shell->events.new_toplevel, &runtime->new_toplevel);
    wl_signal_add(&runtime->shell->events.new_popup, &runtime->new_popup);
    printf("RUNTIME_WAYLAND guest=%d socket=wayland-anco protocol_loop=1 native_windows=0\n", user);
    if(runtime->startup_bundle
#if WLR_HAS_XWAYLAND
       && !runtime->xwayland
#endif
      )runtime->startup_idle=g_idle_add(start_new_task,runtime);
    return;
failure:
    runtime->failed = TRUE;
    puts("RUNTIME_SESSION_INIT_FAILED");
    g_main_loop_quit(runtime->loop);
}

static int query_surface_parameters(char **value, gint32 *flag)
{
    char *argv[] = {"/opt/l410/anco-host-record/query-surface-parameters",NULL};
    char *output = NULL, *errors = NULL;
    gint status = 0;
    GError *error = NULL;
    gboolean ok = g_spawn_sync(NULL,argv,NULL,0,NULL,NULL,
        &output,&errors,&status,&error);
    if (ok) ok = g_spawn_check_wait_status(status,&error);
    printf("RUNTIME_NATIVE_SURFACE_PARAMETERS completed=%d wait_status=%d stderr_bytes=%zu\n",
        ok,status,errors ? strlen(errors) : 0);
    g_free(errors);
    JsonParser *parser = json_parser_new();
    if (ok) ok = output && json_parser_load_from_data(parser,output,-1,&error);
    JsonNode *root = ok ? json_parser_get_root(parser) : NULL;
    if (!root || !JSON_NODE_HOLDS_OBJECT(root)) ok = FALSE;
    if (ok) {
        JsonObject *object = json_node_get_object(root);
        JsonNode *raw = json_object_get_member(object,"value");
        JsonNode *number = json_object_get_member(object,"flag");
        ok = raw && number && json_node_get_value_type(raw) == G_TYPE_STRING &&
            json_node_get_value_type(number) == G_TYPE_INT64;
        if (ok) {
            gint64 n = json_node_get_int(number);
            ok = n >= G_MININT32 && n <= G_MAXINT32;
            if (ok) { *value = g_strdup(json_node_get_string(raw)); *flag = (gint32)n; }
        }
    }
    if (error) fprintf(stderr,"RUNTIME_SURFACE_PARAMETERS_ERROR %s\n",error->message);
    g_clear_error(&error); g_object_unref(parser); g_free(output);
    return ok ? 0 : -EIO;
}

static gboolean stop_runtime(void *data)
{
    Runtime *runtime = data;
    runtime->stopping = TRUE;
    if (runtime->startup_idle) {
        g_source_remove(runtime->startup_idle);
        runtime->startup_idle = 0;
    }
    /* Both explicit startup and original host launches enter this catalog.
     * Signal each actual live process once, using its owned Task in the same
     * PID namespace; never treat a guest PID as a host cleanup identifier.
     * Keep Binder/Wayland alive until the catalog's real child watches drain. */
    GHashTable *signalled = g_hash_table_new(g_direct_hash, g_direct_equal);
    for (GList *t = runtime->task_stack; t; t = t->next) {
        const RuntimeTask *task = t->data;
        AncoLinuxLaunch live;
        if (anco_linux_catalog_task(runtime->catalog, task->id, &live)) continue;
        gpointer process = GINT_TO_POINTER(live.process.pid);
        if (g_hash_table_contains(signalled, process)) continue;
        g_hash_table_add(signalled, process);
        const int status = anco_linux_catalog_signal(runtime->catalog,
            task->id, SIGTERM);
        printf("RUNTIME_STOP_CHILD task=%d pid=%ld status=%d\n",
            task->id, (long)live.process.pid, status);
        if (status && status != -ESRCH) runtime->failed = TRUE;
    }
    g_hash_table_unref(signalled);
    if (anco_linux_catalog_children(runtime->catalog)) return G_SOURCE_CONTINUE;
    g_main_loop_quit(runtime->loop);
    return G_SOURCE_CONTINUE;
}

static gboolean session_bus_lost(gpointer data)
{
    Runtime *runtime = data;
    runtime->failed = TRUE;
    stop_runtime(runtime);
    return G_SOURCE_REMOVE;
}

int main(int argc, char **argv)
{
    if (argc != 4 && argc != 6) {
        fprintf(stderr, "usage: %s CATALOG_DIRECTORY NATIVE_RECORD_DECODER WINDOW_RESOURCE_VALUE [EXPLICIT_NEW_BUNDLE ABILITY]\n", argv[0]);
        return 64;
    }
    const char *runtime_preload = g_getenv("LD_PRELOAD");
    const gboolean runtime_preload_consumed = runtime_preload &&
        !strcmp(runtime_preload, "/opt/l410/mali/libdrm-query-compat.so");
    if (runtime_preload_consumed) g_unsetenv("LD_PRELOAD");
    char *end = NULL;
    errno = 0;
    const long resource_value = strtol(argv[3], &end, 10);
    if (errno || !*argv[3] || *end || resource_value <= 0 ||
        resource_value > G_MAXINT32 / 100000) return 64;
    /* Captured immediately before enter by the original native parameter
     * reader. No guest property service, hardcoded host version or feature.
     * Optional, so previous role/metadata-only invocations remain unchanged. */
    const char *broker_version = g_getenv("ANCO_AMS_BROKER_VERSION");
    const char *multiwindow = g_getenv("ANCO_AMS_MULTIWINDOW");
    if ((broker_version && (!*broker_version || !multiwindow ||
         (strcmp(multiwindow, "false") && strcmp(multiwindow, "true")))) ||
        (!broker_version && multiwindow)) return 64;
    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("RUNTIME_GRAPHIC_PRELOAD scoped_to_runtime=%d child_environment_clean=%d\n",
        runtime_preload_consumed, g_getenv("LD_PRELOAD") == NULL);
    const char *runtime_dir = "/run/l410-anco";
    if (getuid() != 0 || mkdir(runtime_dir, 0700) || chown(runtime_dir, 1000, 1000) ||
        setgroups(0, NULL) || setgid(1000) || setuid(1000)) { perror("session identity"); return 65; }
    g_setenv("XDG_RUNTIME_DIR", runtime_dir, TRUE);
    g_setenv("WAYLAND_DISPLAY", "wayland-anco", TRUE);
    g_setenv("HOME", "/home/linux", TRUE);
    GBinderIpc *ipc = gbinder_ipc_new("/dev/binder", "aidl2");
    if (!ipc) return 66;
    const char *title_directory = g_getenv("ANCO_NATIVE_WINDOW_TITLE_DIR");
    gchar *icon_directory = title_directory ?
        g_build_filename(title_directory, "icons", NULL) : NULL;
    AncoResourceAgent *resource_agent = anco_resource_agent_new(ipc,
        icon_directory);
    if (!resource_agent) return 88;
    const int resource_registered = anco_resource_agent_register(resource_agent);
    const gboolean resource_reused = resource_registered == -EADDRINUSE;
    printf("RUNTIME_RESOURCE_PROVIDER_REGISTER id=66894 result=%d original_interface=1 owned=%d reused=%d\n",
        resource_registered, resource_registered == 0, resource_reused);
    if (resource_registered && !resource_reused) return 88;
    AncoLinuxCatalog *catalog = anco_linux_catalog_new("org.l410.linux.", 0, FALSE);
    if (!load_catalog(catalog, ipc, argv[1], argv[2])) return 67;
    if (anco_linux_catalog_windows_configure(catalog, (gint32)resource_value)) return 73;
    AncoGraphicDisplay *displays = NULL;
    size_t display_count = 0;
    int32_t display_service = INT32_MIN;
    int display_status = anco_graphic_query_displays(
        "/dev/cross/uds/virt_graphic_broker/virtgraphic.sock", 2000,
        &displays, &display_count, &display_service);
    printf("RUNTIME_DISPLAY_QUERY local=%d service=%d count=%zu\n",
        display_status, display_service, display_count);
    for (size_t i = 0; i < display_count; ++i)
        printf("RUNTIME_DISPLAY id=%llu width=%d height=%d xdpi=%.6f ydpi=%.6f hz=%u\n",
            (unsigned long long)displays[i].id, displays[i].width, displays[i].height,
            (double)displays[i].xdpi, (double)displays[i].ydpi, displays[i].refresh_hz);
    if (display_status) return 77;
    AncoWaylandDisplayInfo display_info;
    display_status = anco_wayland_display_query("/opt/l410/anco-host-record/query-display",
        displays, display_count, &display_info);
    free(displays);
    if (display_status) { printf("RUNTIME_DISPLAY_MATCH status=%d\n", display_status); return 79; }
    AncoWmsBroker *wms = anco_wms_broker_new(ipc);
    if (!wms) return 71;
    AncoDelegatorRegistryInit graphics;
    AncoDelegatorRegistry *delegators = anco_delegator_registry_new_original(ipc, &graphics);
    if (!delegators) return 72;
    printf("RUNTIME_GRAPHIC local=%d lookup=%d resolved=%d attempts=%u token=%u features=%u watched=%d\n",
        graphics.check_status, graphics.check.lookup.transport, graphics.check.lookup.resolved,
        graphics.check.exchange.attempts, graphics.check.state.token_supported,
        graphics.check.state.features, graphics.check.watch_installed);
    for (guint i = 0; i < graphics.check.exchange.attempts; ++i)
        printf("RUNTIME_GRAPHIC_REPLY attempt=%u transport=%d decoded=%d value=%d\n", i,
            graphics.check.exchange.wire[i].transport, graphics.check.exchange.wire[i].decoded,
            graphics.check.exchange.wire[i].service_result);
    Runtime runtime = {.loop = g_main_loop_new(NULL, FALSE), .catalog = catalog,
        .catalog_directory=argv[1],.catalog_decoder=argv[2],
        .icon_directory=icon_directory,
        .input_fd=-1,
        .launch_listener=-1,
        .wms = wms,
        .display_info = display_info,
        .ownership = anco_wayland_ownership_new(catalog),
        .startup_broker_version = broker_version,
        .startup_multiwindow = multiwindow && !strcmp(multiwindow, "true"),
        .startup_bundle = argc == 6 ? argv[4] : NULL,
        .startup_ability = argc == 6 ? argv[5] : NULL};
    if (!runtime.ownership) return 74;
    runtime.tasks=g_hash_table_new_full(g_direct_hash,g_direct_equal,NULL,task_state_free);
    runtime.native_system_ids=g_hash_table_new_full(g_direct_hash,g_direct_equal,NULL,g_free);
    if (anco_linux_catalog_on_activity_created(catalog,runtime_activity_created,&runtime)) return 71;
    runtime.ipc = ipc;
    runtime.delegators = delegators;
    if(client_properties_start(&runtime))return 86;
    if(input_relay_attach(&runtime))return 85;
    if(publish_original_wms_callback(&runtime))return 82;
    if (query_surface_parameters(&runtime.buffershared,&runtime.shared_flag)) return 81;
    printf("RUNTIME_SURFACE_PARAMETERS value=%s flag=%d\n",
        runtime.buffershared,runtime.shared_flag);
    AncoAmsAgent *agent = anco_ams_agent_new(ipc, catalog, anco_ams_start_launcher,
        unlock_session, &runtime);
    runtime.agent = agent;
    if(agent && broker_version){
        /* The original guest system Context which owns AncoActivityManager is
         * UID1000/package android.  This config supplies only that already-
         * defined launcher context and the paired original feature properties;
         * each Linux semantic start still supplies its actual registered app
         * caller and source Activity explicitly. */
        const AncoAmsLauncherRuntime launcher={
            .broker_version=broker_version,
            .multiwindow_enabled=runtime.startup_multiwindow,
            .context_caller={(gint32)getuid(),"android"},
            .background_tasks=no_background_window_policy,
            .user_data=&runtime};
        const int configured=anco_ams_agent_configure_launcher(agent,&launcher);
        printf("RUNTIME_ORIGINAL_LAUNCHER_CONFIG local=%d context_uid=%ld context_package=android broker=%s multiwindow=%d background_collector=%d window_stack_policy=0\n",
            configured,(long)getuid(),broker_version,runtime.startup_multiwindow,TRUE);
        if(configured)return 75;
    }
    if (agent && anco_ams_agent_on_task_selected(agent, runtime_task_selected, &runtime)) return 75;
    if (agent && anco_ams_agent_on_child_exit(agent, child_exited, &runtime)) return 75;
    if (agent && anco_ams_agent_on_mission(agent, runtime_mission_observed, &runtime)) return 75;
    if (agent && anco_ams_agent_on_configuration(agent, configuration_received, &runtime)) return 78;
    if (agent && anco_ams_agent_on_task_remove(agent, original_task_remove_received, &runtime)) return 78;
    /* Linux-private standard IPC, shared by all real application children.
     * Original host Binder/AnCo registration and window policy are unchanged. */
    gchar *tray_directory = g_build_filename(argv[1], "tray-bridge", NULL);
    if (anco_linux_session_bus_start(runtime_dir, tray_directory,
        session_bus_lost, &runtime)) { g_free(tray_directory); return 87; }
    g_free(tray_directory);
    int registered = agent ? anco_ams_agent_register(agent) : -1;
    printf("RUNTIME_AGENT_REGISTER result=%d\n", registered);
    if (registered) return 68;
    guint terminate = g_unix_signal_add(SIGTERM, stop_runtime, &runtime);
    guint interrupt = g_unix_signal_add(SIGINT, stop_runtime, &runtime);
    int account = anco_ams_agent_boot_default_user(agent);
    printf("RUNTIME_ACCOUNT result=%d ordinary_launcher_configured=0\n", account);
    if (account) return 69;
    if (launch_control_start(&runtime)) return 89;
    if (!runtime.failed) g_main_loop_run(runtime.loop);
    g_source_remove(terminate); g_source_remove(interrupt);
    launch_control_finish(&runtime);
    anco_client_properties_observer_free(runtime.client_properties_observer);
    runtime.client_properties_observer=NULL;
    g_clear_pointer(&runtime.native_system_ids,g_hash_table_unref);
    if(runtime.initial_commit_resume_idle){
        g_source_remove(runtime.initial_commit_resume_idle);
        runtime.initial_commit_resume_idle=0;
    }
    if(runtime.producer_resume_idle){
        g_source_remove(runtime.producer_resume_idle);
        runtime.producer_resume_idle=0;
    }
    if(runtime.task_retire_idle){
        g_source_remove(runtime.task_retire_idle);
        runtime.task_retire_idle=0;
    }
    if(runtime.layer_idle){g_source_remove(runtime.layer_idle);runtime.layer_idle=0;}
    anco_graphic_layers_close(runtime.layer_stream);runtime.layer_stream=NULL;
    if (runtime.display) wl_display_destroy_clients(runtime.display);
#if WLR_HAS_XWAYLAND
    if(runtime.xwayland){
        wl_list_remove(&runtime.xwayland_ready.link);
        wl_list_remove(&runtime.xwayland_new_surface.link);
        wlr_xwayland_destroy(runtime.xwayland);
        runtime.xwayland=NULL;
    }
#endif
    /* Explicit service stop has already waited for all real child watches.
     * Destroying Wayland/Xwayland above retires every native Window and marks
     * its semantic Task.  The ordinary idle is intentionally disabled while
     * stopping, so synchronously drain the SAME original Activity/Task chain
     * before withdrawing WMS/AMS callbacks.  This adds no shutdown protocol
     * and never removes a Task while one of its native Windows still exists. */
    retire_semantic_tasks_now(&runtime);
    printf("RUNTIME_TASK_RETIRE_DRAIN stopping=1 remaining=%u windows=%u\n",
        g_hash_table_size(runtime.tasks),g_list_length(runtime.windows));
    if(g_hash_table_size(runtime.tasks) || runtime.windows)runtime.failed=TRUE;
    if(retire_original_wms_callback(&runtime))return 83;
    if(runtime.pasteboard){
        const int pasteboard_retired=anco_linux_pasteboard_free(runtime.pasteboard);
        printf("RUNTIME_PASTEBOARD_RETIRE result=%d\n",pasteboard_retired);
        if(pasteboard_retired)runtime.failed=TRUE;else runtime.pasteboard=NULL;
    }
    if(runtime.text_receiver)wl_list_remove(&runtime.new_client.link);
    if(runtime.ibus){anco_ibus_bridge_free(runtime.ibus);runtime.ibus=NULL;}
    if(runtime.imf){
        const int imf_retired=anco_linux_imf_runtime_free(runtime.imf);
        printf("RUNTIME_IMF_RETIRE result=%d\n",imf_retired);
        if(imf_retired)runtime.failed=TRUE;else runtime.imf=NULL;
    }
    if(runtime.text_receiver){anco_text_receiver_free(runtime.text_receiver);
        runtime.text_receiver=NULL;runtime.text_seat=NULL;}
    anco_wayland_ownership_free(runtime.ownership);
    if(runtime.decoration_manager)wl_list_remove(&runtime.new_decoration.link);
    if (runtime.shell) {
        wl_list_remove(&runtime.new_toplevel.link);
        wl_list_remove(&runtime.new_popup.link);
    }
    if (runtime.wayland) anco_wayland_context_free(runtime.wayland);
    if(runtime.input_source){g_source_remove(runtime.input_source);runtime.input_source=0;}
    input_relay_disconnect(&runtime);
    if (runtime.output_layout) wlr_output_layout_destroy(runtime.output_layout);
    if (runtime.output) wlr_output_destroy(runtime.output);
    if (runtime.display) l410_egl_close(runtime.display);
    if (runtime.display) wl_display_destroy(runtime.display);
    anco_ams_agent_on_task_selected(agent, NULL, &runtime);
    anco_ams_agent_on_child_exit(agent, NULL, &runtime);
    anco_ams_agent_on_mission(agent, NULL, &runtime);
    anco_ams_agent_on_configuration(agent, NULL, &runtime);
    anco_ams_agent_on_task_remove(agent, NULL, &runtime);
    int retired = anco_ams_agent_free(agent);
    printf("RUNTIME_AGENT_RETIRE result=%d\n", retired);
    if (retired) return 69;
    anco_delegator_registry_free(delegators);
    g_free(runtime.buffershared);
    anco_linux_catalog_on_activity_created(catalog, NULL, &runtime);
    g_list_free(runtime.task_stack);
    g_hash_table_destroy(runtime.tasks);
    anco_wms_broker_free(wms);
    int freed = anco_linux_catalog_free(catalog);
    printf("RUNTIME_CATALOG_RETIRE result=%d\n", freed);
    if (freed) return 76;
    anco_linux_session_bus_stop();
    const int resource_retired = anco_resource_agent_free(resource_agent);
    printf("RUNTIME_RESOURCE_PROVIDER_RETIRE id=66894 result=%d\n",
        resource_retired);
    if (resource_retired) return 88;
    g_free(icon_directory);
    gbinder_ipc_unref(ipc);
    g_main_loop_unref(runtime.loop);
    return runtime.failed ? 70 : 0;
}
