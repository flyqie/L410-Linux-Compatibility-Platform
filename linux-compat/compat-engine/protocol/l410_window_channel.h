/* 窗口通道协议：宿主侧图形 broker 与容器侧显示服务之间。
 *
 * 逐条对应 anco 的 GraphicBroker / SurfaceViewListenerStub 接口（见
 * COMPAT-ENGINE-PLAN.md 的对应表），不引入新概念：
 *   CreateSurfaceNode            -> SURFACE_NODE_CREATE
 *   SetSurfaceWithWindowID       -> SURFACE_SET        （随消息附带缓冲 fd / fence fd）
 *   SetSurfaceNodeByWindowId     -> SURFACE_NODE_SET
 *   RemoveSurfaceNode            -> SURFACE_NODE_REMOVE
 *   DestroySurfaceByWindowID     -> SURFACE_DESTROY
 *   UpdateWindowZIndex           -> WINDOW_SET_ZINDEX
 *   UpdateWindowProperty         -> WINDOW_SET_PROPERTY
 *   UpdateWindowBackgroundColor  -> WINDOW_SET_BGCOLOR
 *   CreateWindow/ShowWindow/HideWindow/RemoveWindow -> WINDOW_CREATE/SHOW/HIDE/REMOVE
 *   SetWindowTaskId / NotifyTaskStarted / NotifyTaskDestroyed -> WINDOW_TASK_ID / TASK_STARTED / TASK_DESTROYED
 *   SetAlphaWithWindowId         -> WINDOW_SET_ALPHA
 *   SetRelativeWindowId          -> WINDOW_SET_RELATIVE
 *   SetDisplayRotation           -> DISPLAY_ROTATION
 *   SetActiveDisplaySize         -> DISPLAY_SIZE
 *   SetRefreshRate               -> DISPLAY_REFRESH
 *   SetAncoStatus / SetAnimationStatus -> ENGINE_STATUS / ANIMATION_STATUS
 *
 * 传输：AF_UNIX 抽象套接字，一行一条文本消息；图形缓冲与同步栅栏用 SCM_RIGHTS 传 fd。
 * 方向：
 *   宿主 -> 容器：DISPLAY_*、WINDOW_CREATE/SHOW/HIDE/REMOVE/SET_*、SURFACE_NODE_REMOVE、TASK_DESTROYED、INPUT_*（见输入通道）
 *   容器 -> 宿主：SURFACE_NODE_CREATE、SURFACE_SET、SURFACE_COMMIT、SURFACE_RELEASE、WINDOW_TITLE、WINDOW_READY、TASK_STARTED
 */
#ifndef L410_WINDOW_CHANNEL_H
#define L410_WINDOW_CHANNEL_H

#include <stdint.h>

/* 端点命名（与既有部署一致，不带任何网络协议）：
 *   宿主服务  : l410-graphic-broker
 *   输入通道  : l410-input-channel
 *   每引擎实例: l410-engine-<pid>          （容器侧显示/输入服务与宿主 broker 的会话）
 */

/* 缓冲状态：对应 anco 里"producer 队列"的取用/归还，宿主按 fence 上屏。 */
typedef struct {
    int32_t  fd;        /* 图形缓冲 fd（SCM_RIGHTS 传入） */
    int32_t  fence_fd;  /* 同步栅栏 fd，-1 表示无 */
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    uint32_t size;
    int32_t  format;    /* OHOS 图形格式（12 = RGBA_8888） */
    uint64_t usage;
    uint64_t sequence;  /* 同一窗口的提交序号，宿主据此丢弃过期帧 */
} L410WindowBuffer;

/* 窗口属性：宿主掌握这些值，容器只上报"应用希望的"值。 */
typedef struct {
    int32_t x;
    int32_t y;
    uint32_t width;
    uint32_t height;
    int32_t z_index;
    float   alpha;
    uint32_t background;   /* ARGB */
    uint8_t visible;
    uint8_t focused;
    uint8_t decorated;     /* 是否由宿主绘制标题栏 */
    uint8_t reserved;
} L410WindowProperty;

/* 消息类型（第一个字段即类型名，见上面的对应表）。 */
enum {
    /* 宿主 -> 容器 */
    L410_MSG_DISPLAY_SIZE      = 1,   /* DISPLAY_SIZE w= h= density= */
    L410_MSG_DISPLAY_ROTATION  = 2,   /* DISPLAY_ROTATION degrees= */
    L410_MSG_DISPLAY_REFRESH   = 3,   /* DISPLAY_REFRESH hz= */
    L410_MSG_WINDOW_CREATE     = 4,   /* WINDOW_CREATE window= w= h= task= */
    L410_MSG_WINDOW_SHOW       = 5,   /* WINDOW_SHOW window= */
    L410_MSG_WINDOW_HIDE       = 6,   /* WINDOW_HIDE window= */
    L410_MSG_WINDOW_REMOVE     = 7,   /* WINDOW_REMOVE window= */
    L410_MSG_WINDOW_SET_ZINDEX = 8,   /* WINDOW_SET_ZINDEX window= z= */
    L410_MSG_WINDOW_SET_ALPHA  = 9,   /* WINDOW_SET_ALPHA window= alpha= */
    L410_MSG_WINDOW_SET_PROPERTY = 10,/* WINDOW_SET_PROPERTY window= x= y= w= h= decorated= */
    L410_MSG_WINDOW_SET_BGCOLOR = 11, /* WINDOW_SET_BGCOLOR window= argb= */
    L410_MSG_WINDOW_SET_RELATIVE = 12,/* WINDOW_SET_RELATIVE window= parent= */
    L410_MSG_SURFACE_NODE_REMOVE = 13,/* SURFACE_NODE_REMOVE window= */
    L410_MSG_TASK_DESTROYED    = 14,  /* TASK_DESTROYED task= */
    L410_MSG_ENGINE_STATUS     = 15,  /* ENGINE_STATUS state= */
    L410_MSG_ANIMATION_STATUS  = 16,  /* ANIMATION_STATUS state= */
    L410_MSG_FRAME_DONE        = 17,  /* FRAME_DONE window= sequence= status= */

    /* 容器 -> 宿主 */
    L410_MSG_SURFACE_NODE_CREATE = 64,/* SURFACE_NODE_CREATE window= w= h= name= */
    L410_MSG_SURFACE_SET         = 65,/* SURFACE_SET window= sequence= （附缓冲 fd + fence fd） */
    L410_MSG_SURFACE_COMMIT      = 66,/* SURFACE_COMMIT window= sequence= */
    L410_MSG_SURFACE_RELEASE     = 67,/* SURFACE_RELEASE window= sequence= */
    L410_MSG_SURFACE_DESTROY     = 68,/* SURFACE_DESTROY window= */
    L410_MSG_WINDOW_TITLE        = 69,/* WINDOW_TITLE window= title=<utf8> */
    L410_MSG_WINDOW_READY        = 70,/* WINDOW_READY window= */
    L410_MSG_TASK_STARTED        = 71,/* TASK_STARTED task= title=<utf8> */
};

#endif /* L410_WINDOW_CHANNEL_H */
