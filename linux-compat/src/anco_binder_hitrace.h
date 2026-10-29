/* Original OHOS Binder HiTrace envelope, not an AnCo business field.
 * Host6.0 libipc_single HitraceInvoker::TraceClientSend (0x78f3c) appends
 * the padded trace bytes and a four-byte padded uint8 length, setting 0x80.
 * TraceServerReceieve (0x79390) restores the business size before dispatch.
 * This private Linux transport performs that size restoration only; it does
 * not fabricate, consume as business data, or propagate a native trace span.
 */
#ifndef ANCO_BINDER_HITRACE_H
#define ANCO_BINDER_HITRACE_H
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>

static inline size_t anco_binder_business_size(const void *data, size_t size,
                                               uint32_t native_flags)
{
    if ((native_flags & UINT32_C(0x80)) && data && size >= 4) {
        const uint8_t length = ((const uint8_t *)data)[size - 4];
        /* Same bound as original RecoveryDataAndFlag, also for an invalid
         * trace ID. A too-large length leaves the original size unchanged. */
        if ((size_t)length <= size - 4) {
            const size_t business = size - 4 - length;
            fprintf(stderr, "OHOS_HITRACE_ENVELOPE flags=%08x total=%zu "
                    "trace_bytes=%u business=%zu\n", native_flags, size,
                    (unsigned)length, business);
            return business;
        }
    }
    return size;
}
#endif
