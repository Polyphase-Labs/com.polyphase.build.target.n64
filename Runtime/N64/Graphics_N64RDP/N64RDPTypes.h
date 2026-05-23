/**
 * @file N64RDPTypes.h
 * @brief Addon-private resource structs for the N64 graphics backend.
 *
 * Lives in the engine's per-asset `mResource` slot (a void* the engine
 * doesn't care about, populated by GFX_Create*Resource and torn down by
 * GFX_Destroy*Resource). The engine never looks inside these structs;
 * only Graphics_N64RDP.cpp does.
 */

#pragma once

#include <cstdint>
#include <libdragon.h>

#include "Vertex.h"        // engine's Vertex / VertexColor
#include "EngineTypes.h"   // IndexType

namespace n64rdp
{
    /**
     * Per-Texture asset state. We keep a RAM-side RGBA16 copy of the
     * pixels (with cache flushed so the RDP can DMA them) plus a libdragon
     * surface_t pointing at that copy so rdpq_tex_upload can ingest it
     * with no further conversion. The engine's original RGBA8888 pixel
     * vector is discarded after Create — we don't need it again.
     */
    struct TextureResource
    {
        uint16_t  mWidth      = 0;
        uint16_t  mHeight     = 0;
        uint16_t* mPixels     = nullptr;   // RGBA16 (5-5-5-1), 16-byte aligned
        surface_t mSurface    = {};        // wraps mPixels for rdpq_tex_upload
    };

    /**
     * Per-StaticMesh asset state. We hold a weak reference to the engine's
     * Vertex / VertexColor / Index buffers — the engine guarantees they
     * outlive the GPU resource via Asset refcounting. Per-frame draw
     * iterates these and CPU-transforms each vertex to screen space.
     *
     * Future: when we move to Tiny3D / RSP transform, this becomes a
     * device-side vertex buffer with pre-converted format. For now, raw
     * engine pointers are enough.
     */
    struct MeshResource
    {
        const void*       mVertices    = nullptr;  // Vertex* or VertexColor*
        const IndexType*  mIndices     = nullptr;
        uint32_t          mNumVertices = 0;
        uint32_t          mNumIndices  = 0;
        bool              mHasColor    = false;
    };
}
