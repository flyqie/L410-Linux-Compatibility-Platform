#include "anco_surface_identity.h"
#include <stdatomic.h>
#include <unistd.h>

static _Atomic(uint32_t) surface_sequence=1;
uint64_t anco_surface_identity_generate(int32_t task,bool shared,int32_t flag)
{
    uint64_t identity=(uint64_t)(uint32_t)getpid()<<32;
    const bool derived=flag!=0 && task!=-1 && (flag==3 || shared);
    if(shared || derived){
        uint32_t sequence=atomic_fetch_add(&surface_sequence,UINT32_C(1));
        /* Java int-to-long sign extension before OR, not unsigned packing.
         * Use unsigned arithmetic to preserve wrap without C signed overflow. */
        uint64_t widened=sequence;
        if(sequence&UINT32_C(0x80000000))widened|=UINT64_C(0xffffffff00000000);
        identity|=widened;
    }
    return identity;
}
