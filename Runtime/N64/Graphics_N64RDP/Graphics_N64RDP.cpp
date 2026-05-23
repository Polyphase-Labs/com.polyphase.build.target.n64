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

void GFX_BeginFrame()
{
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

// Helper: draw a textured rectangle as a first-light test. Uses libdragon's
// 2D rdpq_texture_rectangle path — no triangle setup, no perspective
// divide, no NaN-trap risk. If this paints a 64×64 checkerboard rectangle
// in the top-left of the screen, the rdpq pipeline + texture upload both
// work end-to-end. Triangle rendering / perspective transforms come next
// once this basic path is verified.
static void DrawTestRectangle()
{
    EnsureTestTexture();

    rdpq_set_mode_copy(false);          // copy mode = 1:1 texel→pixel blit
    rdpq_tex_upload(TILE0, &sTestSurface, NULL);

    // Blit the 32×32 texture to a 32×32 screen rect at (20, 20). Use the
    // "rectangle" sprite-style helper which auto-fills the texcoords.
    rdpq_texture_rectangle(TILE0,
                           20, 20,           // screen x0, y0
                           20 + 32, 20 + 32, // screen x1, y1
                           0, 0);            // s0, t0 (texel start)
}

void GFX_EndFrame()
{
    if (sActiveSurface == nullptr) return;

    if (kDrawTestTriangle)
    {
        DrawTestRectangle();
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

void GFX_CreateTextureResource(Texture* /*texture*/, std::vector<uint8_t>& /*data*/) {}
void GFX_DestroyTextureResource(Texture* /*texture*/) {}
void GFX_UpdateTextureResourcePixels(Texture* /*texture*/, const uint8_t* /*src*/,
                                     uint32_t /*offsetX*/, uint32_t /*offsetY*/,
                                     uint32_t /*width*/, uint32_t /*height*/) {}

// =========================================================================
// Materials
// =========================================================================

void GFX_CreateMaterialResource(Material* /*material*/) {}
void GFX_DestroyMaterialResource(Material* /*material*/) {}

// =========================================================================
// Static meshes
// =========================================================================

void GFX_CreateStaticMeshResource(StaticMesh* /*staticMesh*/, bool /*hasColor*/, uint32_t /*numVertices*/, void* /*vertices*/, uint32_t /*numIndices*/, IndexType* /*indices*/) {}
void GFX_DestroyStaticMeshResource(StaticMesh* /*staticMesh*/) {}

void GFX_CreateSkeletalMeshResource(SkeletalMesh* /*skeletalMesh*/, uint32_t /*numVertices*/, VertexSkinned* /*vertices*/, uint32_t /*numIndices*/, IndexType* /*indices*/) {}
void GFX_DestroySkeletalMeshResource(SkeletalMesh* /*skeletalMesh*/) {}

void GFX_CreateStaticMeshCompResource(StaticMesh3D* /*staticMeshComp*/) {}
void GFX_DestroyStaticMeshCompResource(StaticMesh3D* /*staticMeshComp*/) {}
void GFX_UpdateStaticMeshCompResourceColors(StaticMesh3D* /*staticMeshComp*/) {}
void GFX_DrawStaticMeshComp(StaticMesh3D* /*staticMeshComp*/, StaticMesh* /*meshOverride*/) {}

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
