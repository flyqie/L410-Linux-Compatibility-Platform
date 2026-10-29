/* Opt-in diagnostic intercept: records allocation ABI, fails explicitly. */
#include <stdio.h>
struct l410_ahb_desc {uint32_t width,height,layers,format;uint64_t usage;uint32_t stride,reserved0;uint64_t reserved1;};
static int l410_trace_ahb_allocate(const struct l410_ahb_desc *d,void **out){
 if(out)*out=NULL;
 if(!d||!out)return -EINVAL;
 fprintf(stderr,"L410_AHB_REQUEST width=%u height=%u layers=%u format=%u usage=0x%llx stride=%u reserved0=%u reserved1=%llu\n",d->width,d->height,d->layers,d->format,(unsigned long long)d->usage,d->stride,d->reserved0,(unsigned long long)d->reserved1);
 return -ENOSYS;
}
