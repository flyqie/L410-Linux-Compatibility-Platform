#include "anco_delegator_release.h"
#include <errno.h>
#include <unistd.h>

int anco_delegator_read_release(GBinderReader *reader, AncoDelegatorRelease *out)
{
    if (!reader || !out) return -EINVAL;
    if (out->owned_fd >= 0) return -EBUSY;
    AncoDelegatorRelease decoded = ANCO_DELEGATOR_RELEASE_INIT;
    if (!gbinder_reader_read_int32(reader, &decoded.slot) ||
        !gbinder_reader_read_int32(reader, &decoded.has_fence)) return -EBADMSG;
    if (decoded.has_fence) {
        /* gbinder returns the transaction's BORROWED descriptor. Match the
         * original Android guest dup(), not a transfer of that borrowed FD. */
        const int borrowed = gbinder_reader_read_fd(reader);
        if (borrowed < 0) return -EBADMSG;
        decoded.owned_fd = dup(borrowed);
        if (decoded.owned_fd < 0) decoded.dup_error = -errno;
        if (decoded.owned_fd > 0) decoded.fence_fd = decoded.owned_fd;
    }
    *out = decoded;
    return 0;
}

void anco_delegator_release_clear(AncoDelegatorRelease *release)
{
    if (release) {
        /* Stock's dup<=0 mapping loses the semantic FD0. Keep the actual
         * ownership separately so Linux can clean it without changing the
         * original fence_fd=-1 interpretation or touching the borrowed FD. */
        if (release->owned_fd >= 0) close(release->owned_fd);
        *release = (AncoDelegatorRelease)ANCO_DELEGATOR_RELEASE_INIT;
    }
}
