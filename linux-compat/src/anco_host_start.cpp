// Linux manager binding to the current host's original native start API.
// Native object storage is never interpreted or patched. Its constructors,
// setters, parcel codec, start call and deleting destructors remain original.
#include "json.hpp"
#include "iremote_object.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <new>
#include <string>
#include <vector>

struct OriginalClient;
extern std::shared_ptr<OriginalClient> original_client()
    asm("_ZN4OHOS5AAFwk20AbilityManagerClient11GetInstanceEv");
extern "C" int original_start(OriginalClient *, const void *, const void *,
    OHOS::sptr<OHOS::IRemoteObject>, int, int)
    asm("_ZN4OHOS5AAFwk20AbilityManagerClient12StartAbilityERKNS0_4WantERKNS0_12StartOptionsENS_4sptrINS_13IRemoteObjectEEEii");
extern "C" void want_construct(void *) asm("_ZN4OHOS5AAFwk4WantC1Ev");
extern "C" void want_delete(void *) asm("_ZN4OHOS5AAFwk4WantD0Ev");
extern "C" void *want_element(void *, const std::string &, const std::string &,
    const std::string &, const std::string &)
    asm("_ZN4OHOS5AAFwk4Want14SetElementNameERKNSt3__h12basic_stringIcNS2_11char_traitsIcEENS2_9allocatorIcEEEESA_SA_SA_");
extern "C" void *want_action(void *, const std::string &)
    asm("_ZN4OHOS5AAFwk4Want9SetActionERKNSt3__h12basic_stringIcNS2_11char_traitsIcEENS2_9allocatorIcEEEE");
extern "C" void *want_entity(void *, const std::string &)
    asm("_ZN4OHOS5AAFwk4Want9AddEntityERKNSt3__h12basic_stringIcNS2_11char_traitsIcEENS2_9allocatorIcEEEE");
extern "C" void *want_int(void *, const std::string &, int)
    asm("_ZN4OHOS5AAFwk4Want8SetParamERKNSt3__h12basic_stringIcNS2_11char_traitsIcEENS2_9allocatorIcEEEEi");
extern std::string want_uri(const void *) asm("_ZNK4OHOS5AAFwk4Want5ToUriEv");
extern "C" void parcel_construct(void *) asm("_ZN4OHOS13MessageParcelC1Ev");
extern "C" void parcel_destroy(void *) asm("_ZN4OHOS13MessageParcelD1Ev");
extern "C" bool parcel_int(void *, int) asm("_ZN4OHOS6Parcel10WriteInt32Ei");
extern "C" bool parcel_bool(void *, bool) asm("_ZN4OHOS6Parcel9WriteBoolEb");
extern "C" bool parcel_string(void *, const std::string &)
    asm("_ZN4OHOS6Parcel11WriteStringERKNSt3__h12basic_stringIcNS1_11char_traitsIcEENS1_9allocatorIcEEEE");
extern "C" size_t parcel_size(const void *) asm("_ZNK4OHOS6Parcel11GetDataSizeEv");
extern "C" size_t parcel_remaining(const void *) asm("_ZNK4OHOS6Parcel16GetReadableBytesEv");
extern "C" uintptr_t parcel_data(const void *) asm("_ZNK4OHOS6Parcel7GetDataEv");
extern "C" bool parcel_rewind(void *, size_t) asm("_ZN4OHOS6Parcel10RewindReadEm");
extern "C" void *options_unmarshal(void *) asm("_ZN4OHOS5AAFwk12StartOptions13UnmarshallingERNS_6ParcelE");
extern "C" bool options_marshal(const void *, void *) asm("_ZNK4OHOS5AAFwk12StartOptions11MarshallingERNS_6ParcelE");
static void options_delete(void *object)
{
    // Current original StartOptions vtable 0xa6c8: address point +0x18,
    // deleting destructor +8 resolves to native 0x5a00. It is not exported.
    void **vtable;
    memcpy(&vtable, object, sizeof(vtable));
    void (*destroy)(void *);
    memcpy(&destroy, vtable + 1, sizeof(destroy));
    destroy(object);
}

struct NativeParcel {
    // Original GetParcelInfo and current native Proxy allocation: 0xe8 bytes.
    alignas(8) unsigned char storage[0xe8];
    NativeParcel() { parcel_construct(storage); }
    ~NativeParcel() { parcel_destroy(storage); }
};
struct NativeWant {
    void *object;
    // Original current Want::FromString/ParseUri allocate 0x250 and call C1.
    NativeWant() : object(::operator new(0x250)) { want_construct(object); }
    ~NativeWant() { want_delete(object); }
};
struct NativeOptions {
    void *object;
    explicit NativeOptions(void *parcel) : object(options_unmarshal(parcel)) {}
    ~NativeOptions() { if (object) options_delete(object); }
};

static nlohmann::json load(const char *name)
{
    FILE *file = fopen(name, "rb");
    if (!file) return nullptr;
    std::string bytes;
    char chunk[4096];
    size_t n;
    while ((n = fread(chunk, 1, sizeof(chunk), file))) {
        if (bytes.size() + n > 1024 * 1024) { fclose(file); return nullptr; }
        bytes.append(chunk, n);
    }
    const bool failed = ferror(file);
    fclose(file);
    return failed ? nlohmann::json(nullptr) : nlohmann::json::parse(bytes, nullptr, false);
}

int main(int argc, char **argv)
{
    if (argc != 6 || (strcmp(argv[1], "--roundtrip") && strcmp(argv[1], "--start"))) {
        fprintf(stderr, "usage: %s --roundtrip|--start ORIGINAL_BMS_JSON MODULE ABILITY MODE\n", argv[0]);
        return 64;
    }
    const bool start = !strcmp(argv[1], "--start");
    const int mode = !strcmp(argv[5], "102") ? 102 : !strcmp(argv[5], "1") ? 1 : 0;
    if (!mode) return 64;
    const auto record = load(argv[2]);
    if (!record.is_object() || !record.contains("name") || !record["name"].is_string() ||
        !record.contains("hapModuleInfos") || !record["hapModuleInfos"].is_array()) return 65;
    const std::string bundle = record["name"];
    nlohmann::json ability;
    for (const auto &module : record["hapModuleInfos"]) {
        if (!module.contains("abilityInfos") || !module["abilityInfos"].is_array()) continue;
        for (const auto &item : module["abilityInfos"]) {
            if (item.value("bundleName", "") == bundle && item.value("moduleName", "") == argv[3] &&
                item.value("name", "") == argv[4]) {
                if (!ability.is_null()) return 66; // ambiguous original identity
                ability = item;
            }
        }
    }
    if (!ability.is_object() || !ability.contains("supportWindowMode") ||
        !ability["supportWindowMode"].is_array() || ability["supportWindowMode"].empty() ||
        ability["supportWindowMode"].size() > 10) return 67;
    std::vector<int> modes;
    bool admitted = false;
    for (const auto &value : ability["supportWindowMode"]) {
        if (!value.is_number_integer()) return 67;
        const int v = value.get<int>();
        // Original SupportWindowMode is distinct from actual WindowMode.
        if (v < 0 || v > 2) return 67;
        modes.push_back(v);
        if (v == (mode == 102 ? 2 : 0)) admitted = true;
    }
    if (!admitted) return 68;
    NativeParcel encoded;
    void *p = encoded.storage;
    // Exact current native StartOptions codec, fully extracted from device:
    // mode/display/animation, eight dimensions, focus/hide, eight Used flags,
    // optional Process/StartWindow objects, real capability vector, requestId,
    // optional WindowCreateParams. Optional objects absent, no host rect override.
    bool ok = parcel_int(p, mode) && parcel_int(p, -1) && parcel_bool(p, true);
    for (int i = 0; i < 8; ++i) ok = ok && parcel_int(p, 0);
    ok = ok && parcel_bool(p, true) && parcel_bool(p, false);
    for (int i = 0; i < 8; ++i) ok = ok && parcel_bool(p, false);
    ok = ok && parcel_int(p, 0) && parcel_int(p, 0) && parcel_int(p, static_cast<int>(modes.size()));
    for (int v : modes) ok = ok && parcel_int(p, v);
    ok = ok && parcel_string(p, "") && parcel_int(p, 0) && parcel_rewind(p, 0);
    if (!ok) return 69;
    NativeOptions options(p);
    if (!options.object || parcel_remaining(p)) return 70;
    NativeParcel roundtrip;
    if (!options_marshal(options.object, roundtrip.storage) ||
        parcel_size(p) != parcel_size(roundtrip.storage) ||
        memcmp(reinterpret_cast<const void *>(parcel_data(p)),
            reinterpret_cast<const void *>(parcel_data(roundtrip.storage)), parcel_size(p))) return 71;
    NativeWant want;
    want_element(want.object, "", bundle, argv[4], argv[3]);
    want_action(want.object, "ohos.want.action.home");
    want_entity(want.object, "entity.system.home");
    want_int(want.object, "ancoLaunchWindowMode", mode);
    const auto uri = want_uri(want.object);
    printf("ORIGINAL_HOST_OPTIONS modes=%s actual_mode=%d codec_bytes=%zu roundtrip_bytes_equal=1\n",
        ability["supportWindowMode"].dump().c_str(), mode, parcel_size(p));
    printf("ORIGINAL_HOST_WANT %s\n", uri.c_str());
    fflush(stdout);
    if (!start) return 0;
    auto client = original_client();
    if (!client) return 72;
    // Original SA/shell overload permits an absent caller ability; no token copy.
    const int result = original_start(client.get(), want.object, options.object,
        OHOS::sptr<OHOS::IRemoteObject>(), -1, -1);
    printf("ORIGINAL_HOST_START result=%d\n", result);
    return result ? 73 : 0;
}
