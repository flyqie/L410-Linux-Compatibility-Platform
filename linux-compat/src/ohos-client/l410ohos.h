/* HarmonyOS surface client for Linux toolkits.
 *
 * One ArkUI XComponent owns one OHOS NativeWindow. This library is the Linux
 * side of that window: it receives the graphic buffers the XComponent hands
 * out, exposes them as plain memory the toolkit paints into, and returns the
 * finished frame to the OHOS compositor. The toolkit keeps drawing with its
 * own code - nothing here reads pixels back, shadows a virtual display or
 * synthesises events.
 *
 * Frame channel (binary, request driven):
 *   client -> window host : hello, then acquire/finish records
 *   window host -> client : buffer record + buffer fd (SCM_RIGHTS), result
 * Event channel (text lines, push):
 *   window host -> client : "input ...", "ime ...", "clipboard ...",
 *                           "geometry ...", "focus ...", "closed"
 *   client -> window host : "clipboard set ...", "ime ...", "title ..."
 *
 * Both channels are plain AF_UNIX stream sockets. The event socket is handed
 * to the toolkit's event loop as a file descriptor; nothing is ever polled on
 * a timer.
 */
#ifndef L410_OHOS_CLIENT_H
#define L410_OHOS_CLIENT_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define L410_OHOS_WIRE_HELLO    0x334e344cU /* 'L4N3' - client identifies itself */
#define L410_OHOS_ACQUIRE       0x4154344cU /* 'L4TA' */
#define L410_OHOS_BUFFER        0x4254344cU /* 'L4TB' */
#define L410_OHOS_FINISH        0x4654344cU /* 'L4TF' */
#define L410_OHOS_RESULT        0x5254344cU /* 'L4TR' */

/* 帧回调队列（默认路径）：宿主推 FRAME，容器画完 PRESENT，两端都不等对方。
 * 缓冲的 fd 只在第一次见到某个 slot 时传一次（SLOT），之后每帧只传几个字节。 */
#define L410_OHOS_WIRE_QUEUE   0x514e344cU /* 'L4NQ' - 队列模式：走帧回调，不用每帧请求 */
#define L410_OHOS_WANT          0x5754344cU /* 'L4TW' client -> host: 我要一帧 */
#define L410_OHOS_SLOT          0x534c344cU /* 'L4LS' host -> client: slot 描述 + fd（仅首次） */
#define L410_OHOS_FRAME         0x4652344cU /* 'L4RF' host -> client: 往 slot 里画，serial 递增 */
#define L410_OHOS_PRESENT       0x5052344cU /* 'L4RP' client -> host: 这一帧画好了（无应答） */
#define L410_OHOS_CANCEL        0x4352344cU /* 'L4RC' client -> host: 放弃未绘制帧 */
#define L410_OHOS_RESET         0x5352344cU /* 'L4RS' host -> client: Surface generation changed */

struct l410_ohos_want {
    uint32_t magic;
    uint32_t serial;
};

struct l410_ohos_slot {
    uint32_t magic;
    uint32_t index;
    int32_t width;
    int32_t height;
    int32_t stride;
    int32_t size;
    int32_t format;
};

struct l410_ohos_frame {
    uint32_t magic;
    uint32_t index;
    uint32_t serial;
    int32_t epoch;
};

struct l410_ohos_present {
    uint32_t magic;
    uint32_t index;
    uint32_t serial;
};

struct l410_ohos_reset {
    uint32_t magic;
    uint32_t generation;
    int32_t width;
    int32_t height;
};

struct l410_ohos_acquire {
    uint32_t magic;
    uint32_t serial;
    uint32_t epoch;
    uint32_t width;
    uint32_t height;
};

struct l410_ohos_buffer {
    uint32_t magic;
    uint32_t serial;
    uint32_t epoch;
    int32_t width;
    int32_t height;
    int32_t stride;
    int32_t size;
    int32_t format;
};

struct l410_ohos_finish {
    uint32_t magic;
    uint32_t serial;
    uint32_t submit;
};

struct l410_ohos_result {
    uint32_t magic;
    uint32_t serial;
    uint32_t epoch;
    int32_t status;
};

typedef struct L410Ohos L410Ohos;

/* Connects to the window host that owns `endpoint` (an abstract socket name
 * published by the ArkUI component). `width`/`height` are a first guess of the
 * surface geometry; the real size follows the acquire requests. */
L410Ohos *l410_ohos_open(const char *endpoint, int width, int height);
/* 同上，但声明自己是队列模式客户端（帧回调 + 每帧只传几个字节）。 */
L410Ohos *l410_ohos_open_queue(const char *endpoint, int width, int height);
/* Event channel only (focus, clipboard, input method) for a toolkit that does
 * not own a graphic buffer itself. */
L410Ohos *l410_ohos_open_event_channel(const char *event_endpoint);
void l410_ohos_close(L410Ohos *surface);

/* Asks the window host for the next graphic buffer. Returns 0 and leaves the
 * buffer mapped on success. Safe to call again after a failed present. */
int l410_ohos_acquire(L410Ohos *surface, int width, int height);

/* Mapped pixels of the acquired buffer, its row stride in bytes and its OHOS
 * pixel format. NULL before a successful acquire. */
void *l410_ohos_pixels(const L410Ohos *surface);
int l410_ohos_stride(const L410Ohos *surface);
int l410_ohos_format(const L410Ohos *surface);
int l410_ohos_width(const L410Ohos *surface);
int l410_ohos_height(const L410Ohos *surface);

/* Hands the frame to the compositor (submit != 0) or discards it. */
int l410_ohos_finish(L410Ohos *surface, int submit);

/* ------------------------------------------------------- 帧回调队列（默认路径）
 * 每帧不再请求缓冲、不再传 fd、不再等应答：工具包画完就 WANT 一帧，
 * 宿主推一条 FRAME{slot}，工具包直接写进早就映射好的那块内存，再 PRESENT 回去。
 * slot 的 fd 只在第一次出现时传一次。 */
#define L410_OHOS_SLOTS_MAX 4

/* 图形通道的 socket fd（工具包把它挂进自己的事件循环）。 */
int l410_ohos_surface_fd(const L410Ohos *surface);
int l410_ohos_connected(const L410Ohos *surface);
/* 队列通道上待处理记录的类型（诊断用）：0=没有，其它=魔数。 */
uint32_t l410_ohos_peek_magic(const L410Ohos *surface);
/* 告诉宿主"我这一帧有内容要画"（无应答）。 */
int l410_ohos_want(L410Ohos *surface);
/* 处理宿主推来的一条消息（非阻塞）。返回 1 = 收到 FRAME（slot 与 serial 有效），
 * 0 = 这一轮没消息，-1 = 通道断了。工具包在自己的事件循环里调用它。 */
int l410_ohos_poll_frame(L410Ohos *surface, int *slot, uint32_t *serial);
/* 某个 slot 的映射内存/行距；未映射返回 NULL/0。 */
void *l410_ohos_slot_pixels(L410Ohos *surface, int slot);
int l410_ohos_slot_stride(const L410Ohos *surface, int slot);
/* 声明某一帧画好了（无应答）。 */
int l410_ohos_present(L410Ohos *surface, int slot, uint32_t serial);
/* Return a dequeued slot without displaying it. */
int l410_ohos_cancel(L410Ohos *surface, int slot, uint32_t serial);
/* 队列模式下有效的尺寸（来自最近一条 FRAME 对应的 slot）。 */
int l410_ohos_queue_width(const L410Ohos *surface);
int l410_ohos_queue_height(const L410Ohos *surface);

/* Event channel. Returns 0 when the host accepted the connection. */
int l410_ohos_open_events(L410Ohos *surface, const char *event_endpoint);
/* Watched by the toolkit event loop; -1 when there is no event channel. */
int l410_ohos_event_fd(const L410Ohos *surface);
/* Non-blocking read of one text line; returns length, 0 when idle, -1 when the
 * channel is gone. `line` is NUL terminated. */
int l410_ohos_event_read(L410Ohos *surface, char *line, size_t size);
/* One already formatted line to the window host (input focus, clipboard, ...) */
int l410_ohos_event_send(L410Ohos *surface, const char *line);

/* Last error, for logs. */
const char *l410_ohos_error(const L410Ohos *surface);

/* ------------------------------------------------------------- engine channel
 * One channel per application process, owned by the compatibility engine's host
 * (the same role anco's graphic broker plays). Window level messages travel as
 * text lines:
 *   client -> host : WINDOW_CREATE id= w= h= title= / WINDOW_REMOVE id= /
 *                    WINDOW_TITLE id= title= / CLIPBOARD_SET id= text= /
 *                    IME_CURSOR id= x= y= w= h=
 *   host -> client : WINDOW_CREATED id= surface= w= h= density= /
 *                    WINDOW_GEOMETRY id= w= h= density= / WINDOW_FOCUS id= /
 *                    WINDOW_CLOSED id= / INPUT_POINTER id= ... / INPUT_KEY ... /
 *                    INPUT_WHEEL ... / DISPLAY_SIZE w= h= density= /
 *                    CLIPBOARD_TEXT id= text= / IME_COMMIT id= text=
 * Lines that arrive while waiting for another one are kept and handed out by
 * l410_engine_read() later, so nothing is lost and nothing is ever polled. */
typedef struct L410Engine L410Engine;

L410Engine *l410_engine_open(const char *endpoint);
void l410_engine_close(L410Engine *engine);
int l410_engine_fd(const L410Engine *engine);
int l410_engine_send(L410Engine *engine, const char *line);
/* Non-blocking read of one line; returns its length, 0 when idle, -1 when the
 * engine channel is gone. */
int l410_engine_read(L410Engine *engine, char *line, size_t size);
/* Waits for a line starting with `prefix` (milliseconds). Returns 1 and fills
 * `line` on a match, 0 on timeout, -1 when the channel closed. */
int l410_engine_wait(L410Engine *engine, const char *prefix, char *line, size_t size, int timeout_ms);
int l410_engine_connected(const L410Engine *engine);

#ifdef __cplusplus
}
#endif

#endif
