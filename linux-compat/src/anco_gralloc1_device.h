/* Linux-local holder of ORIGINAL gralloc1 device and function pointers. */
#ifndef ANCO_GRALLOC1_DEVICE_H
#define ANCO_GRALLOC1_DEVICE_H
#include <hardware/gralloc1.h>
#include <stdbool.h>

typedef struct AncoGralloc1Device AncoGralloc1Device;
typedef struct {
    gralloc1_device_t *device;
    GRALLOC1_PFN_CREATE_DESCRIPTOR create;
    GRALLOC1_PFN_DESTROY_DESCRIPTOR destroy;
    GRALLOC1_PFN_SET_DIMENSIONS dimensions;
    GRALLOC1_PFN_SET_FORMAT format;
    GRALLOC1_PFN_SET_PRODUCER_USAGE producer;
    GRALLOC1_PFN_SET_CONSUMER_USAGE consumer;
    GRALLOC1_PFN_ALLOCATE allocate;
    GRALLOC1_PFN_GET_STRIDE stride;
    GRALLOC1_PFN_RELEASE release;
} AncoGralloc1Api;
typedef struct {
    int local_error;
    bool open_called,close_called;
    int open_status,close_status;
    int32_t missing_function;
} AncoGralloc1OpenReport;

/* Caller supplies a genuinely loaded ORIGINAL HMI module, and keeps that
 * module/library resident through device close and all original calls.
 * Returns 0 or Linux-local negative errno; raw HMI statuses remain separate
 * in report and are meaningful only if the corresponding call occurred.
 * No loading hooks, hardcoded library path, feature mask or host operation.
 * The selected nine gralloc1 operations are resolved from actual getFunction;
 * unsupported functions fail with their real descriptor in the report.
 */
int anco_gralloc1_device_open(const hw_module_t *actual_module,
    AncoGralloc1Device **out,AncoGralloc1OpenReport *report);
/* Borrowed until device close. These are original function pointers: callers
 * pass actual descriptors/dimensions/format/usage/handles and receive the
 * unmodified original return values, including NOT_SHARED on actual success.
 * Locally ALLOCATEd buffers have an original reference already; no duplicate
 * RETAIN/native_handle_close/delete may be inferred from this API.
 */
const AncoGralloc1Api *anco_gralloc1_device_api(AncoGralloc1Device *self);
/* Caller must finish and destroy all real descriptors, release all its real
 * buffer references, and drain outgoing/consumer uses BEFORE close. This is
 * not a buffer/slot/fence owner or a wait/completion mechanism. Serialized
 * caller context; no nested close or concurrent outstanding API calls.
 * Calls original HMI.close exactly once, retires this local holder, returns
 * actual close status unchanged. Loaded library remains owned by caller.
 */
int anco_gralloc1_device_close(AncoGralloc1Device *self);
#endif
