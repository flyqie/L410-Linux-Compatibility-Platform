#ifndef ANCO_IMF_CALLBACK_RECEIVER_H
#define ANCO_IMF_CALLBACK_RECEIVER_H
#include "anco_imf_callback_codec.h"
#include "anco_imf_publication.h"
typedef struct AncoImfCallbackReceiver AncoImfCallbackReceiver;
typedef struct {
    gint32 status, index;
    const gunichar2 *text;
    gsize units;
} AncoImfCallbackResult;
typedef struct {
    /* Same serialized common runtime/current editor. Result begins {-1,0,NULL,0}.
     * Preserve original business status. Text borrowed until reply serialization
     * immediately after return; never return stack storage. No nested dispatch,
     * free, private datagrams, synthetic editor or successful absent service.
     * Original 1004/1005 have no arguments; result is ignored and the reply is
     * empty. Consumer routes to actual service onWindowShown/hideImeWindow,
     * not to ordinary status8 or an unconditional host show/hide echo. */
    void (*consume)(const AncoImfCallbackMessage *, const AncoCallbackHeader *,
                    AncoImfCallbackResult *, void *);
    const char *(*exception_trace)(void *);
} AncoImfCallbackReceiverOps;
/* Creates real local object on prepared aidl2 IPC, not an SA registration.
 * Ops copied, data borrowed. Ordinary IMF has no WMS appId authorization rule.
 * This does not bypass driver/SA policy. Message/command array borrowed only
 * during consume; strings remain borrowed from the actual request. */
AncoImfCallbackReceiver *anco_imf_callback_receiver_new(GBinderIpc *,
    const AncoImfCallbackReceiverOps *, void *);
GBinderLocalObject *anco_imf_callback_receiver_ref_object(AncoImfCallbackReceiver *);
/* Original publication on the receiver's actual IPC/object. The runtime must
 * already be the sole authorized owner of this SA namespace and must register
 * the original broker death listener BEFORE publish. No absence inference or
 * replacement of an existing unrelated registration is performed here.
 * Owner thread only. Reentrant publication/withdrawal/free returns -EBUSY.
 * Any attempted publication retains this receiver until withdrawal is decoded
 * as successful, even after a transport error or service refusal. No retry of
 * add while retained. A local preflight failure does not create publication.
 * Retain consumer data too; callbacks can occur during synchronous SAMgr calls.
 * Publication success does not mean editor/window readiness. */
int anco_imf_callback_receiver_publish(AncoImfCallbackReceiver *,
                                     AncoImfPublicationResult *);
int anco_imf_callback_receiver_withdraw(AncoImfCallbackReceiver *,
                                      AncoImfPublicationResult *);
/* After successful withdrawal, unregister the broker listener before free.
 * -EBUSY while exported/ambiguous or in callback; -EPERM on wrong thread.
 * Does not implicitly remove SA, kill any process or release borrowed data. */
int anco_imf_callback_receiver_free(AncoImfCallbackReceiver *);
#endif
