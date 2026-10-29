// Original SP68 RS GetRogScreenResolution wire/error semantics, old-host ABI.
// Donor RSInterfaces19f8f8, RSRenderServiceClient7e9c28,
// RSClientToServiceConnectionProxy807594/820. No substituted screen mode,
// fabricated failure/success or new Binder interface. Execute only on L410.
#include <cstdint>
#include <cstring>
#include <string>
#include "refbase.h"

namespace OriginalRsQueryAbi {
// Both retained native Parcel call sites reserve/zero exactly0xe8 bytes.
// These opaque buffers are constructed/destructed by the ORIGINAL provider.
struct alignas(8) NativeParcel { unsigned char bytes[0xe8]; };
struct NativeOption { uint32_t flags; int32_t wait; };
extern "C" void parcel_ctor(void *) asm("_ZN4OHOS13MessageParcelC1Ev");
extern "C" void parcel_dtor(void *) asm("_ZN4OHOS13MessageParcelD1Ev");
extern bool token(void *,std::u16string)
    asm("_ZN4OHOS13MessageParcel19WriteInterfaceTokenENSt3__h12basic_stringIDsNS1_11char_traitsIDsEENS1_9allocatorIDsEEEE");
extern "C" bool write_screen(void *,uint64_t) asm("_ZN4OHOS6Parcel11WriteUint64Em");
extern "C" bool read_status(void *,int32_t *) asm("_ZN4OHOS6Parcel9ReadInt32ERi");
extern "C" bool read_dimension(void *,uint32_t *) asm("_ZN4OHOS6Parcel10ReadUint32ERj");
extern "C" void option_ctor(void *,int,int) asm("_ZN4OHOS13MessageOptionC1Eii");
extern "C" int original_send(void *,uint32_t,void *,void *,void *)
    asm("_ZN4OHOS5Rosen30RSRenderServiceConnectionProxy11SendRequestEjRNS_13MessageParcelES3_RNS_13MessageOptionE");

// Current ORIGINAL Hub674420 returns an8-byte nontrivial sptr throughx8;
// donor Hub7f7ab0 returns16 bytes. Do not impose donor layout on host.
struct OriginalConnection {
    void *pointer;
    ~OriginalConnection()
    {
        if (!pointer) return;
        const void *table;
        intptr_t adjustment;
        std::memcpy(&table,pointer,sizeof(table));
        std::memcpy(&adjustment,static_cast<const char *>(table)-0x18,sizeof(adjustment));
        reinterpret_cast<OHOS::RefBase *>(static_cast<char *>(pointer)+adjustment)->DecStrongRef(this);
    }
};
static_assert(sizeof(OriginalConnection)==8);
static_assert(sizeof(NativeParcel)==0xe8);
static_assert(sizeof(NativeOption)==8);
extern OriginalConnection original_connection()
    asm("_ZN4OHOS5Rosen25RSRenderServiceConnectHub16GetRenderServiceEv");
struct ParcelOwner {
    NativeParcel object{};
    ParcelOwner() { parcel_ctor(&object); }
    ~ParcelOwner() { parcel_dtor(&object); }
};
}
using namespace OriginalRsQueryAbi;

extern "C" __attribute__((visibility("default")))
int original_rog_query(void *,uint64_t,uint32_t *,uint32_t *)
    asm("_ZN4OHOS5Rosen12RSInterfaces22GetRogScreenResolutionEmRjS2_");

int original_rog_query(void *,uint64_t screen,uint32_t *width,uint32_t *height)
{
    // Original wrapper/client ignores interface this and uses the static Hub.
    auto connection=original_connection();
    if (!connection.pointer) return 4;
    ParcelOwner data,reply;
    NativeOption option{};
    option_ctor(&option,0,8);
    if (!token(&data.object,u"ohos.rosen.ClientToServiceConnection")) return 6;
    if (!write_screen(&data.object,screen)) return 6;
    // Old SendRequest67605c is identical to donor's two Remote() acquisitions,
    // remote-null5, slot8 send and -0x20 virtual-base strong-ref release.
    if (original_send(connection.pointer,66,&data.object,&reply.object,&option)!=0) return 2;
    int32_t status=0;
    if (!read_status(&reply.object,&status)) return 12;
    // Original writes directly, including partial output on a later read error.
    if (!read_dimension(&reply.object,width)) return 12;
    if (!read_dimension(&reply.object,height)) return 12;
    return status;
}
