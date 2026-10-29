/* Linux-local wire encoding only. Host converter/VDI remain original. */
#ifndef ANCO_NATIVE_HANDLE_LEGACY_H
#define ANCO_NATIVE_HANDLE_LEGACY_H
#include <stddef.h>
/* Encode an actual SP68 allocation into the original 6.0 native transport
 * layout. Only linear RGBA8888/2FD is admitted; compression extensions are
 * rejected, never silently discarded. Allocation and FD ownership stay with
 * the original gralloc. Output contains borrowed FDs, not a new allocation.
 * Input/output must not overlap. No host API or OHOS reserve count changes. */
int anco_native_handle_legacy87_rgba(const void *source,size_t source_bytes,
    void *output,size_t output_capacity,size_t *output_bytes);
#endif
