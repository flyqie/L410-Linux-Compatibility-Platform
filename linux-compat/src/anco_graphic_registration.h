/* Single original CreateSurfaceNode registration, not producer readiness. */
#ifndef ANCO_GRAPHIC_REGISTRATION_H
#define ANCO_GRAPHIC_REGISTRATION_H
#include "anco_graphic_transport.h"
#define ANCO_GRAPHIC_CREATE_SURFACE_NODE 2
#define ANCO_GRAPHIC_REMOVE_SURFACE_NODE 5

/* Original body: uint64 identity, int32 window, Android String8, strong Binder.
 * Only already registered real local objects belong here. The caller keeps
 * delegator alive through every use of this request: the gbinder writer doesn't
 * hold an object reference. No Task/window ID allocation or interpretation.
 * NULL name and NULL delegator fail before appending any byte or offset.
 */
int anco_graphic_append_create_surface_node(GBinderLocalRequest *request,
    guint64 identity, gint32 window_id, const char *name,
    GBinderLocalObject *delegator);

/* Borrow resolved original graphic and real registered local delegator. Holds
 * a delegator reference until the synchronous call and request cleanup finish.
 * TRUE means transport0 and a decoded service int, NOT service_result==0,
 * SendSelfProxy/code5 receipt, producer binding or any displayed/GPU frame.
 * Never retries or changes SA/protocol; caller configures the original driver.
 * decoded/service_result preserve any actual reply int even with a nonzero
 * transport status, as the original proxy reads unconditionally. After a
 * request was sent, its original return is decoded?service_result:0; that
 * default is NOT evidence of delivery. Local pre-send errors are separate.
 */
gboolean anco_graphic_create_surface_node_once(GBinderRemoteObject *graphic,
    guint64 identity, gint32 window_id, const char *name,
    GBinderLocalObject *delegator, AncoGraphicResult *out);

typedef struct {
    AncoGraphicResult wire;
    gint32 original_return;
} AncoGraphicRemoveResult;
/* Original RemoveSurfaceNode: uint64 identity, int32 window, code5/flags0.
 * Original reads the reply even after transport failure; missing int defaults
 * to0. Preserve that return separately from actual delivery diagnostics.
 * Caller supplies retained original identities and the original lifecycle
 * trigger. No automatic child-exit removal, retry, synthetic code12, buffer
 * release, fence wait or completion is performed by this single call.
 * TRUE requires transport0 and decoded int, not service_result==0.
 */
gboolean anco_graphic_remove_surface_node_once(GBinderRemoteObject *graphic,
    guint64 identity, gint32 window_id, AncoGraphicRemoveResult *out);
#endif
