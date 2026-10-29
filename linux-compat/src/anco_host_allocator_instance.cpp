// Private native compatibility process only. Select a named real original
// HDF allocator instance through the existing generated Get(name, mode) API.
// The original proxy constructs/validates itself; no allocator implementation,
// substituted object, version, Binder method or death recipient is supplied.
#include <string>
#include "hdi_base.h"

using NativeReference=OHOS::sptr<OHOS::HDI::HdiBase>;
static_assert(sizeof(NativeReference)==8);
extern NativeReference original_get0(const std::string &,bool)
    asm("_ZN4OHOS3HDI7Display6Buffer4V1_010IAllocator3GetERKNSt3__h12basic_stringIcNS5_11char_traitsIcEENS5_9allocatorIcEEEEb");
extern NativeReference original_get4(const std::string &,bool)
    asm("_ZN4OHOS3HDI7Display6Buffer4V1_410IAllocator3GetERKNSt3__h12basic_stringIcNS5_11char_traitsIcEENS5_9allocatorIcEEEEb");
extern __attribute__((visibility("default"))) NativeReference selected_get0(bool)
    asm("_ZN4OHOS3HDI7Display6Buffer4V1_010IAllocator3GetEb");
extern __attribute__((visibility("default"))) NativeReference selected_get4(bool)
    asm("_ZN4OHOS3HDI7Display6Buffer4V1_410IAllocator3GetEb");
NativeReference selected_get0(bool passthrough)
{
    return original_get0(passthrough?"allocator_service":"l410_linux_allocator_service",passthrough);
}
NativeReference selected_get4(bool passthrough)
{
    return original_get4(passthrough?"allocator_service":"l410_linux_allocator_service",passthrough);
}
