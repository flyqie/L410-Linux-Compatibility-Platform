/* 宿主侧图形 broker 的接口。
 *
 * 函数名与语义逐条照抄 anco 的 GraphicBroker / SurfaceViewListenerStub（见
 * had-w32-analysis/extracted/system/p111-system/system/lib64/libgraphic_broker_proxy.z.so 的导出符号），
 * 只把 anco 前缀换成 l410，方便一一对照，不再设计新接口。
 *
 * 实现分两步：
 *   1) 首版寄生于管理 HAP 的 native 模块（宿主窗口用已跑通的 XComponent 路径），
 *   2) 之后提升为 init 系统服务（uid + SELinux 域），与 anco 的形态一致。
 */
#ifndef L410_GRAPHIC_BROKER_H
#define L410_GRAPHIC_BROKER_H

#include <stdint.h>
#include "../protocol/l410_window_channel.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct L410GraphicBroker L410GraphicBroker;

/* GraphicBrokerClient::GetInstance / Init */
L410GraphicBroker *l410_graphic_broker_get_instance(void);
int32_t l410_graphic_broker_init(L410GraphicBroker *broker);

/* ---- 与 anco GraphicBroker 一一对应 ---- */
int32_t l410_graphic_broker_create_surface_node(L410GraphicBroker *broker, uint64_t display_id,
                                                int32_t window_id, const char *name, void *callback);
int32_t l410_graphic_broker_set_surface_node_by_window_id(L410GraphicBroker *broker, uint64_t display_id,
                                                          int32_t window_id, float x, float y, float alpha,
                                                          uint32_t z, const char *name, uint8_t decorated);
int32_t l410_graphic_broker_set_surface_with_window_id(L410GraphicBroker *broker, int64_t window_id,
                                                       const L410WindowBuffer *buffer);
int32_t l410_graphic_broker_destroy_surface_by_window_id(L410GraphicBroker *broker, int64_t window_id);
int32_t l410_graphic_broker_remove_surface_node(L410GraphicBroker *broker, uint64_t display_id, int32_t window_id);
int32_t l410_graphic_broker_clean_all_surface_map(L410GraphicBroker *broker);
int32_t l410_graphic_broker_set_window_task_id(L410GraphicBroker *broker, int32_t window_id, int32_t task_id);
int32_t l410_graphic_broker_notify_task_started(L410GraphicBroker *broker, int32_t task_id);
int32_t l410_graphic_broker_notify_task_destroyed(L410GraphicBroker *broker, int32_t task_id);
int32_t l410_graphic_broker_set_alpha_with_window_id(L410GraphicBroker *broker, float alpha, int32_t window_id);
int32_t l410_graphic_broker_set_relative_window_id(L410GraphicBroker *broker, uint64_t relative, int32_t window_id);
int32_t l410_graphic_broker_set_parent_canvas_node_position(L410GraphicBroker *broker, int32_t x, int32_t y, int32_t z);
int32_t l410_graphic_broker_set_display_rotation(L410GraphicBroker *broker, int32_t degrees);
int32_t l410_graphic_broker_set_active_display_size(L410GraphicBroker *broker, int32_t width, int32_t height);
int32_t l410_graphic_broker_set_refresh_rate(L410GraphicBroker *broker, int32_t hz);
int32_t l410_graphic_broker_set_task_property(L410GraphicBroker *broker, int32_t task_id, float alpha,
                                              int32_t x, int32_t y);
int32_t l410_graphic_broker_set_animation_status(L410GraphicBroker *broker, int32_t state);
int32_t l410_graphic_broker_set_engine_status(L410GraphicBroker *broker, int32_t state);   /* anco: SetAncoStatus */

/* ---- 与 anco SurfaceViewListenerStub 一一对应（窗口生命周期/属性） ---- */
int32_t l410_window_create(L410GraphicBroker *broker, int32_t window_id, uint32_t width, uint32_t height, int32_t task_id);
int32_t l410_window_show(L410GraphicBroker *broker, int32_t window_id);
int32_t l410_window_hide(L410GraphicBroker *broker, int32_t window_id);
int32_t l410_window_remove(L410GraphicBroker *broker, int32_t window_id);
int32_t l410_window_update_zindex(L410GraphicBroker *broker, int32_t window_id, int32_t z_index);
int32_t l410_window_update_property(L410GraphicBroker *broker, int32_t window_id, const L410WindowProperty *property);
int32_t l410_window_update_background_color(L410GraphicBroker *broker, int32_t window_id, uint32_t argb);
int32_t l410_window_set_title(L410GraphicBroker *broker, int32_t window_id, const char *title);

/* ---- 容器侧连接（宿主 broker <-> 容器显示/输入服务） ---- */
int32_t l410_graphic_broker_accept_engine(L410GraphicBroker *broker, const char *endpoint);
int32_t l410_graphic_broker_poll(L410GraphicBroker *broker, int timeout_ms);
int32_t l410_graphic_broker_frame_done(L410GraphicBroker *broker, int32_t window_id, uint64_t sequence, int32_t status);

#ifdef __cplusplus
}
#endif

#endif /* L410_GRAPHIC_BROKER_H */
