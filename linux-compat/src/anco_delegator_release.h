/* Original host code3 body decoding, with explicit Linux FD ownership. */
#ifndef ANCO_DELEGATOR_RELEASE_H
#define ANCO_DELEGATOR_RELEASE_H
#include <gbinder.h>
#define ANCO_DELEGATOR_RELEASE_TRANSACTION 3

typedef struct {
    gint32 slot;
    gint32 has_fence;
    int owned_fd; /* actual dup result, including 0; close/transfer exactly once */
    int fence_fd; /* original guest semantic value: owned_fd>0, otherwise -1 */
    int dup_error; /* negative errno on dup failure, otherwise 0 */
} AncoDelegatorRelease;
#define ANCO_DELEGATOR_RELEASE_INIT {0, 0, -1, -1, 0}

/* Caller has validated original sender/token/code3 and positioned a top-level
 * reader at its body. Initialize out with INIT; clear its previous owned FD
 * before reuse. Preserve raw slot/hasFence, and ignore trailing bytes as stock.
 * 0 means decoded, NOT fence signaling or buffer reuse. Original dup failure
 * still decodes with fence_fd=-1, while dup_error exposes the real local failure.
 * A malformed/truncated message is negative errno with out unchanged; its reader
 * must then be discarded. No callback registration, state update, wait or release.
 */
int anco_delegator_read_release(GBinderReader *reader, AncoDelegatorRelease *out);
void anco_delegator_release_clear(AncoDelegatorRelease *release);
#endif
