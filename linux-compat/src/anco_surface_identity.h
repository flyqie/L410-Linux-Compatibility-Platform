#ifndef ANCO_SURFACE_IDENTITY_H
#define ANCO_SURFACE_IDENTITY_H
#include <stdbool.h>
#include <stdint.h>
/* Linux semantic equivalent of original Java SurfaceControl identity generator.
 * Only the real surface creation owner calls this at the original generation
 * point (Builder skips it when Window == -1). Uses THIS process's actual PID,
 * never an app PID override, Wayland object ID, Window or Task as the identity.
 * One process-wide sequence, starting1; do not allocate an instance per window.
 * Inputs are actual Task/shared state and parsed original buffershared property
 * captured by the controller, not guessed defaults. No property reads/writes.
 * Returns identity only: no transaction/apply, object, registry or host call.
 * Caller retains it on the real content-surface record through retirement.
 * Native auxiliary Surface generation is a separate original path. */
uint64_t anco_surface_identity_generate(int32_t actual_task,
    bool explicitly_shared,int32_t original_shared_flag);
#endif
