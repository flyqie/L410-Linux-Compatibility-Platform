#include "anco_imf_publication.h"
#include "gbinder_ipc.h"
#include "gbinder_local_object_p.h"
#include "gbinder_local_request_p.h"
#include "gbinder_rpc_protocol.h"
#include <errno.h>
#include <string.h>

static int publish(GBinderIpc *ipc, GBinderLocalObject *object, gboolean add,
                   AncoImfPublicationResult *result)
{
    if (!result) return -EINVAL;
    *result = (AncoImfPublicationResult){.service_status=-1};
    if (!ipc || (add && (!object || object->ipc != ipc))) return -EINVAL;
    const GBinderRpcProtocol *protocol=gbinder_ipc_protocol(ipc);
    if (!protocol || strcmp(protocol->name,"aidl2")) return -EPROTONOSUPPORT;
    GBinderLocalRequest *request=gbinder_local_request_new(
        gbinder_ipc_io(ipc),protocol,NULL);
    if (!request) return -ENOMEM;
    gbinder_local_request_append_int32(request,0x100);
    gbinder_local_request_append_int32(request,0);
    gbinder_local_request_append_string16(request,"ohos.samgr.accessToken");
    gbinder_local_request_append_int32(request,66875);
    if (add) {
        gbinder_local_request_append_local_object(request,object);
        gbinder_local_request_append_int32(request,0);
        gbinder_local_request_append_int32(request,8);
        gbinder_local_request_append_string16(request,"");
        gbinder_local_request_append_string16(request,"");
    }
    result->attempted=TRUE;
    int status=-1;
    GBinderRemoteReply *reply=gbinder_ipc_transact_sync_reply_main(
        ipc,0,add?3:4,request,&status);
    result->transport_status=status;
    if (!status && reply) {
        GBinderReader reader;
        gbinder_remote_reply_init_reader(reply,&reader);
        result->reply_decoded=gbinder_reader_read_int32(&reader,
            &result->service_status);
    }
    gbinder_remote_reply_unref(reply);
    gbinder_local_request_unref(request);
    return status ? status : result->reply_decoded ? result->service_status : -1;
}
int anco_imf_publication_add(GBinderIpc *ipc, GBinderLocalObject *object,
                           AncoImfPublicationResult *result)
{ return publish(ipc,object,TRUE,result); }
int anco_imf_publication_remove(GBinderIpc *ipc, AncoImfPublicationResult *result)
{ return publish(ipc,NULL,FALSE,result); }
