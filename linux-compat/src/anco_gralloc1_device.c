#include "anco_gralloc1_device.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>

_Static_assert(sizeof(native_handle_t)==12,"original native handle header ABI");
_Static_assert(sizeof(hw_device_t)==120,"original 64-bit device ABI");
_Static_assert(sizeof(gralloc1_device_t)==136,"original 64-bit gralloc1 ABI");
struct AncoGralloc1Device { AncoGralloc1Api api; };

int anco_gralloc1_device_open(const hw_module_t *module,
    AncoGralloc1Device **out,AncoGralloc1OpenReport *report)
{
    if(out)*out=NULL;
    if(report)memset(report,0,sizeof(*report));
    if(!out||!report||!module){if(report)report->local_error=-EINVAL;return -EINVAL;}
    if(module->tag!=HARDWARE_MODULE_TAG||
       module->module_api_version!=GRALLOC_MODULE_API_VERSION_1_0||
       !module->id||strcmp(module->id,GRALLOC_HARDWARE_MODULE_ID)||
       !module->methods||!module->methods->open){report->local_error=-EPROTO;return -EPROTO;}
    AncoGralloc1Device *self=calloc(1,sizeof(*self));
    if(!self){report->local_error=-ENOMEM;return -ENOMEM;}
    hw_device_t *base=NULL;
    report->open_called=true;
    report->open_status=module->methods->open(module,GRALLOC_HARDWARE_MODULE_ID,&base);
    int error=-EREMOTEIO;
    if(report->open_status||!base)goto fail;
    gralloc1_device_t *device=(gralloc1_device_t *)base;
    error=-EPROTO;
    if(base->tag!=HARDWARE_DEVICE_TAG||base->version!=GRALLOC_MODULE_API_VERSION_1_0||
       !base->close||!device->getFunction)goto fail;
    self->api.device=device;
#define RESOLVE(field,ID) do { \
    gralloc1_function_pointer_t raw=device->getFunction(device,GRALLOC1_FUNCTION_##ID); \
    _Static_assert(sizeof(self->api.field)==sizeof(raw),"original function pointer ABI"); \
    memcpy(&self->api.field,&raw,sizeof(raw)); \
    if(!self->api.field){report->missing_function=GRALLOC1_FUNCTION_##ID;error=-ENOTSUP;goto fail;} \
} while(0)
    RESOLVE(create,CREATE_DESCRIPTOR);RESOLVE(destroy,DESTROY_DESCRIPTOR);
    RESOLVE(dimensions,SET_DIMENSIONS);RESOLVE(format,SET_FORMAT);
    RESOLVE(producer,SET_PRODUCER_USAGE);RESOLVE(consumer,SET_CONSUMER_USAGE);
    RESOLVE(allocate,ALLOCATE);RESOLVE(stride,GET_STRIDE);RESOLVE(release,RELEASE);
#undef RESOLVE
    *out=self;
    return 0;
fail:
    if(base&&base->close){report->close_called=true;report->close_status=base->close(base);}
    free(self);report->local_error=error;return error;
}
const AncoGralloc1Api *anco_gralloc1_device_api(AncoGralloc1Device *self)
{
    return self?&self->api:NULL;
}
int anco_gralloc1_device_close(AncoGralloc1Device *self)
{
    if(!self)return -EINVAL;
    int status=self->api.device->common.close(&self->api.device->common);
    free(self);
    return status;
}
