#include "anco_native_handle_legacy.h"
#include <errno.h>
#include <stdint.h>
#include <string.h>

static uint32_t u32(const unsigned char *p,size_t offset)
{uint32_t value;memcpy(&value,p+offset,sizeof(value));return value;}
static uint64_t u64(const unsigned char *p,size_t offset)
{uint64_t value;memcpy(&value,p+offset,sizeof(value));return value;}

int anco_native_handle_legacy87_rgba(const void *source,size_t bytes,
    void *output,size_t capacity,size_t *output_bytes)
{
    if(output_bytes)*output_bytes=0;
    if(!source||!output||!output_bytes)return -EINVAL;
    if(bytes!=0x188||capacity<0x170)return -EMSGSIZE;
    const unsigned char *in=source;
    if(u32(in,0)!=12||u32(in,4)!=2||u32(in,8)!=93||
        u32(in,0x14)!=0x03141592)return -EPROTO;
    /* Both original ConvertFormatUsage bodies use internal format@0x20.
     * Exactly1 is linear RGBA; no AFBC/HEBC/HFBC modifier is representable
     * through this limited encoder. Never reinterpret compressed storage. */
    if(u64(in,0x20)!=1||u32(in,0x18)!=1)return -ENOTSUP;
    /* Original SP68 SetHandleHebcInfo14d28 writes two additional planes into
     * 134..14b. Constructor12e08 clears them. Verify the actual allocation,
     * rather than assume CPU usage or zero metadata implies linear storage. */
    for(size_t offset=0x134;offset<0x14c;offset+=4)
        if(u32(in,offset))return -ENOTSUP;
    /* Common fields keep their original bytes. Original 6.0/new converters:
     * HEBC payload stride134<-14c; producer148<-160; consumer150<-168;
     * plane metadata158<-170; attr offset15c<-174; IOMMU168<-180.
     * All remaining common trailer bytes also retain their actual values.
     * This encodes a separate versioned transport object. It does not patch
     * the live93-int gralloc allocation or shorten a65-int OHOS handle. */
    memcpy(output,in,0x134);
    memcpy((unsigned char *)output+0x134,in+0x14c,0x3c);
    const uint32_t legacy_ints=87;
    memcpy((unsigned char *)output+8,&legacy_ints,sizeof(legacy_ints));
    *output_bytes=0x170;
    return 0;
}
