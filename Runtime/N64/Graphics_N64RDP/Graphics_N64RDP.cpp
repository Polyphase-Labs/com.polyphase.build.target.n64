/**
 * @file Graphics_N64RDP.cpp
 * @brief N64 graphics backend — Phase 1 stub.
 *
 * Phase 1 goal: the ROM boots, the engine initialises, the framebuffer
 * clears to black each frame, and display_show swaps so we can see the
 * sync is alive in the emulator. Every other GFX_* is a no-op or a
 * trivial resource alloc that satisfies the engine's invariants without
 * touching the RDP / RSP.
 *
 * Phase 3 will replace BeginFrame/EndFrame with Tiny3D's t3d_frame_start
 * / t3d_screen_clear + RDP draw command stream, and fill in the resource
 * create/draw functions for textured + lit static meshes. Widgets (Quad,
 * Text, etc.) land in a sibling Graphics_N64RDP_2D.cpp once Phase 3
 * verifies 3D works end-to-end.
 *
 * Keep this file libdragon-only — no Tiny3D / external GL shim references
 * until Phase 3. That way Phase 1 builds against vanilla libdragon
 * without any extra dependencies.
 */

#if defined(POLYPHASE_PLATFORM_ADDON)

#include "Graphics/Graphics.h"
#include "Graphics/GraphicsTypes.h"
#include "Graphics/GraphicsConstants.h"
#include "Renderer.h"
#include "World.h"
#include "Engine.h"
#include "Log.h"

// Engine resource types referenced by GFX_* signatures.
#include "Engine/Assets/Texture.h"
#include "Engine/Assets/Material.h"
#include "Engine/Assets/MaterialLite.h"  // GetTexture(slot)

// libdragon's <malloc.h> doesn't expose memalign even though newlib
// provides the symbol. Forward-declare. (System_N64.cpp does the same.)
extern "C" void* memalign(size_t alignment, size_t size);
#include "Engine/Assets/StaticMesh.h"
#include "Engine/Assets/SkeletalMesh.h"
#include "Engine/Nodes/3D/StaticMesh3D.h"
#include "Engine/Nodes/3D/SkeletalMesh3D.h"
#include "Engine/Nodes/3D/ShadowMesh3D.h"
#include "Engine/Nodes/3D/InstancedMesh3D.h"
#include "Engine/Nodes/3D/TextMesh3D.h"
#include "Engine/Nodes/3D/Particle3D.h"
#include "Engine/Nodes/Widgets/Quad.h"
#include "Engine/Nodes/Widgets/Text.h"
#include "Engine/Nodes/Widgets/Poly.h"

#include <libdragon.h>

#include <cstdint>
#include <cstring>

namespace
{
    // libdragon's display API returns a surface_t* for the current
    // framebuffer. We hold the active surface for the duration of a frame
    // (between BeginFrame and EndFrame) and release it on swap.
    surface_t* sActiveSurface = nullptr;

    // Clear color — Phase 1 picks bright magenta so you can visually
    // confirm GFX_BeginFrame/EndFrame are running each frame. A black
    // screen would be ambiguous (could be a hang BEFORE rendering, the
    // bootloader's idle screen, or actually-working). Magenta is
    // unmistakable. Phase 3 swaps this for a Renderer-supplied clear.
    constexpr color_t kClearColor = { 0xFF, 0x00, 0xFF, 0xFF };

    // Frame counter so we can sanity-check "rendering" by watching the
    // color cycle. The clear color itself stays magenta; the counter is
    // used to gate a per-frame ISViewer log line so the host knows the
    // engine's main loop is ticking.
    static uint32_t sFrameCount = 0;

    // Z-buffer for 3D rendering. Allocated lazily on first GFX_BeginFrame
    // to keep the boot path lean. 16-bit Z is the N64-native depth format
    // (FMT_RGBA16 used as a depth surface — libdragon convention).
    static surface_t sZBuffer = {};
    static bool      sZBufferReady = false;

    // First-light test rendering. Set to true to draw a small textured
    // triangle every frame as a sanity check that the rdpq pipeline works
    // before any engine GFX_DrawStaticMeshComp gets wired up. Disable once
    // engine mesh rendering is fully integrated. Visible as a small
    // gradient triangle in the top-left.
    constexpr bool kDrawTestTriangle = true;

    // 32x32 procedural checkerboard texture used by the test triangle.
    // RGBA16 (5-5-5-1) format = 2 bytes per texel = 2 KB total (well
    // under TMEM's 4 KB ceiling).
    alignas(16) static uint16_t sTestTexData[32 * 32];
    static surface_t sTestSurface = {};
    static bool      sTestTexReady = false;

    void EnsureTestTexture()
    {
        if (sTestTexReady) return;
        // Build a magenta-on-yellow 4x4 checker for visibility.
        for (int y = 0; y < 32; ++y)
        {
            for (int x = 0; x < 32; ++x)
            {
                const bool dark = ((x / 4) ^ (y / 4)) & 1;
                // RGBA16: rrrrrgggggbbbbba
                sTestTexData[y * 32 + x] = dark
                    ? 0xF03Fu  // magenta-ish
                    : 0xFFC1u; // yellow-ish
            }
        }
        // Flush the CPU data cache so the RDP can read the texture via DMA.
        // Without this, the RDP DMA bypasses CPU cache and reads stale
        // (often zero) bytes — visible as horizontal black bars across
        // the texture in TMEM. Pattern repeats per-row because TMEM loads
        // are 8-byte-aligned and cache lines are 16 bytes.
        data_cache_hit_writeback_invalidate(sTestTexData, sizeof(sTestTexData));
        sTestSurface = surface_make_linear(sTestTexData, FMT_RGBA16, 32, 32);
        sTestTexReady = true;
        debugf("[GFX] test texture built (32x32 RGBA16, cache flushed)\n");
    }
}

// =========================================================================
// Lifecycle
// =========================================================================

void GFX_Initialize()
{
    debugf("[GFX] Entered GFX_Initialize (before rdpq_init)\n");
    // display_init / rdpq_init are called from Main_N64.cpp's boot. Engine-
    // visible setup happens here. For Phase 1 there's nothing to do — the
    // first BeginFrame will grab a surface and clear it.
    rdpq_init();
    debugf("[GFX] rdpq_init returned\n");
    LogDebug("Graphics_N64RDP: rdpq initialised (Phase 1 stub)");
}

void GFX_Shutdown()
{
    rdpq_close();
}

// Forward declaration so GFX_BeginFrame can invalidate the per-frame
// camera-matrix cache (defined further down with the static mesh helpers).
namespace { extern bool sViewProjValid; }

void GFX_BeginFrame()
{
    // Per-frame: invalidate camera-matrix cache. The first
    // GFX_DrawStaticMeshComp call this frame will re-read it.
    sViewProjValid = false;

    // Lazy z-buffer allocation — first frame only. Allocating in
    // GFX_Initialize would risk the heap not being warm yet on some
    // libdragon configs; this is safer.
    if (!sZBufferReady)
    {
        sZBuffer = surface_alloc(FMT_RGBA16, 320, 240);
        sZBufferReady = (sZBuffer.buffer != nullptr);
        debugf("[GFX] z-buffer allocated: %dx%d (ready=%d)\n",
               sZBuffer.width, sZBuffer.height, (int)sZBufferReady);
    }

    // Pull a framebuffer to render into. display_get blocks until VBlank
    // when the queue is full, giving us natural 60 Hz / 50 Hz sync.
    sActiveSurface = display_get();
    if (sActiveSurface == nullptr) return;

    // Attach BOTH color and Z surfaces. Z is required for 3D meshes to
    // depth-sort correctly. The Z-buffer gets cleared via rdpq_clear_z.
    rdpq_attach(sActiveSurface, sZBufferReady ? &sZBuffer : nullptr);
    rdpq_clear(kClearColor);
    if (sZBufferReady)
    {
        rdpq_clear_z(0xFFFC);
    }

    // Log every ~60th frame so ISViewer/USB shows the engine is alive
    // without spamming the channel at 60 Hz.
    if ((sFrameCount++ % 60) == 0)
    {
        debugf("[GFX] frame %u\n", (unsigned)sFrameCount);
    }
}

// Helper: draw a spinning, perspective-correct, depth-tested,
// texture-mapped CUBE using exactly the same rdpq_triangle code path
// engine StaticMesh3D nodes will use. Proves the whole 3D pipeline works
// before any engine asset loading lands.
//
// Cube is centered at the world origin, 1 unit per side. Camera sits at
// (0, 0, 4) looking down -Z. Rotation is time-based using the global
// frame counter so we get a steady spin per second.
static void DrawTestCube()
{
    EnsureTestTexture();

    // -- Texture state --------------------------------------------------
    rdpq_set_mode_standard();
    rdpq_mode_zbuf(true, true);
    rdpq_mode_persp(true);
    rdpq_mode_filter(FILTER_BILINEAR);
    rdpq_tex_upload(TILE0, &sTestSurface, NULL);

    // -- Matrices -------------------------------------------------------
    // Time-based Y/X rotation. sFrameCount ticks once per frame; at 60 Hz
    // dividing by 30 gives roughly 2 rad/sec.
    const float t = (float)sFrameCount / 30.0f;
    const glm::mat4 model =
        glm::rotate(glm::mat4(1.0f), t,        glm::vec3(0.0f, 1.0f, 0.0f)) *
        glm::rotate(glm::mat4(1.0f), t * 0.7f, glm::vec3(1.0f, 0.0f, 0.0f));
    const glm::mat4 view  = glm::lookAt(
        glm::vec3(0.0f, 0.0f, 4.0f),
        glm::vec3(0.0f, 0.0f, 0.0f),
        glm::vec3(0.0f, 1.0f, 0.0f));
    const glm::mat4 proj  = glm::perspective(
        glm::radians(60.0f),
        320.0f / 240.0f,
        0.1f, 100.0f);
    const glm::mat4 mvp   = proj * view * model;

    // -- Cube geometry (8 verts, 12 triangles, UVs per-vertex) ---------
    struct CV { float px, py, pz, u, v; };
    static const CV cube_verts[8] = {
        {-1.0f, -1.0f, -1.0f, 0.0f, 0.0f},  // 0: -x -y -z
        { 1.0f, -1.0f, -1.0f, 1.0f, 0.0f},  // 1: +x -y -z
        { 1.0f,  1.0f, -1.0f, 1.0f, 1.0f},  // 2: +x +y -z
        {-1.0f,  1.0f, -1.0f, 0.0f, 1.0f},  // 3: -x +y -z
        {-1.0f, -1.0f,  1.0f, 0.0f, 0.0f},  // 4: -x -y +z
        { 1.0f, -1.0f,  1.0f, 1.0f, 0.0f},  // 5: +x -y +z
        { 1.0f,  1.0f,  1.0f, 1.0f, 1.0f},  // 6: +x +y +z
        {-1.0f,  1.0f,  1.0f, 0.0f, 1.0f},  // 7: -x +y +z
    };
    static const uint8_t cube_idx[36] = {
        0,1,2, 0,2,3,    // -Z face
        5,4,7, 5,7,6,    // +Z face
        4,0,3, 4,3,7,    // -X face
        1,5,6, 1,6,2,    // +X face
        3,2,6, 3,6,7,    // +Y face
        4,5,1, 4,1,0,    // -Y face
    };

    // -- Transform all 8 vertices once ---------------------------------
    constexpr float kScreenW = 320.0f;
    constexpr float kScreenH = 240.0f;
    constexpr float kTexW    = 32.0f;
    constexpr float kTexH    = 32.0f;

    struct Xfm { float sx, sy, sz, s, t, inv_w; };
    Xfm xfm[8];
    for (int i = 0; i < 8; ++i)
    {
        const CV& cv = cube_verts[i];
        const glm::vec4 clip = mvp * glm::vec4(cv.px, cv.py, cv.pz, 1.0f);
        const float w  = clip.w;
        const float iw = (w > 0.0001f) ? (1.0f / w) : 0.0f;

        const float nx = clip.x * iw;
        const float ny = clip.y * iw;
        const float nz = clip.z * iw;

        xfm[i].sx    = (nx * 0.5f + 0.5f) * kScreenW;
        xfm[i].sy    = (1.0f - (ny * 0.5f + 0.5f)) * kScreenH;
        xfm[i].sz    = nz * 0.5f + 0.5f;
        xfm[i].s     = cv.u * kTexW;
        xfm[i].t     = cv.v * kTexH;
        xfm[i].inv_w = iw;
    }

    // -- Issue 12 triangles --------------------------------------------
    for (int t_idx = 0; t_idx < 12; ++t_idx)
    {
        const Xfm& A = xfm[cube_idx[t_idx * 3 + 0]];
        const Xfm& B = xfm[cube_idx[t_idx * 3 + 1]];
        const Xfm& C = xfm[cube_idx[t_idx * 3 + 2]];

        // Skip if every vertex is behind the near plane.
        if (A.inv_w == 0.0f && B.inv_w == 0.0f && C.inv_w == 0.0f) continue;

        const float va[6] = { A.sx, A.sy, A.sz, A.s, A.t, A.inv_w };
        const float vb[6] = { B.sx, B.sy, B.sz, B.s, B.t, B.inv_w };
        const float vc[6] = { C.sx, C.sy, C.sz, C.s, C.t, C.inv_w };
        rdpq_triangle(&TRIFMT_ZBUF_TEX, va, vb, vc);
    }
}

void GFX_EndFrame()
{
    if (sActiveSurface == nullptr) return;

    if (kDrawTestTriangle)
    {
        DrawTestCube();
    }

    rdpq_detach_show();
    sActiveSurface = nullptr;
}

void GFX_BeginScreen(uint32_t /*screenIndex*/) {}
void GFX_BeginView(uint32_t /*viewIndex*/) {}

bool GFX_ShouldCullLights() { return false; }

void GFX_BeginRenderPass(RenderPassId /*renderPassId*/) {}
void GFX_EndRenderPass() {}
void GFX_SetPipelineState(PipelineConfig /*config*/) {}

void GFX_SetViewport(int32_t /*x*/, int32_t /*y*/, int32_t /*width*/, int32_t /*height*/, bool /*handlePrerotation*/)
{
    // Hardcoded to the 320x240 framebuffer. The engine may pass scene-tab
    // viewport dimensions inherited from the editor that don't match N64's
    // physical screen; ignore those and trust display_init's setup.
}

void GFX_SetScissor(int32_t /*x*/, int32_t /*y*/, int32_t /*width*/, int32_t /*height*/, bool /*handlePrerotation*/)
{
}

void GFX_SetFog(const FogSettings& /*fogSettings*/) {}
void GFX_DrawLines(const std::vector<Line>& /*lines*/) {}
void GFX_DrawFullscreen() {}

void GFX_ResizeWindow() {}
void GFX_Reset() {}

Node3D* GFX_ProcessHitCheck(World* /*world*/, int32_t /*x*/, int32_t /*y*/, uint32_t* outInstance)
{
    if (outInstance) *outInstance = 0;
    return nullptr;
}

uint32_t GFX_GetNumViews() { return 1; }

void GFX_SetFrameRate(int32_t /*frameRate*/) {}

void GFX_PathTrace() {}

void GFX_BeginLightBake() {}
void GFX_UpdateLightBake() {}
void GFX_EndLightBake() {}
bool GFX_IsLightBakeInProgress() { return false; }
float GFX_GetLightBakeProgress() { return 0.0f; }

void GFX_EnableMaterials(bool /*enable*/) {}

void GFX_BeginGpuTimestamp(const char* /*name*/) {}
void GFX_EndGpuTimestamp(const char* /*name*/) {}

// =========================================================================
// Textures
// =========================================================================
//
// The engine's TextureResource (Graphics/GraphicsTypes.h, addon-platform
// branch) carries generic POD fields: mPixels (void*), mWidth, mHeight,
// mBufWidth, mPsm, mMipCount, mSwizzled. We use mPixels for our RGBA16
// buffer, mWidth/mHeight for dimensions, and ignore the others. The
// engine never inspects these — only Graphics_N64RDP.cpp does.
//
// Format: N64 RGBA16 (5-5-5-1). Convert engine's RGBA8888 once at create
// time, cache-flush, hand to rdpq_tex_upload via a surface_t we
// rebuild per-draw (surface_t is cheap; 16 bytes of struct on the stack).

namespace
{
    inline uint16_t Rgba8888To16(uint8_t r, uint8_t g, uint8_t b, uint8_t a)
    {
        // N64 RGBA16: rrrrr ggggg bbbbb a (big-endian word).
        const uint16_t r5 = (uint16_t)((r >> 3) & 0x1F);
        const uint16_t g5 = (uint16_t)((g >> 3) & 0x1F);
        const uint16_t b5 = (uint16_t)((b >> 3) & 0x1F);
        const uint16_t a1 = (a >= 128) ? 1u : 0u;
        return (uint16_t)((r5 << 11) | (g5 << 6) | (b5 << 1) | a1);
    }
}

void GFX_CreateTextureResource(Texture* texture, std::vector<uint8_t>& data)
{
    if (texture == nullptr) return;

    const uint32_t w = texture->GetWidth();
    const uint32_t h = texture->GetHeight();
    if (w == 0 || h == 0 || data.empty())
    {
        debugf("[GFX] CreateTextureResource: skipping zero-size texture\n");
        return;
    }

    // Convert RGBA8888 → RGBA16. memalign(16,...) keeps the buffer aligned
    // for RDP DMA. Engine input is tightly packed RGBA8888 with no row
    // padding.
    const size_t numTexels = (size_t)w * (size_t)h;
    uint16_t* rgba16 = (uint16_t*)memalign(16, numTexels * sizeof(uint16_t));
    if (rgba16 == nullptr)
    {
        debugf("[GFX] CreateTextureResource: memalign(%zu) failed\n",
               numTexels * sizeof(uint16_t));
        return;
    }
    const uint8_t* src = data.data();
    for (size_t i = 0; i < numTexels; ++i)
    {
        rgba16[i] = Rgba8888To16(src[i*4 + 0], src[i*4 + 1],
                                  src[i*4 + 2], src[i*4 + 3]);
    }
    // Cache writeback so RDP DMA reads the actual converted bytes, not
    // stale CPU cache lines.
    data_cache_hit_writeback_invalidate(rgba16, numTexels * sizeof(uint16_t));

    TextureResource* res = texture->GetResource();
    res->mPixels = rgba16;
    res->mWidth  = w;
    res->mHeight = h;
    res->mBufWidth = w;
    res->mPsm    = 0;
    res->mMipCount = 0;
    res->mSwizzled = 0;

    debugf("[GFX] Texture created: %lux%lu (%lu bytes)\n",
           (unsigned long)w, (unsigned long)h,
           (unsigned long)(numTexels * sizeof(uint16_t)));
}

void GFX_DestroyTextureResource(Texture* texture)
{
    if (texture == nullptr) return;
    TextureResource* res = texture->GetResource();
    if (res->mPixels != nullptr)
    {
        free(res->mPixels);
        res->mPixels = nullptr;
    }
    res->mWidth = 0;
    res->mHeight = 0;
    res->mBufWidth = 0;
}

void GFX_UpdateTextureResourcePixels(Texture* /*texture*/, const uint8_t* /*src*/,
                                     uint32_t /*offsetX*/, uint32_t /*offsetY*/,
                                     uint32_t /*width*/, uint32_t /*height*/)
{
    // Streaming texture updates (video player) — Phase 3+. For now,
    // re-Create the resource if the engine needs to update pixels.
}

// =========================================================================
// Materials
// =========================================================================

void GFX_CreateMaterialResource(Material* /*material*/) {}
void GFX_DestroyMaterialResource(Material* /*material*/) {}

// =========================================================================
// Static meshes
// =========================================================================
//
// Mesh data flow on N64 (software transform path):
//
//   CreateStaticMeshResource — stash weak refs to engine's vertex/index
//     buffers in the engine-side StaticMeshResource POD slot. No copy,
//     no upload (vertex data is read CPU-side per-frame for transform).
//
//   DrawStaticMeshComp — build MVP from camera + node transform, iterate
//     the index buffer 3-at-a-time, transform each triangle's 3 vertices
//     into screen space + perspective-correct texture coords, hand to
//     rdpq_triangle with TRIFMT_ZBUF_TEX layout.
//
// Vertex layout we pass to rdpq_triangle (TRIFMT_ZBUF_TEX):
//
//   float[0] = screen_x   (post-perspective screen X in pixels)
//   float[1] = screen_y   (post-perspective screen Y in pixels)
//   float[2] = z_buf      (depth in 0..1, RDP scales to 16-bit z)
//   float[3] = s          (texel coord s, in texels)
//   float[4] = t          (texel coord t, in texels)
//   float[5] = inv_w      (1 / clip-space W — perspective divider)
//
// libdragon's RSP triangle-setup uses inv_w to interpolate s, t per-pixel
// with perspective correction. Passing inv_w = 1 disables perspective
// (affine texture mapping — fine for UI quads, wrong for 3D meshes).

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

namespace
{
    // Helper accessor: engine StaticMesh exposes vertex data through the
    // base Vertex pointer when !HasVertexColor, or VertexColor* when it
    // does. For the draw, we only need position + uv0 — both share the
    // same offset in either struct (Vertex / VertexColor both start with
    // glm::vec3 mPosition then glm::vec2 mTexcoord0). So one stride
    // accessor handles both.
    struct N64MeshAccess
    {
        const uint8_t* base;
        size_t         stride;
        const IndexType* indices;
        uint32_t       numIndices;
    };

    inline const glm::vec3& MeshPos(const N64MeshAccess& m, uint32_t idx)
    {
        return *reinterpret_cast<const glm::vec3*>(m.base + idx * m.stride);
    }
    inline const glm::vec2& MeshUV(const N64MeshAccess& m, uint32_t idx)
    {
        // mTexcoord0 lives at offset sizeof(glm::vec3) inside each Vertex.
        return *reinterpret_cast<const glm::vec2*>(m.base + idx * m.stride + sizeof(glm::vec3));
    }

    // Active per-draw camera matrices — cached at the start of each
    // GFX_DrawStaticMeshComp to avoid re-computing per vertex.
    glm::mat4 sViewProj{1.0f};
    bool      sViewProjValid = false;
    Camera3D* sCachedCamera  = nullptr;

    void UpdateCameraCache(World* world)
    {
        if (world == nullptr)
        {
            sViewProjValid = false;
            return;
        }
        Camera3D* cam = world->GetActiveCamera();
        if (cam == nullptr)
        {
            sViewProjValid = false;
            return;
        }
        if (cam != sCachedCamera)
        {
            sCachedCamera = cam;
        }
        // Camera updates view/proj internally each Tick; just multiply.
        sViewProj = cam->GetProjectionMatrix() * cam->GetViewMatrix();
        sViewProjValid = true;
    }
}

void GFX_CreateStaticMeshResource(StaticMesh* staticMesh, bool hasColor,
                                  uint32_t numVertices, void* vertices,
                                  uint32_t numIndices, IndexType* indices)
{
    if (staticMesh == nullptr) return;
    StaticMeshResource* res = staticMesh->GetResource();
    res->mVertexData   = vertices;          // weak ref — engine owns lifetime
    res->mIndexData    = indices;
    res->mNumVertices  = numVertices;
    res->mNumIndices   = numIndices;
    res->mVertexStride = (uint32_t)(hasColor ? sizeof(VertexColor) : sizeof(Vertex));
    res->mVertexFlags  = hasColor ? 1u : 0u;  // bit 0 = has-color
    debugf("[GFX] StaticMesh created: %lu verts, %lu indices, hasColor=%d\n",
           (unsigned long)numVertices, (unsigned long)numIndices, (int)hasColor);
}

void GFX_DestroyStaticMeshResource(StaticMesh* staticMesh)
{
    if (staticMesh == nullptr) return;
    StaticMeshResource* res = staticMesh->GetResource();
    // Weak refs — nothing to free, just clear.
    res->mVertexData = nullptr;
    res->mIndexData = nullptr;
    res->mNumVertices = 0;
    res->mNumIndices = 0;
    res->mVertexStride = 0;
    res->mVertexFlags = 0;
}

void GFX_CreateSkeletalMeshResource(SkeletalMesh* /*skeletalMesh*/, uint32_t /*numVertices*/, VertexSkinned* /*vertices*/, uint32_t /*numIndices*/, IndexType* /*indices*/) {}
void GFX_DestroySkeletalMeshResource(SkeletalMesh* /*skeletalMesh*/) {}

void GFX_CreateStaticMeshCompResource(StaticMesh3D* /*staticMeshComp*/) {}
void GFX_DestroyStaticMeshCompResource(StaticMesh3D* /*staticMeshComp*/) {}
void GFX_UpdateStaticMeshCompResourceColors(StaticMesh3D* /*staticMeshComp*/) {}

void GFX_DrawStaticMeshComp(StaticMesh3D* staticMeshComp, StaticMesh* meshOverride)
{
    if (staticMeshComp == nullptr || sActiveSurface == nullptr) return;

    StaticMesh* mesh = meshOverride ? meshOverride : staticMeshComp->GetStaticMesh();
    if (mesh == nullptr) return;

    StaticMeshResource* meshRes = mesh->GetResource();
    if (meshRes->mVertexData == nullptr || meshRes->mIndexData == nullptr) return;
    if (meshRes->mNumIndices < 3) return;

    // Camera matrices. Cache per-frame; the same camera will be the active
    // camera for every draw in a given Render pass, so this is effectively
    // memoized.
    if (!sViewProjValid)
    {
        UpdateCameraCache(staticMeshComp->GetWorld());
        if (!sViewProjValid) return;
    }

    const glm::mat4 model = staticMeshComp->GetTransform();
    const glm::mat4 mvp   = sViewProj * model;

    // Optional texture. Material may be the base type — convert to
    // MaterialLite via Material::AsLite (same pattern PSP uses) to call
    // GetTexture(slot). MaterialInstance / other types fall back to null.
    Material* matBase = staticMeshComp->GetMaterial();
    MaterialLite* mat = Material::AsLite(matBase);
    Texture*  tex     = (mat != nullptr) ? mat->GetTexture(0) : nullptr;
    TextureResource* texRes = (tex != nullptr) ? tex->GetResource() : nullptr;

    if (texRes != nullptr && texRes->mPixels != nullptr && texRes->mWidth > 0)
    {
        // Build a surface_t on the fly pointing at our cached RGBA16
        // pixels and upload to TMEM. surface_t is a small POD; stack
        // allocation is fine.
        surface_t surf = surface_make_linear(texRes->mPixels, FMT_RGBA16,
                                             texRes->mWidth, texRes->mHeight);
        rdpq_set_mode_standard();
        rdpq_mode_zbuf(true, true);             // z read + write
        rdpq_mode_persp(true);                  // perspective-correct texture
        rdpq_mode_filter(FILTER_BILINEAR);
        rdpq_tex_upload(TILE0, &surf, NULL);
    }
    else
    {
        // No texture — solid-color triangle (white).
        rdpq_set_mode_standard();
        rdpq_mode_zbuf(true, true);
        rdpq_set_prim_color(RGBA32(255, 255, 255, 255));
    }

    // Software vertex transform. Build the screen-space + UV + 1/w array
    // up front, then iterate the index buffer to issue triangles.
    N64MeshAccess access;
    access.base       = static_cast<const uint8_t*>(meshRes->mVertexData);
    access.stride     = meshRes->mVertexStride;
    access.indices    = static_cast<const IndexType*>(meshRes->mIndexData);
    access.numIndices = meshRes->mNumIndices;

    // Texture dims for texel-space UVs (engine UVs are 0..1 normalized).
    const float texW = (texRes != nullptr) ? (float)texRes->mWidth  : 1.0f;
    const float texH = (texRes != nullptr) ? (float)texRes->mHeight : 1.0f;

    // Screen-space dimensions (matches Main_N64's display_init).
    constexpr float kScreenW = 320.0f;
    constexpr float kScreenH = 240.0f;
    constexpr float kScreenHalfW = kScreenW * 0.5f;
    constexpr float kScreenHalfH = kScreenH * 0.5f;

    // Per-vertex screen-space buffer. Engine meshes are typically <1k verts;
    // a fixed-size stack buffer of 2048 floats (~256 vertices) avoids heap
    // pressure. For bigger meshes we'd page through indices.
    constexpr uint32_t kMaxVerts = 256;
    float xfm[kMaxVerts * 6];

    if (meshRes->mNumVertices > kMaxVerts)
    {
        debugf("[GFX] mesh %lu verts > %lu, draw skipped (need paged transform)\n",
               (unsigned long)meshRes->mNumVertices, (unsigned long)kMaxVerts);
        return;
    }

    // Transform all vertices once. Cull anything fully behind the near plane.
    for (uint32_t i = 0; i < meshRes->mNumVertices; ++i)
    {
        const glm::vec3& pos = MeshPos(access, i);
        const glm::vec2& uv  = MeshUV(access, i);

        glm::vec4 clip = mvp * glm::vec4(pos, 1.0f);
        const float w  = clip.w;
        const float inv_w = (w > 0.0001f) ? (1.0f / w) : 0.0f;

        const float ndc_x = clip.x * inv_w;
        const float ndc_y = clip.y * inv_w;
        const float ndc_z = clip.z * inv_w;

        const float sx = (ndc_x * 0.5f + 0.5f) * kScreenW;
        // Flip Y: NDC +Y is up, screen +Y is down.
        const float sy = (1.0f - (ndc_y * 0.5f + 0.5f)) * kScreenH;
        // Map NDC z [-1, 1] → buffer z [0, 1].
        const float sz = (ndc_z * 0.5f + 0.5f);

        float* v = xfm + i * 6;
        v[0] = sx;
        v[1] = sy;
        v[2] = sz;
        v[3] = uv.x * texW;
        v[4] = uv.y * texH;
        v[5] = inv_w;
    }

    // Issue one rdpq_triangle per triangle. The index buffer is groups of
    // 3 IndexType values. We skip triangles with any vertex behind the
    // near plane (inv_w == 0 sentinel from above).
    const IndexType* idx = access.indices;
    const uint32_t triCount = access.numIndices / 3;
    for (uint32_t t = 0; t < triCount; ++t)
    {
        const uint32_t i0 = idx[t*3 + 0];
        const uint32_t i1 = idx[t*3 + 1];
        const uint32_t i2 = idx[t*3 + 2];
        const float* v0 = xfm + i0 * 6;
        const float* v1 = xfm + i1 * 6;
        const float* v2 = xfm + i2 * 6;

        // Near-plane reject — at least one vertex must have w > 0.
        if (v0[5] == 0.0f && v1[5] == 0.0f && v2[5] == 0.0f) continue;

        if (texRes != nullptr)
        {
            rdpq_triangle(&TRIFMT_ZBUF_TEX, v0, v1, v2);
        }
        else
        {
            // Solid (no texture). TRIFMT_ZBUF wants { x, y, z } only —
            // build a 3-float subset.
            const float sv0[] = { v0[0], v0[1], v0[2] };
            const float sv1[] = { v1[0], v1[1], v1[2] };
            const float sv2[] = { v2[0], v2[1], v2[2] };
            rdpq_triangle(&TRIFMT_ZBUF, sv0, sv1, sv2);
        }
    }
}

void GFX_CreateSkeletalMeshCompResource(SkeletalMesh3D* /*skeletalMeshComp*/) {}
void GFX_DestroySkeletalMeshCompResource(SkeletalMesh3D* /*skeletalMeshComp*/) {}
void GFX_ReallocateSkeletalMeshCompVertexBuffer(SkeletalMesh3D* /*skeletalMeshComp*/, uint32_t /*numVertices*/) {}
void GFX_UpdateSkeletalMeshCompVertexBuffer(SkeletalMesh3D* /*skeletalMeshComp*/, const std::vector<Vertex>& /*skinnedVertices*/) {}
void GFX_DrawSkeletalMeshComp(SkeletalMesh3D* /*skeletalMeshComp*/) {}
bool GFX_IsCpuSkinningRequired(SkeletalMesh3D* /*skeletalMeshComp*/) { return true; }

void GFX_DrawShadowMeshComp(ShadowMesh3D* /*shadowMeshComp*/) {}
void GFX_DrawInstancedMeshComp(InstancedMesh3D* /*instancedMeshComp*/) {}

void GFX_CreateTextMeshCompResource(TextMesh3D* /*textMeshComp*/) {}
void GFX_DestroyTextMeshCompResource(TextMesh3D* /*textMeshComp*/) {}
void GFX_UpdateTextMeshCompVertexBuffer(TextMesh3D* /*textMeshComp*/, const std::vector<Vertex>& /*vertices*/) {}
void GFX_DrawTextMeshComp(TextMesh3D* /*textMeshComp*/) {}

void GFX_CreateParticleCompResource(Particle3D* /*particleComp*/) {}
void GFX_DestroyParticleCompResource(Particle3D* /*particleComp*/) {}
void GFX_UpdateParticleCompVertexBuffer(Particle3D* /*particleComp*/, const std::vector<VertexParticle>& /*vertices*/) {}
void GFX_DrawParticleComp(Particle3D* /*particleComp*/) {}

// =========================================================================
// Widgets
// =========================================================================

void GFX_CreateQuadResource(Quad* /*quad*/) {}
void GFX_DestroyQuadResource(Quad* /*quad*/) {}
void GFX_UpdateQuadResourceVertexData(Quad* /*quad*/) {}
void GFX_DrawQuad(Quad* /*quad*/) {}

void GFX_CreateQuadBorderResource(Quad* /*quad*/) {}
void GFX_DestroyQuadBorderResource(Quad* /*quad*/) {}
void GFX_UpdateQuadBorderResourceVertexData(Quad* /*quad*/) {}
void GFX_DrawQuadBorder(Quad* /*quad*/) {}

void GFX_CreateTextResource(Text* /*text*/) {}
void GFX_DestroyTextResource(Text* /*text*/) {}
void GFX_UpdateTextResourceVertexData(Text* /*text*/) {}
void GFX_DrawText(Text* /*text*/) {}

void GFX_CreatePolyResource(Poly* /*poly*/) {}
void GFX_DestroyPolyResource(Poly* /*poly*/) {}
void GFX_UpdatePolyResourceVertexData(Poly* /*poly*/) {}
void GFX_DrawPoly(Poly* /*poly*/) {}

// =========================================================================
// Misc
// =========================================================================

void GFX_DrawStaticMesh(StaticMesh* /*mesh*/, Material* /*material*/, const glm::mat4& /*transform*/, glm::vec4 /*color*/) {}
void GFX_RenderPostProcessPasses() {}

// =========================================================================
// Matrix helpers — return identity. Phase 3 swaps to libdragon's matrix
// stack via t3d_perspective / t3d_ortho. For Phase 1 the engine renders
// nothing, so identity is safe.
// =========================================================================

#include "Engine/Nodes/3D/Terrain3d.h"
#include "Engine/Nodes/3D/TileMap2d.h"
#include "Engine/Nodes/3D/Voxel3d.h"

glm::mat4 GFX_MakePerspectiveMatrix(float /*fovyDegrees*/, float /*aspectRatio*/, float /*zNear*/, float /*zFar*/)
{
    return glm::mat4(1.0f);
}

glm::mat4 GFX_MakeOrthographicMatrix(float /*left*/, float /*right*/, float /*bottom*/, float /*top*/, float /*zNear*/, float /*zFar*/)
{
    return glm::mat4(1.0f);
}

// =========================================================================
// Terrain3D / TileMap2D / Voxel3D resources — Phase 1 no-op stubs. The
// engine's Terrain3D::Render etc. call these via virtual dispatch, so they
// must link even if the renderer never actually draws anything.
// =========================================================================

void GFX_CreateVoxel3DResource(Voxel3D* /*voxel*/) {}
void GFX_DestroyVoxel3DResource(Voxel3D* /*voxel*/) {}
void GFX_UpdateVoxel3DResource(Voxel3D* /*voxel*/, const std::vector<VertexColor>& /*vertices*/, const std::vector<IndexType>& /*indices*/) {}
void GFX_DrawVoxel3D(Voxel3D* /*voxel*/) {}

void GFX_CreateTerrain3DResource(Terrain3D* /*terrain*/) {}
void GFX_DestroyTerrain3DResource(Terrain3D* /*terrain*/) {}
void GFX_UpdateTerrain3DResource(Terrain3D* /*terrain*/, const std::vector<VertexColor>& /*vertices*/, const std::vector<IndexType>& /*indices*/) {}
void GFX_DrawTerrain3D(Terrain3D* /*terrain*/) {}

void GFX_CreateTileMap2DResource(TileMap2D* /*tileMap*/) {}
void GFX_DestroyTileMap2DResource(TileMap2D* /*tileMap*/) {}
void GFX_UpdateTileMap2DResource(TileMap2D* /*tileMap*/, const std::vector<VertexColor>& /*vertices*/, const std::vector<IndexType>& /*indices*/) {}
void GFX_DrawTileMap2D(TileMap2D* /*tileMap*/) {}

#endif // POLYPHASE_PLATFORM_ADDON
