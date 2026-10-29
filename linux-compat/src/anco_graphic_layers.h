/* Linux semantic values for the ORIGINAL donghu metadata-only layer stream.
 * Window ID here is the upstream setSurfaceWindowId value, not AMS Task ID.
 * This stream neither carries pixels nor acknowledges buffer consumption. */
#ifndef ANCO_GRAPHIC_LAYERS_H
#define ANCO_GRAPHIC_LAYERS_H
#include <stdint.h>
#include <stddef.h>

typedef struct {
    uint64_t surface_view;
    uint32_t window, z;
    /* Original FE isOpaque -> HWC transform -32/-31; NOT readiness. */
    uint8_t opaque;
    /* Original isSnapshotWindowLayer -> composition100; NOT visibility. */
    int8_t snapshot_byte;
    float scale, smart_x, smart_y;
    int32_t transform, dataspace;
    uint32_t composition, rgba, display_transform;
    int32_t source_crop[4], display_frame[4]; /* original LTRB, not XYWH */
    /* Borrowed genuine gralloc native_handle, and its actual allocated size.
     * Kept alive through encoding. No handle means original flag2 and extras0.
     * Handle FD fstat failures skip the layer as original Serialize does. */
    const void *handle;
    size_t handle_bytes;
} AncoGraphicLayer;

enum { ANCO_GRAPHIC_LAYER_BYTES = 108, ANCO_GRAPHIC_LAYER_PACKET_LIMIT = 2048 };

/* Encode the COMPLETE scene, in the original order, with original count
 * backpatching. NULL entries and invalid-FD layers are skipped as original.
 * Caller supplies actual evaluated values; this does not invent identities,
 * geometry, opacity, snapshot state, rotation, or shared-buffer mode. Linux float
 * crop conversion must precede this call; these crop ints are original FCVTZS
 * results. Input pointers must remain valid for the duration of the call.
 * A zero accepted count is retained as original, but is not sendable to the
 * current receiver (which accepts1..100). No partial-scene splitting.
 * On error both output counters are0; partial output must not be used. */
int anco_graphic_layers_encode(const AncoGraphicLayer *const *layers,
    size_t count, void *packet, size_t capacity, size_t *bytes, uint32_t *accepted);

typedef struct AncoGraphicLayerStream AncoGraphicLayerStream;

/* One original fd connection, owned by the common runtime. timeouts bound
 * transport calls only, never application/UI lifetime. Errors close the fd;
 * no guessed acknowledgement, replay, retry scene, or fence is generated. */
int anco_graphic_layers_open(const char *actual_socket, int timeout_ms,
    AncoGraphicLayerStream **out);
int anco_graphic_layers_send(AncoGraphicLayerStream *stream,
    const AncoGraphicLayer *const *complete_scene, size_t count,
    int timeout_ms, uint32_t *accepted);
void anco_graphic_layers_close(AncoGraphicLayerStream *stream);
#endif
