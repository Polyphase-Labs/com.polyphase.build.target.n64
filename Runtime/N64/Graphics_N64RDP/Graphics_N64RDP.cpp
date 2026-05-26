#if defined(POLYPHASE_PLATFORM_ADDON)

#include "Graphics/Graphics.h"
#include "Graphics/GraphicsTypes.h"
#include "Graphics/GraphicsConstants.h"
#include "Renderer.h"
#include "World.h"
#include "Engine.h"
#include "Log.h"

#include "Engine/Assets/Texture.h"
#include "Engine/Assets/Material.h"
#include "Engine/Assets/MaterialLite.h"

// libdragon's <malloc.h> omits memalign; forward-declare from newlib.
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
    surface_t* sActiveSurface = nullptr;

    constexpr color_t kClearColor = { 0x00, 0x00, 0x00, 0xFF };

    static uint32_t sFrameCount = 0;

    static surface_t sZBuffer = {};
    static bool      sZBufferReady = false;

    // Set to true to draw a spinning textured cube as a pipeline sanity check.
    constexpr bool kDrawTestTriangle = false;

    alignas(16) static uint16_t sTestTexData[32 * 32];
    static surface_t sTestSurface = {};
    static bool      sTestTexReady = false;

    void EnsureTestTexture()
    {
        if (sTestTexReady) return;
        for (int y = 0; y < 32; ++y)
        {
            for (int x = 0; x < 32; ++x)
            {
                const bool dark = ((x / 4) ^ (y / 4)) & 1;
                sTestTexData[y * 32 + x] = dark ? 0xF03Fu : 0xFFC1u;
            }
        }
        // Flush CPU cache: RDP DMA bypasses cache and would read stale bytes.
        data_cache_hit_writeback_invalidate(sTestTexData, sizeof(sTestTexData));
        sTestSurface = surface_make_linear(sTestTexData, FMT_RGBA16, 32, 32);
        sTestTexReady = true;
    }
}

void GFX_Initialize()
{
    rdpq_init();
    LogDebug("Graphics_N64RDP: rdpq initialised");
}

void GFX_Shutdown()
{
    rdpq_close();
}

namespace { extern bool sViewProjValid; }

void GFX_BeginFrame()
{
    sViewProjValid = false;

    if (!sZBufferReady)
    {
        sZBuffer = surface_alloc(FMT_RGBA16, 320, 240);
        sZBufferReady = (sZBuffer.buffer != nullptr);
    }

    sActiveSurface = display_get();
    if (sActiveSurface == nullptr) return;

    rdpq_attach(sActiveSurface, sZBufferReady ? &sZBuffer : nullptr);
    rdpq_clear(kClearColor);
    if (sZBufferReady)
    {
        rdpq_clear_z(0xFFFC);
    }

    sFrameCount++;
}

static void DrawTestCube()
{
    EnsureTestTexture();

    rdpq_set_mode_standard();
    rdpq_mode_zbuf(true, true);
    rdpq_mode_persp(true);
    rdpq_mode_filter(FILTER_BILINEAR);
    rdpq_tex_upload(TILE0, &sTestSurface, NULL);

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

    struct CV { float px, py, pz, u, v; };
    static const CV cube_verts[8] = {
        {-1.0f, -1.0f, -1.0f, 0.0f, 0.0f},
        { 1.0f, -1.0f, -1.0f, 1.0f, 0.0f},
        { 1.0f,  1.0f, -1.0f, 1.0f, 1.0f},
        {-1.0f,  1.0f, -1.0f, 0.0f, 1.0f},
        {-1.0f, -1.0f,  1.0f, 0.0f, 0.0f},
        { 1.0f, -1.0f,  1.0f, 1.0f, 0.0f},
        { 1.0f,  1.0f,  1.0f, 1.0f, 1.0f},
        {-1.0f,  1.0f,  1.0f, 0.0f, 1.0f},
    };
    static const uint8_t cube_idx[36] = {
        0,1,2, 0,2,3,
        5,4,7, 5,7,6,
        4,0,3, 4,3,7,
        1,5,6, 1,6,2,
        3,2,6, 3,6,7,
        4,5,1, 4,1,0,
    };

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

    for (int t_idx = 0; t_idx < 12; ++t_idx)
    {
        const Xfm& A = xfm[cube_idx[t_idx * 3 + 0]];
        const Xfm& B = xfm[cube_idx[t_idx * 3 + 1]];
        const Xfm& C = xfm[cube_idx[t_idx * 3 + 2]];

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
    // Fixed 320x240 from display_init — ignore editor scene-tab dims.
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

namespace
{
    // N64 RGBA16 (5-5-5-1) packed big-endian.
    inline uint16_t Rgba8888To16(uint8_t r, uint8_t g, uint8_t b, uint8_t a)
    {
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
    if (w == 0 || h == 0 || data.empty()) return;

    const size_t numTexels = (size_t)w * (size_t)h;
    uint16_t* rgba16 = (uint16_t*)memalign(16, numTexels * sizeof(uint16_t));
    if (rgba16 == nullptr) return;

    const uint8_t* src = data.data();
    for (size_t i = 0; i < numTexels; ++i)
    {
        rgba16[i] = Rgba8888To16(src[i*4 + 0], src[i*4 + 1],
                                  src[i*4 + 2], src[i*4 + 3]);
    }
    // RDP DMA bypasses CPU cache; flush so it reads the converted bytes.
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
}

void GFX_CreateMaterialResource(Material* /*material*/) {}
void GFX_DestroyMaterialResource(Material* /*material*/) {}

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

namespace
{
    // Stride-agnostic accessor: Vertex and VertexColor share the same
    // pos/tex0/tex1/normal prefix, only the trailing color differs.
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
        return *reinterpret_cast<const glm::vec2*>(m.base + idx * m.stride + sizeof(glm::vec3));
    }
    inline const glm::vec3& MeshNormal(const N64MeshAccess& m, uint32_t idx)
    {
        return *reinterpret_cast<const glm::vec3*>(m.base + idx * m.stride
                + sizeof(glm::vec3) + 2 * sizeof(glm::vec2));
    }

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
    // Weak refs — engine owns vertex/index buffer lifetime.
    res->mVertexData   = vertices;
    res->mIndexData    = indices;
    res->mNumVertices  = numVertices;
    res->mNumIndices   = numIndices;
    res->mVertexStride = (uint32_t)(hasColor ? sizeof(VertexColor) : sizeof(Vertex));
    res->mVertexFlags  = hasColor ? 1u : 0u;
    debugf("[GFX] StaticMesh created: %lu verts, %lu indices, hasColor=%d\n",
           (unsigned long)numVertices, (unsigned long)numIndices, (int)hasColor);
}

void GFX_DestroyStaticMeshResource(StaticMesh* staticMesh)
{
    if (staticMesh == nullptr) return;
    StaticMeshResource* res = staticMesh->GetResource();
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

    if (!sViewProjValid)
    {
        UpdateCameraCache(staticMeshComp->GetWorld());
        if (!sViewProjValid) return;
    }

    const glm::mat4 model = staticMeshComp->GetTransform();
    const glm::mat4 mvp   = sViewProj * model;
    // Uniform scale, no shear → normal matrix collapses to the 3x3 rotation.
    const glm::mat3 normalMat = glm::mat3(model);

    Material* matBase = staticMeshComp->GetMaterial();
    MaterialLite* mat = Material::AsLite(matBase);
    Texture*  tex     = (mat != nullptr) ? mat->GetTexture(0) : nullptr;
    TextureResource* texRes = (tex != nullptr) ? tex->GetResource() : nullptr;

    const glm::vec3 matColor = (mat != nullptr)
        ? glm::clamp(glm::vec3(mat->GetColor()), 0.0f, 1.0f)
        : glm::vec3(1.0f);
    const float matAlpha = (mat != nullptr)
        ? glm::clamp(mat->GetColor().a, 0.0f, 1.0f)
        : 1.0f;
    const float emission = (mat != nullptr) ? mat->GetEmission() : 0.0f;

    World* world = staticMeshComp->GetWorld();
    const glm::vec3 ambient = world
        ? glm::vec3(world->GetAmbientLightColor())
        : glm::vec3(0.1f);
    const std::vector<LightData>& lights = Renderer::Get()->GetLightData();

    if (texRes != nullptr && texRes->mPixels != nullptr && texRes->mWidth > 0)
    {
        surface_t surf = surface_make_linear(texRes->mPixels, FMT_RGBA16,
                                             texRes->mWidth, texRes->mHeight);
        rdpq_set_mode_standard();
        rdpq_mode_zbuf(true, true);
        rdpq_mode_persp(true);
        rdpq_mode_filter(FILTER_BILINEAR);
        rdpq_mode_combiner(RDPQ_COMBINER_TEX_SHADE);
        rdpq_tex_upload(TILE0, &surf, NULL);
    }
    else
    {
        rdpq_set_mode_standard();
        rdpq_mode_zbuf(true, true);
        rdpq_mode_combiner(RDPQ_COMBINER_SHADE);
    }

    N64MeshAccess access;
    access.base       = static_cast<const uint8_t*>(meshRes->mVertexData);
    access.stride     = meshRes->mVertexStride;
    access.indices    = static_cast<const IndexType*>(meshRes->mIndexData);
    access.numIndices = meshRes->mNumIndices;

    const float texW = (texRes != nullptr) ? (float)texRes->mWidth  : 1.0f;
    const float texH = (texRes != nullptr) ? (float)texRes->mHeight : 1.0f;

    constexpr float kScreenW = 320.0f;
    constexpr float kScreenH = 240.0f;

    // TRIFMT_ZBUF_SHADE_TEX layout: {x,y,z, r,g,b,a, s,t, inv_w}.
    constexpr uint32_t kVertStride = 10;
    constexpr uint32_t kMaxVerts   = 256;
    float xfm[kMaxVerts * kVertStride];

    if (meshRes->mNumVertices > kMaxVerts)
    {
        debugf("[GFX] mesh %lu verts > %lu, draw skipped (need paged transform)\n",
               (unsigned long)meshRes->mNumVertices, (unsigned long)kMaxVerts);
        return;
    }

    for (uint32_t i = 0; i < meshRes->mNumVertices; ++i)
    {
        const glm::vec3& pos = MeshPos(access, i);
        const glm::vec2& uv  = MeshUV(access, i);
        const glm::vec3& nrm = MeshNormal(access, i);

        glm::vec4 clip = mvp * glm::vec4(pos, 1.0f);
        const float w  = clip.w;
        const float inv_w = (w > 0.0001f) ? (1.0f / w) : 0.0f;
        const float ndc_x = clip.x * inv_w;
        const float ndc_y = clip.y * inv_w;
        const float ndc_z = clip.z * inv_w;
        const float sx = (ndc_x * 0.5f + 0.5f) * kScreenW;
        const float sy = (1.0f - (ndc_y * 0.5f + 0.5f)) * kScreenH;
        const float sz = (ndc_z * 0.5f + 0.5f);

        const glm::vec3 worldNormal = glm::normalize(normalMat * nrm);
        glm::vec3 lit = ambient + glm::vec3(emission);
        for (const LightData& ld : lights)
        {
            if (ld.mType == LightType::Directional)
            {
                const float ndotl = glm::max(0.0f,
                    glm::dot(worldNormal, -ld.mDirection));
                lit += glm::vec3(ld.mColor) * ld.mIntensity * ndotl;
            }
            else if (ld.mType == LightType::Point && ld.mRadius > 0.0f)
            {
                const glm::vec3 worldPos = glm::vec3(model * glm::vec4(pos, 1.0f));
                const glm::vec3 toLight = ld.mPosition - worldPos;
                const float dist = glm::length(toLight);
                if (dist < ld.mRadius)
                {
                    const glm::vec3 L = toLight / glm::max(dist, 0.0001f);
                    const float ndotl = glm::max(0.0f, glm::dot(worldNormal, L));
                    const float falloff = 1.0f - (dist / ld.mRadius);
                    lit += glm::vec3(ld.mColor) * ld.mIntensity * ndotl * (falloff * falloff);
                }
            }
        }
        glm::vec3 finalRGB = glm::clamp(lit * matColor, 0.0f, 1.0f);

        float* v = xfm + i * kVertStride;
        v[0] = sx;
        v[1] = sy;
        v[2] = sz;
        v[3] = finalRGB.r;
        v[4] = finalRGB.g;
        v[5] = finalRGB.b;
        v[6] = matAlpha;
        v[7] = uv.x * texW;
        v[8] = uv.y * texH;
        v[9] = inv_w;
    }

    const IndexType* idx = access.indices;
    const uint32_t triCount = access.numIndices / 3;
    for (uint32_t t = 0; t < triCount; ++t)
    {
        const uint32_t i0 = idx[t*3 + 0];
        const uint32_t i1 = idx[t*3 + 1];
        const uint32_t i2 = idx[t*3 + 2];
        const float* v0 = xfm + i0 * kVertStride;
        const float* v1 = xfm + i1 * kVertStride;
        const float* v2 = xfm + i2 * kVertStride;

        if (v0[9] == 0.0f && v1[9] == 0.0f && v2[9] == 0.0f) continue;

        rdpq_triangle(&TRIFMT_ZBUF_SHADE_TEX, v0, v1, v2);
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

void GFX_DrawStaticMesh(StaticMesh* /*mesh*/, Material* /*material*/, const glm::mat4& /*transform*/, glm::vec4 /*color*/) {}
void GFX_RenderPostProcessPasses() {}

#include "Engine/Nodes/3D/Terrain3d.h"
#include "Engine/Nodes/3D/TileMap2d.h"
#include "Engine/Nodes/3D/Voxel3d.h"

// OpenGL-convention NDC (+Y up, z in [-1, 1]) — matches GFX_DrawStaticMeshComp's
// NDC-to-screen mapping.
glm::mat4 GFX_MakePerspectiveMatrix(float fovyDegrees, float aspectRatio, float zNear, float zFar)
{
    return glm::perspective(glm::radians(fovyDegrees), aspectRatio, zNear, zFar);
}

glm::mat4 GFX_MakeOrthographicMatrix(float left, float right, float bottom, float top, float zNear, float zFar)
{
    return glm::ortho(left, right, bottom, top, zNear, zFar);
}

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
