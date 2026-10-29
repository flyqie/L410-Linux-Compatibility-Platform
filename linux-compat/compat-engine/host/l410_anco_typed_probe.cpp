#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <unistd.h>
#include <poll.h>
#include <sys/mman.h>

struct L410AncoSurface;
extern "C" L410AncoSurface *l410_anco_surface_create_typed(
    const char *name, int32_t width, int32_t height);
extern "C" int32_t l410_anco_surface_publish_typed(
    L410AncoSurface *handle, int32_t windowId);
extern "C" int32_t l410_anco_surface_destroy_typed(L410AncoSurface *handle);
extern "C" int32_t l410_anco_surface_last_create_stage(void);
extern "C" void *l410_anco_surface_native_window_typed(L410AncoSurface *handle);
extern "C" int32_t l410_anco_surface_id_typed(L410AncoSurface *handle, uint64_t *surfaceId);
struct L410NativeBufferInfo {
    int32_t fd, width, stride, height, size, format;
    uint64_t usage;
};
extern "C" int32_t l410_anco_surface_configure_typed(
    L410AncoSurface *handle, int32_t width, int32_t height);
extern "C" int32_t l410_anco_surface_request_buffer_typed(
    L410AncoSurface *handle, void **bufferOut, int32_t *fenceFd,
    L410NativeBufferInfo *info);
extern "C" int32_t l410_anco_surface_flush_buffer_typed(
    L410AncoSurface *handle, void *buffer, int32_t fenceFd);
extern "C" int32_t l410_anco_surface_abort_buffer_typed(
    L410AncoSurface *handle, void *buffer);
extern "C" int32_t l410_anco_surface_node_create_typed(
    uint64_t displayId, int32_t windowId, const char *name);
extern "C" int32_t l410_anco_surface_node_create_delegated_typed(
    uint64_t displayId, int32_t windowId, const char *name, void *remoteObject);
extern "C" void *l410_anco_surface_delegator_remote_typed(L410AncoSurface *surface);
extern "C" int32_t l410_anco_surface_node_remove_typed(
    uint64_t displayId, int32_t windowId);

int main(int argc, char **argv)
{
    const int32_t windowId = argc > 1 ? static_cast<int32_t>(std::strtol(argv[1], nullptr, 10)) : 41001;
    if (argc > 2 && std::strcmp(argv[2], "node-only") == 0) {
        const int32_t created = l410_anco_surface_node_create_typed(
            0, windowId, "l410-direct-xcomponent-probe");
        std::printf("node_create=%d window=%d\n", created, windowId);
        std::fflush(stdout);
        sleep(4);
        const int32_t removed = l410_anco_surface_node_remove_typed(0, windowId);
        std::printf("node_remove=%d window=%d\n", removed, windowId);
        return created == 0 ? 0 : 4;
    }
    const bool delegatedFrameMode = argc > 2 &&
        (std::strcmp(argv[2], "delegated-frame") == 0 ||
         std::strcmp(argv[2], "delegated-publish-frame") == 0);
    const bool delegatedPublishMode = argc > 2 &&
        std::strcmp(argv[2], "delegated-publish-frame") == 0;
    const bool frameMode = delegatedFrameMode ||
        (argc > 2 && std::strcmp(argv[2], "frame") == 0);
    L410AncoSurface *surface = l410_anco_surface_create_typed("l410-typed-probe", 800, 600);
    if (surface == nullptr) {
        std::printf("create=-1 stage=%d\n", l410_anco_surface_last_create_stage());
        return 2;
    }
    uint64_t surfaceId = 0;
    const int32_t surfaceIdResult = l410_anco_surface_id_typed(surface, &surfaceId);
    std::printf("create=0 native_window=%d surface_id_rc=%d surface_id=%llu\n",
        l410_anco_surface_native_window_typed(surface) != nullptr ? 1 : 0,
        surfaceIdResult, static_cast<unsigned long long>(surfaceId));
    int32_t node = 0;
    if (delegatedFrameMode) {
        node = l410_anco_surface_node_create_delegated_typed(0, windowId,
            "l410-delegated-frame-probe",
            l410_anco_surface_delegator_remote_typed(surface));
        std::printf("delegated_node=%d\n", node);
    }
    const int32_t publish = delegatedFrameMode ?
        (node == 0 && delegatedPublishMode ?
            l410_anco_surface_publish_typed(surface, windowId) : node) :
        l410_anco_surface_publish_typed(surface, windowId);
    std::printf("publish=%d delegated=%d\n", publish,
        delegatedFrameMode ? (delegatedPublishMode ? 2 : 1) : 0);
    if (frameMode && publish == 0) {
        const int32_t configured = l410_anco_surface_configure_typed(surface, 800, 600);
        std::printf("configure=%d\n", configured);
        void *buffer = nullptr;
        int32_t fence = -1;
        L410NativeBufferInfo info {};
        const int32_t requested = configured == 0
            ? l410_anco_surface_request_buffer_typed(surface, &buffer, &fence, &info)
            : configured;
        std::printf("request=%d fd=%d fence=%d size=%d extent=%dx%d stride=%d format=%d usage=%llu\n",
            requested, info.fd, fence, info.size, info.width, info.height,
            info.stride, info.format, static_cast<unsigned long long>(info.usage));
        if (requested == 0) {
            if (fence >= 0) {
                pollfd waitFence {fence, POLLIN, 0};
                const int waited = poll(&waitFence, 1, 1000);
                std::printf("fence_wait=%d revents=%d\n", waited, waitFence.revents);
                close(fence);
                fence = -1;
            }
            void *mapped = mmap(nullptr, static_cast<size_t>(info.size),
                PROT_READ | PROT_WRITE, MAP_SHARED, info.fd, 0);
            if (mapped != MAP_FAILED) {
                auto *pixels = static_cast<uint32_t *>(mapped);
                const size_t pitch = static_cast<size_t>(info.stride) / sizeof(uint32_t);
                for (int32_t y = 0; y < info.height; ++y) {
                    for (int32_t x = 0; x < info.width; ++x) {
                        const uint8_t red = static_cast<uint8_t>(32 + x * 192 / (info.width ? info.width : 1));
                        const uint8_t green = static_cast<uint8_t>(32 + y * 192 / (info.height ? info.height : 1));
                        pixels[static_cast<size_t>(y) * pitch + x] =
                            0xff000000u | (static_cast<uint32_t>(red) << 16) |
                            (static_cast<uint32_t>(green) << 8) | 0x40u;
                    }
                }
                msync(mapped, static_cast<size_t>(info.size), MS_SYNC);
                munmap(mapped, static_cast<size_t>(info.size));
                const int32_t flushed = l410_anco_surface_flush_buffer_typed(surface, buffer, -1);
                std::printf("flush=%d\n", flushed);
            } else {
                std::printf("mmap=-1\n");
                l410_anco_surface_abort_buffer_typed(surface, buffer);
            }
        }
        std::fflush(stdout);
        sleep(delegatedFrameMode ? 2 : 8);
    }
    if (delegatedFrameMode && node == 0) {
        const int32_t removed = l410_anco_surface_node_remove_typed(0, windowId);
        std::printf("node_remove=%d\n", removed);
    }
    const int32_t destroy = l410_anco_surface_destroy_typed(surface);
    std::printf("destroy=%d\n", destroy);
    return publish == 0 ? 0 : 3;
}
