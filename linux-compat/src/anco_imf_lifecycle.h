/* Linux-side composition of retained AncoImeManager outbound lifecycle.
 * Not an editor admission policy, callback service or complete IME manager. */
#ifndef ANCO_IMF_LIFECYCLE_H
#define ANCO_IMF_LIFECYCLE_H
#include "anco_imf_broker.h"
#include "anco_ime_editor.h"
#include <stdatomic.h>
/* Shared original StatusUtils flags. Initialize once, not on editor changes.
 * Callback/service consumers may update flags during a synchronous transaction. */
typedef struct {
    atomic_bool show, hide, window_hide, has_attach;
} AncoImeStatus;
typedef struct {
    AncoImfBroker *broker; /* borrowed; must outlive all operations */
    AncoImeStatus *status; /* borrowed; shared with original callback semantics */
    AncoImeEditor last_editor;
    bool has_editor, support_new_attach, support_new_show;
    GThread *owner;
} AncoImfLifecycle;
typedef struct {
    AncoImfAttachResult attach[2];
    unsigned attach_count;
    AncoImfShowResult show;
    AncoImfCallResult direct;
} AncoImfLifecycleResult;
/* All lifecycle operations on broker owner thread. State/storage must remain
 * alive until calls including reverse callbacks return. No implicit teardown. */
void anco_ime_status_init(AncoImeStatus *status);
bool anco_imf_lifecycle_init(AncoImfLifecycle *state, AncoImfBroker *broker,
                             AncoImeStatus *status);
bool anco_imf_lifecycle_update_editor(AncoImfLifecycle *state,
                                      const AncoImeEditor *editor);
/* Commands are already parsed original private commands, borrowed for call.
 * NULL editor follows original: fresh zero attribute, no private commands.
 * Restart admission is performed by the service before onStartInput. */
bool anco_imf_lifecycle_start(AncoImfLifecycle *state, const AncoImeEditor *editor,
    const AncoImfPrivateCommand *const *commands, size_t count,
    AncoImfLifecycleResult *result);
/* Original Utils.isOnDeviceCollaboration is queried only after resolving the
 * actual manager and setting show=true. Do not pass a snapshot sampled before
 * Binder resolution: a synchronous reverse callback may have changed it.
 * Query is mandatory, reads the actual shared mode on the owner thread, and
 * must not destroy/reenter the lifecycle. data remains alive for this call.
 * Missing manager performs no query. This is a Linux-local getter, no IPC. */
typedef bool (*AncoImeCollaborationQuery)(void *data);
bool anco_imf_lifecycle_shown(AncoImfLifecycle *state,
    AncoImeCollaborationQuery collaboration, void *data,
    int32_t motion_tool_type, AncoImfLifecycleResult *result);
bool anco_imf_lifecycle_hidden(AncoImfLifecycle *state,
    AncoImfLifecycleResult *result);
/* Actual AncoImeService-equivalent fields, owned by the common input service.
 * notification_shown is mIsWindowShown, NOT isInputViewShown, actual host
 * visibility or isShowInputRequested. height is mImeHieght, not a guessed
 * application height. Runtime supplies/maintains actual service values.
 * This is not a second independently initialized visibility cache. */
typedef struct {
    bool notification_shown;
    int32_t height;
    int32_t last_display_mode_height;
} AncoImeWindowState;
/* Original notifyImeHeight filtering before updateImeHeightSafely. Both
 * height fields belong to the actual service. Initialize last at actual input
 * view creation from current height, not on every notification/editor change.
 * update is mandatory and implements the actual original height-application
 * path (scheduling/cache/layout); do not supply a no-op or direct host hide.
 * It is invoked on the owner with last_display_mode_height already updated.
 * The callback/data/state must outlive nested calls, and must not destroy the
 * manager. It owns current-height changes; they are never overwritten here.
 * True means valid local handling, including original suppression branches,
 * NOT layout/display success. No flags, cache policy or clipping is invented. */
typedef void (*AncoImeHeightUpdate)(void *data, int32_t height, bool by_self);
bool anco_imf_service_notify_height(AncoImfLifecycle *manager,
    AncoImeWindowState *window, int32_t height, bool by_self,
    AncoImeHeightUpdate update, void *data);
/* Original service onWindowHidden and hideImeWindow respectively. The latter
 * is the service target for callback1005, NOT an unconditional WMS reset.
 * Fields and manager/status remain alive on the manager owner through nested
 * callbacks. Clear notification_shown before manager.hidden; hideImeWindow
 * additionally sets window_hide iff current height!=0, without clearing it at
 * zero. Neither clears height/has_attach or detaches the editor. Local invalid
 * arguments/thread fail before state changes. Transport diagnostics are kept;
 * successful local dispatch does not mean host visibility changed.
 * These functions do not publish a service or implement show/height layout. */
bool anco_imf_service_window_hidden(AncoImfLifecycle *manager,
    AncoImeWindowState *window, AncoImfLifecycleResult *result);
bool anco_imf_service_hide_window(AncoImfLifecycle *manager,
    AncoImeWindowState *window, AncoImfLifecycleResult *result);
bool anco_imf_lifecycle_unbind(AncoImfLifecycle *state, bool keyboard_shown,
    AncoImfLifecycleResult *result);
/* Bool return means local owner/admission was valid, NOT host success.
 * Inspect per-call results; flags never stand in for actual host acceptance.
 * finish-input, death listener and callback publication are separate owners. */
/* Guest-local semantic provider, NOT a host wire interface. Borrowed for call;
 * provider/data and status must survive all nested callbacks. Invoke on the
 * lifecycle owner thread. These requests must go through the real input-service
 * admission/current-editor path, never be direct host show/hide echoes.
 * is_shown means guest input-service visibility, not host transport success.
 * The ordinary status/height functions below are required for those calls;
 * the two explicitly optional window entrypoints have a separate contract.
 * NULL service means
 * no current service. No Binder publication or editor identity is supplied here. */
typedef struct {
    void *data;
    bool is_anco_service;
    bool (*is_shown)(void *);
    bool (*hardware_keyboard)(void *);
    bool (*collaboration)(void *);
    void (*request_show)(void *, int32_t flags);
    void (*request_hide)(void *, int32_t flags);
    void (*notify_height)(void *, int32_t height, bool by_self);
    /* Original AncoImeProxy 1004/1005 service entrypoints. Optional until the
     * actual service implements them; NULL is not successful delivery.
     * These are onWindowShown/hideImeWindow, NOT request_show/request_hide
     * or direct manager calls. The service owns preset height, its distinct
     * notification visibility and original manager lifecycle. No wire change. */
    void (*on_window_shown)(void *);
    void (*hide_ime_window)(void *);
} AncoImeServiceView;
int32_t anco_ime_receive_status(AncoImeStatus *status,
    const AncoImeServiceView *service, int32_t keyboard_status);
int32_t anco_ime_receive_height(AncoImeStatus *status,
    const AncoImeServiceView *service, int32_t height);
#endif
