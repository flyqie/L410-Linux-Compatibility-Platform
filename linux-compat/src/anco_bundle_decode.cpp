// Host-native original BundleInfo parser bridge. No reimplemented schema.
// Input is the inline GetBundleInfo response envelope; output is original JSON.
#include "json.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace OHOS::AppExecFwk {
class BundleInfo;
void to_json(nlohmann::json &, const BundleInfo &);
}
extern "C" void parcel_construct(void *) asm("_ZN4OHOS13MessageParcelC1Ev");
extern "C" void parcel_destroy(void *) asm("_ZN4OHOS13MessageParcelD1Ev");
extern "C" bool parcel_parse(void *, uintptr_t, size_t) asm("_ZN4OHOS6Parcel9ParseFromEmm");
extern "C" int32_t parcel_int32(void *) asm("_ZN4OHOS6Parcel9ReadInt32Ev");
extern "C" OHOS::AppExecFwk::BundleInfo *original_unmarshal(void *)
    asm("_ZN4OHOS10AppExecFwk10BundleInfo13UnmarshallingERNS_6ParcelE");
static void original_delete(OHOS::AppExecFwk::BundleInfo *object)
{
    // Original client default_delete, 0x16f10/76: vptr + 8 deleting destructor.
    void **vtable;
    memcpy(&vtable, object, sizeof(vtable));
    using Delete = void (*)(OHOS::AppExecFwk::BundleInfo *);
    Delete destroy;
    memcpy(&destroy, vtable + 1, sizeof(destroy));
    destroy(object);
}
// Original GetParcelInfo allocates 0xe8 bytes and calls these exact constructors.
// All member access remains in the original library. No locally defined vtable.
struct NativeParcel {
    alignas(8) unsigned char storage[0xe8];
    NativeParcel() { parcel_construct(storage); }
    ~NativeParcel() { parcel_destroy(storage); }
};
static_assert(NLOHMANN_JSON_VERSION_MAJOR == 3 && NLOHMANN_JSON_VERSION_MINOR == 11 &&
    NLOHMANN_JSON_VERSION_PATCH == 3, "original exported JSON ABI");

int main()
{
    std::vector<unsigned char> bytes;
    unsigned char chunk[4096];
    for (;;) {
        size_t n = fread(chunk, 1, sizeof(chunk), stdin);
        if (bytes.size() + n > 32768 + 12) return 65;
        bytes.insert(bytes.end(), chunk, chunk + n);
        if (n < sizeof(chunk)) { if (ferror(stdin)) return 66; break; }
    }
    if (bytes.size() < 16) return 67;
    int32_t result; uint32_t size, raw_size;
    memcpy(&result, bytes.data(), 4);
    memcpy(&size, bytes.data() + 4, 4);
    memcpy(&raw_size, bytes.data() + 8, 4);
    // Original ReadRawData inline case only. Never interpret an FD as bytes.
    if (result || size != raw_size || size > 32768 || size < 4 ||
        bytes.size() != static_cast<size_t>(size) + 12) return 68;
    void *data = malloc(size);
    if (!data) return 69;
    memcpy(data, bytes.data() + 12, size);
    NativeParcel parcel;
    if (!parcel_parse(parcel.storage, reinterpret_cast<uintptr_t>(data), size)) {
        free(data); return 70;
    }
    if (parcel_int32(parcel.storage) != 1) return 71; // original ReadParcelable presence
    auto *bundle = original_unmarshal(parcel.storage);
    if (!bundle) return 72;
    nlohmann::json record;
    OHOS::AppExecFwk::to_json(record, *bundle);
    original_delete(bundle);
    const std::string output = record.dump();
    if (fwrite(output.data(), 1, output.size(), stdout) != output.size()) return 73;
    return fputc('\n', stdout) == EOF ? 73 : 0;
}
