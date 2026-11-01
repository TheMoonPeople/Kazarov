#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include "gfx/context.hpp"
#include "gfx/displaylist.hpp"
#include "tests/gfx_fixture.hpp"

using namespace dc::test;

namespace {

constexpr int                   kWidth = 64;
constexpr int                   kHeight = 48;
const std::array<uint8_t, 4>    kClear = {0, 0, 0, 0x80};
const std::vector<uint32_t>     kTriangle = {0, 1, 2};
constexpr std::array<float, 12> kIdentityBone = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};

GfxOptions Prog0Options() {
    GfxOptions options;
    options.prog0_meshes = true;
    // Its own cache: the plain fixtures' cache is not rewritten with more pipelines by every case here.
    options.pipeline_cache = std::filesystem::temp_directory_path() / "dc_gfx_test" / "pipeline_cache_prog0.bin";
    return options;
}

void Identity(float (&m)[16]) {
    std::memset(m, 0, sizeof m);
    m[0] = m[5] = m[10] = m[15] = 1.0f;
}

template <size_t N>
void Set(float (&dst)[N], std::initializer_list<float> values) {
    size_t i = 0;
    for (float v : values) {
        dst[i++] = v;
    }
}

// clip = (w x, w y, 0.5 w, w): the screen triangle keeps its NDC place and depth 0.5 at clip.w = w.
void SetWMvp(float (&m)[16], float w) {
    std::memset(m, 0, sizeof m);
    m[0] = m[5] = m[15] = w;
    m[14] = 0.5f * w;
}

// mgSetFogParam: the values at near_w and far_w, on a 1/w scale.
void SetFog(float (&fog)[4], float near_w, float far_w, float near_value, float far_value) {
    float range = far_value - near_value;
    fog[0] = (far_value + near_value + range * (far_w + near_w) / (far_w - near_w)) / 2.0f;
    fog[1] = near_value;
    fog[2] = far_value;
    fog[3] = -far_w * near_w * range / (far_w - near_w);
}

// A kMeshProg0 draw: the mesh constants and the Vu_prog0 block the game uploads with them.
struct Prog0Draw {
    gfx::MeshConstants  mesh;
    gfx::Prog0Constants prog0;
    gfx::Prog0Palette   palette;  // read only by a kProg0Skinned draw
    uint32_t            face = 0; // Prog0Face, sent as the DrawState's prog0_face
};

Prog0Draw Prog0(uint32_t flags, uint32_t face = 0) {
    Prog0Draw c{};
    Identity(c.mesh.mvp);
    Identity(c.prog0.model_world);
    Set(c.prog0.diffuse, {1, 1, 1, 1});
    // Offset 255 with near value 255 and scale 0: no fog.
    Set(c.prog0.fog, {255, 255, 0, 0});
    c.prog0.flags = flags;
    c.face = face;
    c.prog0.specular_power = 3;
    return c;
}

gfx::MeshConstants ChronicleLit() {
    gfx::MeshConstants c{};
    c.flags = gfx::kMeshLit;
    c.light_count = 1;
    Identity(c.mvp);
    Set(c.normal_matrix, {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0});
    Set(c.light_direction[0], {0, 0, -1, 0});
    Set(c.light_color[0], {0.5f, 0.25f, 0, 0});
    Set(c.ambient, {0.25f, 0.25f, 0.25f, 0});
    Set(c.ambient_material, {1, 1, 1, 1});
    Set(c.diffuse, {1, 1, 1, 1});
    return c;
}

gfx::Vertex3D Vert(std::array<float, 3> position, std::array<float, 3> normal, std::array<uint8_t, 4> color = {}) {
    return {
        {position[0], position[1], position[2]},
        {normal[0], normal[1], normal[2]},
        {0.0f, 0.0f},
        {color[0], color[1], color[2], color[3]}
    };
}

std::vector<gfx::Vertex3D> ScreenVertices(std::array<float, 3> normal, std::array<uint8_t, 4> color = {}) {
    return {Vert({-1.0f, -1.0f, 0.5f}, normal, color), Vert({3.0f, -1.0f, 0.5f}, normal, color),
            Vert({-1.0f, 3.0f, 0.5f}, normal, color)};
}

// Magnified by UseTinyMvp, so the centre pixel samples model position (0, 0, 0).
std::vector<gfx::Vertex3D> TinyVertices(std::array<float, 3> normal, std::array<uint8_t, 4> color = {}) {
    return {Vert({-0.01f, -0.01f, 0.0f}, normal, color), Vert({0.03f, -0.01f, 0.0f}, normal, color),
            Vert({-0.01f, 0.03f, 0.0f}, normal, color)};
}

std::vector<gfx::Vertex3D> QuadVertices() {
    return {Vert({-1.0f, -1.0f, 0.5f}, {0, 0, 1}), Vert({0.0f, -1.0f, 0.5f}, {0, 0, 1}),
            Vert({0.0f, 1.0f, 0.5f}, {0, 0, 1}), Vert({-1.0f, 1.0f, 0.5f}, {0, 0, 1})};
}

void UseTinyMvp(float (&m)[16]) {
    Identity(m);
    m[0] = 100.0f;
    m[5] = 100.0f;
    m[14] = 0.5f;
}

// Each vertex followed by its skin, as Vu_prog0 reads a kProg0Skinned mesh.
std::vector<gfx::Vertex3D> Skinned(const std::vector<gfx::Vertex3D> &vertices, std::array<float, 4> weight,
                                   std::array<uint8_t, 4> bone) {
    gfx::Prog0Skin skin = {};
    for (size_t i = 0; i < 4; i++) {
        skin.weight[i] = weight[i];
        skin.bone[i] = bone[i];
    }
    gfx::Vertex3D pair = {};
    std::memcpy(&pair, &skin, sizeof pair);
    std::vector<gfx::Vertex3D> out;
    for (const gfx::Vertex3D &vertex : vertices) {
        out.push_back(vertex);
        out.push_back(pair);
    }
    return out;
}

void SetDirectional(gfx::Prog0Constants &p, uint32_t light, std::array<float, 3> direction,
                    std::array<float, 4> color) {
    for (uint32_t k = 0; k < 3; k++) {
        p.light_direction[k][light] = direction[k];
    }
    for (uint32_t k = 0; k < 4; k++) {
        p.light_color[light][k] = color[k];
    }
}

void SetPoint(gfx::Prog0Constants &p, uint32_t k, std::array<float, 4> position_power, std::array<float, 4> color) {
    for (uint32_t j = 0; j < 4; j++) {
        p.point_light[k][j] = position_power[j];
        p.point_color[k][j] = color[j];
    }
}

// Rows 0-2 of the bone's 4x4, row-major.
void SetBone(gfx::Prog0Palette &p, uint32_t slot, const std::array<float, 12> &rows) {
    for (uint32_t r = 0; r < 3; r++) {
        for (uint32_t c = 0; c < 4; c++) {
            p.bones[slot][r][c] = rows[r * 4 + c];
        }
    }
}

Prog0Draw Unlit(uint32_t flags, uint32_t face = 0) {
    Prog0Draw c = Prog0(flags, face);
    Set(c.prog0.object_color, {128, 64, 32, 128});
    Set(c.prog0.diffuse, {0.5f, 0.5f, 0.5f, 1});
    SetDirectional(c.prog0, 0, {0, 0, 1}, {255, 255, 255, 0});
    return c;
}

Prog0Draw FourLights() {
    Prog0Draw c = Prog0(0);
    Set(c.prog0.ambient, {10, 20, 30, 128});
    SetDirectional(c.prog0, 0, {0, 0, 1}, {40, 0, 0, 0});
    SetDirectional(c.prog0, 1, {0, 0.6f, 0.8f}, {0, 50, 0, 0});
    SetDirectional(c.prog0, 2, {0, 0, -1}, {0, 0, 200, 0});
    SetDirectional(c.prog0, 3, {1, 0, 0}, {0, 0, 90, 0});
    return c;
}

gfx::TextureHandle RgbQuadTexture() {
    gfx::TextureHandle texture = gfx::CreateTexture(gfx::TextureDesc{2, 2});
    const uint8_t      texels[16] = {255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 255, 255, 255, 255};
    EXPECT_TRUE(gfx::UpdateTexture(texture, 0, 0, 0, 2, 2, texels));
    return texture;
}

gfx::TextureBinding NearestBinding(gfx::TextureHandle texture) {
    gfx::TextureBinding binding;
    binding.texture = texture;
    binding.filter = gfx::Filter::Nearest;
    return binding;
}

void Render(GfxFixture &fixture, const std::vector<gfx::Vertex3D> &vertices, const std::vector<uint32_t> &indices,
            const gfx::MeshConstants &constants, const gfx::TextureBinding &texture = {},
            const gfx::DrawState &state = {}) {
    fixture.Frame(kClear, [&] { gfx::DrawMeshImmediate(vertices, indices, constants, texture, state); });
}

void Render(GfxFixture &fixture, const std::vector<gfx::Vertex3D> &vertices, const std::vector<uint32_t> &indices,
            const Prog0Draw &draw, const gfx::TextureBinding &texture = {}, const gfx::DrawState &state = {}) {
    gfx::DrawState draw_state = state;
    draw_state.prog0_face = draw.face;
    const gfx::Prog0Palette *palette = (draw.prog0.flags & gfx::kProg0Skinned) != 0 ? &draw.palette : nullptr;
    fixture.Frame(kClear, [&] {
        gfx::DrawMeshImmediateProg0(vertices, indices, draw.mesh, draw.prog0, texture, draw_state, nullptr, palette);
    });
}

bool Centre(const GfxFixture &fixture, int r, int g, int b, int tolerance = 1) {
    return fixture.PixelNear(fixture.width / 2, fixture.height / 2, r, g, b, tolerance);
}

// On the y=1 plane, w=1/screen_y. The two near corners share a homogeneous edge weight h.
float EyePlaneFog(float depth, float inverse_w, float fresh) {
    const float h = (4.0f - 1.0f / inverse_w) / (4.0f - depth);
    // Both near corners have |x|=10, which dominates their homogeneous norm.
    const float marker = (1.0f - depth) / 10.0f;
    if (depth <= 0.0f) {
        return h * marker * fresh;
    }
    const float vertex_weight = h * depth * (1.0f - marker);
    const float vertex = 255.0f * vertex_weight / (vertex_weight + (1.0f - h) * 4.0f);
    return vertex + h * marker * (fresh - vertex);
}

} // namespace

TEST(GfxMeshDc2, MeshConstantsKeepChroniclesSize) {
    EXPECT_EQ(sizeof(gfx::MeshConstants), 400u);
}

TEST(GfxGpuMeshDc2, PipelinesOnlyWhenAsked) {
    uint32_t plain = 0;
    {
        GfxFixture fixture(kWidth, kHeight);
        plain = gfx::PipelineCount();
    }
    ASSERT_EQ(plain % 3, 0u);
    GfxFixture fixture(kWidth, kHeight, 1.0f, Prog0Options());
    EXPECT_EQ(gfx::PipelineCount(), plain / 3 * 5);
}

TEST(GfxGpuMeshDc2, PlainMeshesIgnoreTheProg0Pipelines) {
    std::array<uint8_t, 4> plain = {};
    {
        GfxFixture fixture(kWidth, kHeight);
        Render(fixture, ScreenVertices({0, 0, 1}), kTriangle, ChronicleLit());
        plain = fixture.Pixel(fixture.width / 2, fixture.height / 2);
    }
    GfxFixture fixture(kWidth, kHeight, 1.0f, Prog0Options());
    Render(fixture, ScreenVertices({0, 0, 1}), kTriangle, ChronicleLit());
    EXPECT_EQ(fixture.Pixel(fixture.width / 2, fixture.height / 2), plain);
}

TEST(GfxGpuMeshDc2, PlainDrawWithProg0FlagDrawsPlain) {
    GfxFixture         fixture(kWidth, kHeight, 1.0f, Prog0Options());
    gfx::MeshConstants plain = ChronicleLit();
    Render(fixture, ScreenVertices({0, 0, 1}), kTriangle, plain);
    const std::array<uint8_t, 4> expected = fixture.Pixel(fixture.width / 2, fixture.height / 2);
    gfx::MeshConstants           flagged = plain;
    flagged.flags |= gfx::kMeshProg0;
    Render(fixture, ScreenVertices({0, 0, 1}), kTriangle, flagged);
    EXPECT_EQ(fixture.Pixel(fixture.width / 2, fixture.height / 2), expected);
}

TEST(GfxGpuMeshDc2, IndexedProg0MeshMatchesImmediate) {
    GfxFixture                       fixture(kWidth, kHeight, 1.0f, Prog0Options());
    const Prog0Draw                  c = FourLights();
    const std::vector<gfx::Vertex3D> vertices = ScreenVertices({0, 0, 1});
    Render(fixture, vertices, kTriangle, c);
    const std::array<uint8_t, 4> immediate = fixture.Pixel(fixture.width / 2, fixture.height / 2);

    gfx::MeshHandle mesh = gfx::CreateMesh(vertices, kTriangle);
    ASSERT_NE(mesh, gfx::kNullMesh);
    fixture.Frame(kClear, [&] { gfx::DrawMeshProg0(mesh, 0, 3, c.mesh, c.prog0, {}, {}); });
    EXPECT_EQ(fixture.Pixel(fixture.width / 2, fixture.height / 2), immediate);
    EXPECT_TRUE(Centre(fixture, 50, 60, 30));
    gfx::DestroyMesh(mesh);
}

TEST(GfxGpuMeshDc2, RecordedProg0DrawReplaysThroughRenderList) {
    GfxFixture                       fixture(kWidth, kHeight, 1.0f, Prog0Options());
    const Prog0Draw                  c = FourLights();
    const std::vector<gfx::Vertex3D> vertices = ScreenVertices({0, 0, 1});
    Render(fixture, vertices, kTriangle, c);
    const std::array<uint8_t, 4> direct = fixture.Pixel(fixture.width / 2, fixture.height / 2);

    gfx::BeginRecording();
    gfx::Clear(true, kClear.data(), true, 0.0f);
    gfx::DrawMeshImmediateProg0(vertices, kTriangle, c.mesh, c.prog0, {}, {});
    gfx::DisplayListRef list = gfx::EndRecording();
    ASSERT_TRUE(list != nullptr);
    gfx::RenderOptions options;
    options.canonical = true;
    ASSERT_TRUE(gfx::RenderList(*list, 1.0f, options));
    ASSERT_TRUE(gfx::ReadbackFrame(fixture.pixels, fixture.width, fixture.height));
    EXPECT_EQ(fixture.Pixel(fixture.width / 2, fixture.height / 2), direct);
    EXPECT_TRUE(Centre(fixture, 50, 60, 30));
}

TEST(GfxGpuMeshDc2, FourDirectionalLightsSaturate) {
    GfxFixture fixture(kWidth, kHeight, 1.0f, Prog0Options());
    Prog0Draw  c = Prog0(0);
    Set(c.prog0.ambient, {200, 200, 200, 128});
    SetDirectional(c.prog0, 0, {0, 0, 1}, {100, 100, 100, 0});
    Render(fixture, ScreenVertices({0, 0, 1}), kTriangle, c);
    EXPECT_TRUE(Centre(fixture, 255, 255, 255));
}

TEST(GfxGpuMeshDc2, LightsTurnIntoModelSpace) {
    GfxFixture fixture(kWidth, kHeight, 1.0f, Prog0Options());
    Prog0Draw  c = Prog0(0);
    Set(c.prog0.model_world, {0, 2, 0, 0, -2, 0, 0, 0, 0, 0, 2, 0, 0, 0, 0, 1});
    SetDirectional(c.prog0, 0, {1, 0, 0}, {100, 0, 0, 0});
    Render(fixture, ScreenVertices({0, -1, 0}), kTriangle, c);
    EXPECT_TRUE(Centre(fixture, 100, 0, 0));
    Render(fixture, ScreenVertices({0, 1, 0}), kTriangle, c);
    EXPECT_TRUE(Centre(fixture, 0, 0, 0));
}

TEST(GfxGpuMeshDc2, Unlit) {
    GfxFixture fixture(kWidth, kHeight, 1.0f, Prog0Options());
    Render(fixture, ScreenVertices({0, 0, 1}), kTriangle, Unlit(gfx::kProg0Unlit));
    EXPECT_TRUE(Centre(fixture, 64, 32, 16));
    Render(fixture, ScreenVertices({0, 0, 1}), kTriangle, Unlit(0, gfx::kProg0FaceNoNormal));
    EXPECT_TRUE(Centre(fixture, 64, 32, 16));
}

TEST(GfxGpuMeshDc2, PointLights) {
    GfxFixture fixture(kWidth, kHeight, 1.0f, Prog0Options());
    Prog0Draw  c = Prog0(gfx::kProg0PointLights);
    UseTinyMvp(c.mesh.mvp);
    Set(c.prog0.ambient, {10, 10, 10, 0});
    SetPoint(c.prog0, 0, {0, 0, 2, 1}, {128, 0, 0, 0});
    SetPoint(c.prog0, 1, {0, 0, 0.5f, 1}, {0, 64, 0, 0});
    SetPoint(c.prog0, 2, {0, 0, -2, 10}, {0, 0, 255, 0});
    // O-4: power enters squared, so a negative power attenuates as its magnitude.
    SetPoint(c.prog0, 3, {0, 0, 2, -1}, {0, 0, 128, 0});
    Render(fixture, TinyVertices({0, 0, 1}), kTriangle, c);
    EXPECT_TRUE(Centre(fixture, 42, 74, 42));
}

TEST(GfxGpuMeshDc2, RealSpecular) {
    GfxFixture fixture(kWidth, kHeight, 1.0f, Prog0Options());
    Prog0Draw  c = Prog0(gfx::kProg0Real);
    UseTinyMvp(c.mesh.mvp);
    Set(c.prog0.diffuse, {0.25f, 0.25f, 0.25f, 1});
    Set(c.prog0.specular, {1, 1, 1, 1});
    // Five units along a unit direction 0.9 off the normal: the reflection term is 0.9 at the origin.
    Set(c.prog0.eye, {2.1794495f, 0, 4.5f, 1});
    SetDirectional(c.prog0, 0, {0, 0, 1}, {128, 128, 128, 0});
    Render(fixture, TinyVertices({0, 0, 1}), kTriangle, c);
    EXPECT_TRUE(Centre(fixture, 55, 55, 55, 2));
    c.prog0.specular_power = 1;
    Render(fixture, TinyVertices({0, 0, 1}), kTriangle, c);
    EXPECT_TRUE(Centre(fixture, 115, 115, 115, 2));
    c.prog0.specular_power = 3;
    c.prog0.flags = gfx::kProg0Real | gfx::kProg0PointLights;
    Render(fixture, TinyVertices({0, 0, 1}), kTriangle, c);
    EXPECT_TRUE(Centre(fixture, 32, 32, 32, 2));
}

TEST(GfxGpuMeshDc2, VertexColour) {
    GfxFixture fixture(kWidth, kHeight, 1.0f, Prog0Options());
    Prog0Draw  c = Prog0(gfx::kProg0Unlit, gfx::kProg0FaceColor);
    Set(c.prog0.object_color, {100, 100, 100, 128});
    Render(fixture, ScreenVertices({0, 0, 1}, {64, 64, 64, 128}), kTriangle, c);
    EXPECT_TRUE(Centre(fixture, 50, 50, 50));
    c.prog0.flags = gfx::kProg0Unlit | gfx::kProg0ColorAdd;
    Render(fixture, ScreenVertices({0, 0, 1}, {64, 64, 64, 128}), kTriangle, c);
    EXPECT_TRUE(Centre(fixture, 164, 164, 164));
    // O-10: the ColorAdd alpha is the squared vertex alpha, 128 * 0.5^2 = 32, and that is what blends.
    gfx::DrawState blend;
    blend.blend = true;
    blend.alpha = gfx::GsBlend{0, 2, 0, 2};
    Render(fixture, ScreenVertices({0, 0, 1}, {64, 64, 64, 64}), kTriangle, c, {}, blend);
    EXPECT_TRUE(Centre(fixture, 41, 41, 41, 2));
}

TEST(GfxGpuMeshDc2, RefSphere) {
    GfxFixture         fixture(kWidth, kHeight, 1.0f, Prog0Options());
    gfx::TextureHandle texture = RgbQuadTexture();
    ASSERT_NE(texture, gfx::kNullTexture);
    gfx::TextureBinding binding = NearestBinding(texture);

    Prog0Draw c = Prog0(gfx::kProg0Unlit | gfx::kProg0RefSphere);
    Set(c.prog0.object_color, {128, 128, 128, 128});
    Identity(c.prog0.clip_or_view);
    Render(fixture, ScreenVertices({0.5f, -0.5f, 0.70710678f}), kTriangle, c, binding);
    EXPECT_TRUE(Centre(fixture, 0, 255, 0));
    Render(fixture, ScreenVertices({-0.5f, 0.5f, 0.70710678f}), kTriangle, c, binding);
    EXPECT_TRUE(Centre(fixture, 0, 0, 255));
    gfx::DestroyTexture(texture);
}

TEST(GfxGpuMeshDc2, RefSphereIgnoresSkinning) {
    GfxFixture         fixture(kWidth, kHeight, 1.0f, Prog0Options());
    gfx::TextureHandle texture = RgbQuadTexture();
    ASSERT_NE(texture, gfx::kNullTexture);
    gfx::TextureBinding binding = NearestBinding(texture);

    Prog0Draw c = Prog0(gfx::kProg0Unlit | gfx::kProg0RefSphere | gfx::kProg0Skinned);
    Set(c.prog0.object_color, {128, 128, 128, 128});
    Identity(c.prog0.clip_or_view);
    SetBone(c.palette, 0, {0, -1, 0, 0, 1, 0, 0, 0, 0, 0, 1, 0});
    // O-9: the env UV is taken from the unblended normal, before the skin is applied.
    Render(fixture, Skinned(ScreenVertices({0.5f, -0.5f, 0.70710678f}), {1, 0, 0, 0}, {0, 0, 0, 0}), kTriangle, c,
           binding);
    EXPECT_TRUE(Centre(fixture, 0, 255, 0));
    gfx::DestroyTexture(texture);
}

TEST(GfxGpuMeshDc2, Skinning) {
    GfxFixture fixture(kWidth, kHeight, 1.0f, Prog0Options());
    Prog0Draw  c = Prog0(gfx::kProg0Unlit | gfx::kProg0Skinned);
    Set(c.prog0.object_color, {200, 200, 200, 128});
    SetBone(c.palette, 0, kIdentityBone);
    SetBone(c.palette, 1, {1, 0, 0, 1, 0, 1, 0, 0, 0, 0, 1, 0});
    Render(fixture, Skinned(QuadVertices(), {0.5f, 0.5f, 0, 0}, {0, 1, 0, 0}), {0, 1, 2, 0, 2, 3}, c);
    EXPECT_TRUE(fixture.PixelNear(fixture.width * 5 / 8, fixture.height / 2, 200, 200, 200));
    EXPECT_TRUE(fixture.PixelNear(fixture.width / 8, fixture.height / 2, 0, 0, 0));
}

TEST(GfxGpuMeshDc2, SkinnedNormals) {
    GfxFixture fixture(kWidth, kHeight, 1.0f, Prog0Options());
    Prog0Draw  c = Prog0(gfx::kProg0Skinned);
    SetBone(c.palette, 0, kIdentityBone);
    SetDirectional(c.prog0, 0, {0, 0, 1}, {100, 100, 100, 0});
    Render(fixture, Skinned(ScreenVertices({0, 0, 1}), {1, 0, 0, 0}, {0, 0, 0, 0}), kTriangle, c);
    EXPECT_TRUE(Centre(fixture, 100, 100, 100));
}

// prog0.md 10.6, bone-3 sub-case (O-8).
TEST(GfxGpuMeshDc2, SkinnedFourthInfluenceNormal) {
    GfxFixture fixture(kWidth, kHeight, 1.0f, Prog0Options());
    Prog0Draw  c = Prog0(gfx::kProg0Skinned);
    SetBone(c.palette, 3, kIdentityBone);
    const std::vector<gfx::Vertex3D> vertices = Skinned(ScreenVertices({0, 0, 1}), {0, 0, 0, 1}, {0, 0, 0, 3});
    // O-8: influence 3's z term reads column 1, so the retail normal is (0, 1, 0).
    SetDirectional(c.prog0, 0, {0, 1, 0}, {100, 100, 100, 0});
    Render(fixture, vertices, kTriangle, c);
    EXPECT_TRUE(Centre(fixture, 100, 100, 100));
    SetDirectional(c.prog0, 0, {0, 0, 1}, {100, 100, 100, 0});
    Render(fixture, vertices, kTriangle, c);
    EXPECT_TRUE(Centre(fixture, 0, 0, 0));
}

TEST(GfxGpuMeshDc2, ScissorClipsToClipOrView) {
    GfxFixture fixture(kWidth, kHeight, 1.0f, Prog0Options());
    Prog0Draw  c = Prog0(gfx::kProg0Unlit | gfx::kProg0Scissor);
    Set(c.prog0.object_color, {200, 200, 200, 128});
    Set(c.prog0.clip_or_view, {2, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1});
    Render(fixture, ScreenVertices({0, 0, 1}), kTriangle, c);
    EXPECT_TRUE(fixture.PixelNear(fixture.width / 2, fixture.height / 2, 200, 200, 200));
    EXPECT_TRUE(fixture.PixelNear(fixture.width * 7 / 8, fixture.height / 2, 0, 0, 0));
    c.prog0.flags = gfx::kProg0Unlit;
    Render(fixture, ScreenVertices({0, 0, 1}), kTriangle, c);
    EXPECT_TRUE(fixture.PixelNear(fixture.width * 7 / 8, fixture.height / 2, 200, 200, 200));
}

TEST(GfxGpuMeshDc2, Prog0CullModeFollowsFlagAndMirror) {
    EXPECT_EQ(gfx::Prog0CullMode(Prog0(0).prog0), gfx::CullMode::None);
    EXPECT_EQ(gfx::Prog0CullMode(Prog0(gfx::kProg0Cull).prog0), gfx::CullMode::Back);
    Prog0Draw mirrored = Prog0(gfx::kProg0Unlit | gfx::kProg0Cull);
    Set(mirrored.prog0.model_world, {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, -1, 0, 0, 0, 0, 1});
    EXPECT_EQ(gfx::Prog0CullMode(mirrored.prog0), gfx::CullMode::Front);

    GfxFixture                       fixture(kWidth, kHeight, 1.0f, Prog0Options());
    const std::vector<gfx::Vertex3D> triangle = {Vert({0.0f, 0.0f, 0.5f}, {0, 0, 1}),
                                                 Vert({0.5f, 0.0f, 0.5f}, {0, 0, 1}),
                                                 Vert({0.0f, 0.5f, 0.5f}, {0, 0, 1})};
    Prog0Draw                        c = Prog0(gfx::kProg0Unlit | gfx::kProg0Cull);
    Set(c.prog0.object_color, {200, 200, 200, 128});
    gfx::DrawState state;
    state.cull = gfx::Prog0CullMode(c.prog0);
    // Pixel (35, 26) is at NDC (0.11, 0.10), inside the triangle.
    Render(fixture, triangle, {0, 1, 2}, c, {}, state);
    EXPECT_TRUE(fixture.PixelNear(35, 26, 0, 0, 0));
    Render(fixture, triangle, {0, 2, 1}, c, {}, state);
    EXPECT_TRUE(fixture.PixelNear(35, 26, 200, 200, 200));

    Prog0Draw draw_mirrored = c;
    Set(draw_mirrored.prog0.model_world, {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, -1, 0, 0, 0, 0, 1});
    gfx::DrawState mirrored_state;
    mirrored_state.cull = gfx::Prog0CullMode(draw_mirrored.prog0);
    Render(fixture, triangle, {0, 1, 2}, draw_mirrored, {}, mirrored_state);
    EXPECT_TRUE(fixture.PixelNear(35, 26, 200, 200, 200));
}

TEST(GfxGpuMeshDc2, Fog) {
    GfxFixture fixture(kWidth, kHeight, 1.0f, Prog0Options());
    Prog0Draw  c = Prog0(gfx::kProg0Unlit);
    Set(c.prog0.object_color, {255, 255, 255, 128});
    SetFog(c.prog0.fog, 10, 1000, 255, 0);
    gfx::DrawState state;
    state.fog = true;
    auto render_at = [&](float w) {
        SetWMvp(c.mesh.mvp, w);
        Render(fixture, ScreenVertices({0, 0, 1}), kTriangle, c, {}, state);
    };
    render_at(10);
    EXPECT_TRUE(Centre(fixture, 255, 255, 255));
    render_at(1000);
    EXPECT_TRUE(Centre(fixture, 0, 0, 0));
    render_at(100);
    EXPECT_TRUE(Centre(fixture, 23, 23, 23));
}

TEST(GfxGpuMeshDc2, EditorFogSaturatesAtFarValue) {
    GfxFixture fixture(kWidth, kHeight, 1.0f, Prog0Options());
    Prog0Draw  c = Prog0(gfx::kProg0Unlit);
    Set(c.prog0.object_color, {255, 255, 255, 128});
    SetFog(c.prog0.fog, 10, 1000, 0, 255);
    SetWMvp(c.mesh.mvp, 100);
    gfx::DrawState state;
    state.fog = true;
    // The shader clamps with min against the near value, then max against the far value, so the
    // result is the far value at every w.
    Render(fixture, ScreenVertices({0, 0, 1}), kTriangle, c, {}, state);
    EXPECT_TRUE(Centre(fixture, 255, 255, 255));
}

TEST(GfxGpuMeshDc2, FogInterpolatesInScreenSpace) {
    GfxFixture fixture(kWidth, kHeight, 1.0f, Prog0Options());
    Prog0Draw  c = Prog0(gfx::kProg0Unlit);
    Set(c.prog0.object_color, {255, 255, 255, 128});
    SetFog(c.prog0.fog, 1, 4, 255, 0);
    std::memset(c.mesh.mvp, 0, sizeof c.mesh.mvp);
    c.mesh.mvp[0] = c.mesh.mvp[5] = c.mesh.mvp[11] = c.mesh.mvp[14] = 1;
    const std::vector<gfx::Vertex3D> vertices = {
        Vert({-1, -1, 1}, {0, 0, 1}), Vert({1, -1, 1}, {0, 0, 1}),
        Vert({4, 4, 4}, {0, 0, 1}), Vert({-4, 4, 4}, {0, 0, 1})};
    gfx::DrawState state;
    state.fog = true;
    for (const auto &indices : {
             std::vector<uint32_t>{0, 1, 2, 0, 2, 3},
             std::vector<uint32_t>{0, 1, 3, 1, 2, 3}
    }) {
        Render(fixture, vertices, indices, c, {}, state);
        // The GS interpolates F across screen rows; applying 1/w again biases it toward the near edge.
        for (int y : {8, 24, 40}) {
            const int fog = static_cast<int>(255.0f * (1.0f - (y + 0.5f) / kHeight) + 0.5f);
            EXPECT_TRUE(fixture.PixelNear(kWidth / 2, y, fog, fog, fog)) << "row " << y;
        }
    }
}

TEST(GfxGpuMeshDc2, FogClampsVerticesBeforeScreenInterpolation) {
    GfxFixture fixture(kWidth, kHeight, 1.0f, Prog0Options());
    Prog0Draw  c = Prog0(gfx::kProg0Unlit);
    Set(c.prog0.object_color, {255, 255, 255, 128});
    SetFog(c.prog0.fog, 1, 4, 255, 0);
    std::memset(c.mesh.mvp, 0, sizeof c.mesh.mvp);
    c.mesh.mvp[0] = c.mesh.mvp[5] = c.mesh.mvp[11] = 1;
    c.mesh.mvp[14] = 0.1f;
    const std::vector<gfx::Vertex3D> vertices = {
        Vert({-0.5f, -0.5f, 0.5f}, {0, 0, 1}), Vert({0.5f, -0.5f, 0.5f}, {0, 0, 1}),
        Vert({0, 4, 4}, {0, 0, 1})};
    gfx::DrawState state;
    state.fog = true;
    for (const auto &indices : {
             std::vector<uint32_t>{0, 1, 2},
             std::vector<uint32_t>{1, 2, 0},
             std::vector<uint32_t>{2, 0, 1}
    }) {
        Render(fixture, vertices, indices, c, {}, state);
        for (int y : {8, 24, 40}) {
            const int fog = static_cast<int>(255.0f * (1.0f - (y + 0.5f) / kHeight) + 0.5f);
            EXPECT_TRUE(fixture.PixelNear(kWidth / 2, y, fog, fog, fog, 1)) << "row " << y;
        }
    }
}

// A near-clipped neighbour must agree with the visible triangle on their unchanged edge.
TEST(GfxGpuMeshDc2, FogIsContinuousAcrossSharedClippedEdge) {
    for (bool behind_eye : {false, true}) {
        for (int size : {256, 1024}) {
            for (bool plain : {false, true}) {
                GfxFixture fixture(size, size, 1.0f, Prog0Options());
                Prog0Draw  c = Prog0(gfx::kProg0Unlit);
                Set(c.prog0.object_color, {255, 255, 255, 128});
                Set(c.prog0.fog, {-85, 255, 0, 340});
                c.mesh.flags = gfx::kMeshFog;
                Set(c.mesh.diffuse, {255.0f / 128.0f, 255.0f / 128.0f, 255.0f / 128.0f, 1});
                Set(c.mesh.fog, {-85, 340, 0, 255});
                std::memset(c.mesh.mvp, 0, sizeof c.mesh.mvp);
                c.mesh.mvp[0] = c.mesh.mvp[5] = c.mesh.mvp[11] = 1;
                c.mesh.mvp[14] = 0.1f;
                const std::array<float, 3>       clipped = behind_eye ? std::array<float, 3>{0, -0.9f, -1}
                                                                      : std::array<float, 3>{0, -0.045f, 0.05f};
                const std::vector<gfx::Vertex3D> vertices = {
                    Vert({-0.4f, 0, 0.5f}, {0, 0, 1}), Vert({3.2f, 0, 4}, {0, 0, 1}),
                    Vert({0, 3.6f, 4}, {0, 0, 1}), Vert(clipped, {0, 0, 1})};
                gfx::DrawState state;
                state.fog = true;
                int max_step = 0;
                for (const auto &indices : {
                         std::vector<uint32_t>{0, 1, 2, 0, 3, 1},
                         std::vector<uint32_t>{1, 2, 0, 3, 1, 0}
                }) {
                    if (plain) {
                        Render(fixture, vertices, indices, c.mesh, {}, state);
                    } else {
                        Render(fixture, vertices, indices, c, {}, state);
                    }
                    for (int x = size * 5 / 16; x < size * 11 / 16; ++x) {
                        const int a = fixture.pixels[((size / 2 - 1) * fixture.width + x) * 4];
                        const int b = fixture.pixels[(size / 2 * fixture.width + x) * 4];
                        max_step = std::max(max_step, std::abs(a - b));
                    }
                }
                // The one-pixel step must shrink with resolution, rather than retain a fog-range seam.
                EXPECT_LE(max_step, 2048 / size + 1) << "plain=" << plain << " size=" << size << " behind_eye=" << behind_eye;
                RecordProperty(std::string(plain ? "plain_edge_step_" : "prog0_edge_step_") + std::to_string(size) + (behind_eye ? "_behind_eye" : ""), max_step);
            }
        }
    }
}

TEST(GfxGpuMeshDc2, FogChangesContinuouslyAcrossNearPlane) {
    for (bool plain : {false, true}) {
        GfxFixture fixture(256, 256, 1.0f, Prog0Options());
        Prog0Draw  c = Prog0(gfx::kProg0Unlit);
        Set(c.prog0.object_color, {255, 255, 255, 128});
        Set(c.prog0.fog, {-85, 255, 0, 340});
        c.mesh.flags = gfx::kMeshFog;
        Set(c.mesh.diffuse, {255.0f / 128.0f, 255.0f / 128.0f, 255.0f / 128.0f, 1});
        Set(c.mesh.fog, {-85, 340, 0, 255});
        std::memset(c.mesh.mvp, 0, sizeof c.mesh.mvp);
        c.mesh.mvp[0] = c.mesh.mvp[5] = c.mesh.mvp[11] = 1;
        c.mesh.mvp[14] = 0.1f;
        gfx::DrawState state;
        state.fog = true;
        std::vector<uint8_t> previous;
        int                  max_step = 0;
        // Includes exactly on, immediately inside, and immediately outside z == w.
        for (int step = -20; step <= 20; ++step) {
            const float                      w = 0.1f + step * 0.00005f;
            const std::vector<gfx::Vertex3D> vertices = {
                Vert({-0.4f, -0.25f, 0.5f}, {0, 0, 1}), Vert({3.2f, -2, 4}, {0, 0, 1}),
                Vert({0, 0.8f * w, w}, {0, 0, 1})};
            if (plain) {
                Render(fixture, vertices, kTriangle, c.mesh, {}, state);
            } else {
                Render(fixture, vertices, kTriangle, c, {}, state);
            }
            if (!previous.empty()) {
                for (int y = 80; y < 160; ++y) {
                    for (int x = 112; x < 144; ++x) {
                        const int i = (y * fixture.width + x) * 4;
                        max_step = std::max(max_step, std::abs(int(fixture.pixels[i]) - int(previous[i])));
                    }
                }
            }
            previous = fixture.pixels;
        }
        EXPECT_LE(max_step, 1) << "plain=" << plain;
        RecordProperty(plain ? "plain_time_step" : "prog0_time_step", max_step);
    }
}

TEST(GfxGpuMeshDc2, FogChangesContinuouslyBehindEyeAcrossFarPlane) {
    for (bool plain : {false, true}) {
        GfxFixture fixture(256, 256, 1.0f, Prog0Options());
        Prog0Draw  c = Prog0(gfx::kProg0Unlit);
        Set(c.prog0.object_color, {255, 255, 255, 128});
        Set(c.prog0.fog, {-85, 255, 0, 340});
        c.mesh.flags = gfx::kMeshFog;
        Set(c.mesh.diffuse, {255.0f / 128.0f, 255.0f / 128.0f, 255.0f / 128.0f, 1});
        Set(c.mesh.fog, {-85, 340, 0, 255});
        std::memset(c.mesh.mvp, 0, sizeof c.mesh.mvp);
        c.mesh.mvp[0] = c.mesh.mvp[5] = c.mesh.mvp[11] = 1;
        c.mesh.mvp[14] = 0.1f;
        const std::vector<gfx::Vertex3D> vertices = {
            Vert({-0.4f, 0, 0.5f}, {0, 0, 1}), Vert({3.2f, 0, 4}, {0, 0, 1}),
            Vert({0, 1, -1}, {0, 0, 1})};
        gfx::DrawState state;
        state.fog = true;
        std::vector<uint8_t> previous;
        int                  max_step = 0;
        for (int step = -10; step <= 10; ++step) {
            // Only the already eye-clipped corner moves across z=0; projected positions stay fixed.
            c.mesh.mvp[6] = step * 0.0001f - 0.1f;
            if (plain) {
                Render(fixture, vertices, kTriangle, c.mesh, {}, state);
            } else {
                Render(fixture, vertices, kTriangle, c, {}, state);
            }
            if (!previous.empty()) {
                for (int y = 144; y < 192; ++y) {
                    for (int x = 112; x < 144; ++x) {
                        const int i = (y * fixture.width + x) * 4;
                        max_step = std::max(max_step, std::abs(int(fixture.pixels[i]) - int(previous[i])));
                    }
                }
            }
            previous = fixture.pixels;
        }
        EXPECT_LE(max_step, 1) << "plain=" << plain;
        RecordProperty(plain ? "plain_behind_eye_step" : "prog0_behind_eye_step", max_step);
    }
}

TEST(GfxGpuMeshDc2, FogChangesContinuouslyThroughZeroDepthAndW) {
    for (bool plain : {false, true}) {
        for (float height : {1.0f, 0.01f}) {
            GfxFixture fixture(256, 256, 1.0f, Prog0Options());
            Prog0Draw  c = Prog0(gfx::kProg0Unlit);
            Set(c.prog0.object_color, {255, 255, 255, 128});
            Set(c.prog0.fog, {-85, 255, 0, 340});
            c.mesh.flags = gfx::kMeshFog;
            Set(c.mesh.diffuse, {255.0f / 128.0f, 255.0f / 128.0f, 255.0f / 128.0f, 1});
            Set(c.mesh.fog, {-85, 340, 0, 255});
            std::memset(c.mesh.mvp, 0, sizeof c.mesh.mvp);
            c.mesh.mvp[0] = c.mesh.mvp[5] = c.mesh.mvp[11] = 1;
            c.mesh.mvp[14] = 0.1f;
            const std::vector<gfx::Vertex3D> vertices = {
                Vert({-3.2f, 0, 4}, {0, 0, 1}), Vert({3.2f, 0, 4}, {0, 0, 1}),
                Vert({0, height, 0}, {0, 0, 1})};
            gfx::DrawState       state;
            std::vector<uint8_t> previous;
            int                  max_step = 0;
            // C=(0,height,epsilon,0) crosses z=w=0 with nonzero y and unchanged side coverage.
            for (int step = -20; step <= 20; ++step) {
                const float epsilon = step * height * 0.00005f;
                c.mesh.mvp[6] = (epsilon - 0.1f) / height;
                // Independently verify the entire sampled rectangle is covered at every step.
                state.fog = false;
                if (plain) {
                    Render(fixture, vertices, kTriangle, c.mesh, {}, state);
                } else {
                    Render(fixture, vertices, kTriangle, c, {}, state);
                }
                for (int y = 176; y < 192; ++y) {
                    for (int x = 120; x < 136; ++x) {
                        ASSERT_TRUE(fixture.PixelNear(x, y, 255, 255, 255, 0));
                    }
                }
                state.fog = true;
                if (plain) {
                    Render(fixture, vertices, kTriangle, c.mesh, {}, state);
                } else {
                    Render(fixture, vertices, kTriangle, c, {}, state);
                }
                if (!previous.empty()) {
                    for (int y = 176; y < 192; ++y) {
                        for (int x = 120; x < 136; ++x) {
                            const int i = (y * fixture.width + x) * 4;
                            max_step = std::max(max_step, std::abs(int(fixture.pixels[i]) - int(previous[i])));
                        }
                    }
                }
                previous = fixture.pixels;
            }
            EXPECT_LE(max_step, 1) << "plain=" << plain << " height=" << height;
            RecordProperty(std::string(plain ? "plain_zero_zw_step_" : "prog0_zero_zw_step_") + std::to_string(height), max_step);
        }
    }
}

TEST(GfxGpuMeshDc2, PlainFogClampsVerticesBeforeScreenInterpolation) {
    GfxFixture         fixture(kWidth, kHeight);
    gfx::MeshConstants c{};
    c.flags = gfx::kMeshFog;
    Set(c.diffuse, {255.0f / 128.0f, 255.0f / 128.0f, 255.0f / 128.0f, 1});
    Set(c.fog, {-85, 340, 0, 255});
    c.mvp[0] = c.mvp[5] = c.mvp[11] = 1;
    c.mvp[14] = 0.1f;
    const std::vector<gfx::Vertex3D> vertices = {
        Vert({-0.5f, -0.5f, 0.5f}, {0, 0, 1}), Vert({0.5f, -0.5f, 0.5f}, {0, 0, 1}),
        Vert({0, 4, 4}, {0, 0, 1})};
    gfx::DrawState state;
    state.fog = true;
    Render(fixture, vertices, kTriangle, c, {}, state);
    for (int y : {8, 24, 40}) {
        const int fog = static_cast<int>(255.0f * (1.0f - (y + 0.5f) / kHeight) + 0.5f);
        EXPECT_TRUE(fixture.PixelNear(kWidth / 2, y, fog, fog, fog, 1)) << "row " << y;
    }
}

TEST(GfxGpuMeshDc2, FogFarClippingRetainsVertexInterpolation) {
    for (bool plain : {false, true}) {
        GfxFixture fixture(kWidth, kHeight, 1.0f, Prog0Options());
        Prog0Draw  c = Prog0(gfx::kProg0Unlit);
        Set(c.prog0.object_color, {255, 255, 255, 128});
        Set(c.prog0.fog, {-85, 255, 0, 340});
        c.mesh.flags = gfx::kMeshFog;
        Set(c.mesh.diffuse, {255.0f / 128.0f, 255.0f / 128.0f, 255.0f / 128.0f, 1});
        Set(c.mesh.fog, {-85, 340, 0, 255});
        std::memset(c.mesh.mvp, 0, sizeof c.mesh.mvp);
        c.mesh.mvp[0] = c.mesh.mvp[5] = c.mesh.mvp[11] = 1;
        c.mesh.mvp[10] = -0.1f;
        c.mesh.mvp[14] = 0.2f;
        const std::vector<gfx::Vertex3D> vertices = {
            Vert({-0.5f, -0.5f, 0.5f}, {0, 0, 1}), Vert({0.5f, -0.5f, 0.5f}, {0, 0, 1}),
            Vert({0, 4, 4}, {0, 0, 1})};
        gfx::DrawState state;
        state.fog = true;
        if (plain) {
            Render(fixture, vertices, kTriangle, c.mesh, {}, state);
        } else {
            Render(fixture, vertices, kTriangle, c, {}, state);
        }
        // The far source corner is clipped at w=2, but its finite original F remains valid.
        for (int y : {8, 24, 40}) {
            const int fog = static_cast<int>(255.0f * (1.0f - (y + 0.5f) / kHeight) + 0.5f);
            EXPECT_TRUE(fixture.PixelNear(kWidth / 2, y, fog, fog, fog, 1)) << "plain=" << plain << " row=" << y;
        }
    }
}

TEST(GfxGpuMeshDc2, FogBlendsClippingPerVertexInOneDraw) {
    GfxFixture fixture(kWidth, kHeight, 1.0f, Prog0Options());
    Prog0Draw  c = Prog0(gfx::kProg0Unlit);
    Set(c.prog0.object_color, {255, 255, 255, 128});
    SetFog(c.prog0.fog, 2, 4, 255, 0);
    std::memset(c.mesh.mvp, 0, sizeof c.mesh.mvp);
    c.mesh.mvp[0] = c.mesh.mvp[5] = c.mesh.mvp[11] = 1;
    c.mesh.mvp[14] = 0.1f;
    const std::vector<gfx::Vertex3D> vertices = {
        Vert({-0.5f, -0.5f, 0.5f}, {0, 0, 1}), Vert({0, -0.5f, 0.5f}, {0, 0, 1}),
        Vert({-2, 4, 4}, {0, 0, 1}), Vert({0, 1, -1}, {0, 0, 1}),
        Vert({10, 1, -1}, {0, 0, 1}), Vert({0, 1, 4}, {0, 0, 1})};
    gfx::DrawState state;
    state.fog = true;
    for (const auto &indices : {
             std::vector<uint32_t>{0, 1, 2, 3, 4, 5},
             std::vector<uint32_t>{2, 0, 1, 5, 3, 4}
    }) {
        Render(fixture, vertices, indices, c, {}, state);
        for (int y : {32, 36, 40}) {
            const int vertex_fog = static_cast<int>(255.0f * (1.0f - (y + 0.5f) / kHeight) + 0.5f);
            EXPECT_TRUE(fixture.PixelNear(kWidth / 4, y, vertex_fog, vertex_fog, vertex_fog, 1));
            const float inverse_w = 2.0f * (y + 0.5f) / kHeight - 1.0f;
            // The behind-eye corners have markers 1 and 0.11, with weights h-a and a.
            const float screen_x = 2.0f * (3 * kWidth / 4 + 0.5f) / kWidth - 1.0f;
            const float marker = (4.0f - 1.0f / inverse_w) / 5.0f - 0.89f * screen_x / (10.0f * inverse_w);
            const int   clipped_fog = static_cast<int>(marker * std::floor(std::clamp(-255.0f + 1020.0f * inverse_w, 0.0f, 255.0f)) + 0.5f);
            EXPECT_TRUE(fixture.PixelNear(3 * kWidth / 4, y, clipped_fog, clipped_fog, clipped_fog, 1));
        }
    }
}

TEST(GfxGpuMeshDc2, FogTruncatesEachVisibleVertexBeforeInterpolation) {
    GfxFixture fixture(kWidth, kHeight, 1.0f, Prog0Options());
    Prog0Draw  c = Prog0(gfx::kProg0Unlit);
    Set(c.prog0.object_color, {255, 255, 255, 128});
    Set(c.prog0.fog, {127.5666667f, 255, 0, 1.3333333f});
    std::memset(c.mesh.mvp, 0, sizeof c.mesh.mvp);
    c.mesh.mvp[0] = c.mesh.mvp[5] = c.mesh.mvp[11] = 1;
    c.mesh.mvp[14] = 0.1f;
    const std::vector<gfx::Vertex3D> vertices = {
        Vert({-1, -1, 1}, {0, 0, 1}), Vert({1, -1, 1}, {0, 0, 1}),
        Vert({0, 4, 4}, {0, 0, 1})};
    gfx::DrawState state;
    state.fog = true;
    Render(fixture, vertices, kTriangle, c, {}, state);
    // F=128.9 and 127.9 become 128 and 127 at the vertices, before the screen midpoint is blended.
    EXPECT_TRUE(fixture.PixelNear(kWidth / 2, kHeight / 2, 127, 127, 127, 0));
}

TEST(GfxGpuMeshDc2, FogTruncatesClampedFractionalVertexLimits) {
    GfxFixture fixture(kWidth, kHeight, 1.0f, Prog0Options());
    Prog0Draw  c = Prog0(gfx::kProg0Unlit);
    Set(c.prog0.object_color, {255, 255, 255, 128});
    gfx::DrawState state;
    state.fog = true;
    for (float scale : {0.0f, 340.0f}) {
        // Inverted limits collapse to the far limit before truncation, even for constant fog.
        Set(c.prog0.fog, {100.9f, 0.2f, 100.8f, scale});
        Render(fixture, ScreenVertices({0, 0, 1}), kTriangle, c, {}, state);
        EXPECT_TRUE(fixture.PixelNear(kWidth / 2, kHeight / 2, 100, 100, 100, 0));
    }
}

TEST(GfxGpuMeshDc2, PlainFogInvertedLimitsSaturateNearLast) {
    GfxFixture         fixture(kWidth, kHeight);
    gfx::MeshConstants c{};
    Identity(c.mvp);
    c.flags = gfx::kMeshFog;
    Set(c.diffuse, {255.0f / 128.0f, 255.0f / 128.0f, 255.0f / 128.0f, 1});
    gfx::DrawState state;
    state.fog = true;
    for (float scale : {0.0f, 340.0f}) {
        Set(c.fog, {100, scale, 255, 40});
        Render(fixture, ScreenVertices({0, 0, 1}), kTriangle, c, {}, state);
        EXPECT_TRUE(fixture.PixelNear(kWidth / 2, kHeight / 2, 40, 40, 40, 0));
    }
}

TEST(GfxGpuMeshDc2, FogCrossingTheEyeBlendsClampedFog) {
    GfxFixture fixture(kWidth, kHeight, 1.0f, Prog0Options());
    Prog0Draw  c = Prog0(gfx::kProg0Unlit);
    Set(c.prog0.object_color, {255, 255, 255, 128});
    SetFog(c.prog0.fog, 2, 4, 255, 0);
    std::memset(c.mesh.mvp, 0, sizeof c.mesh.mvp);
    c.mesh.mvp[0] = c.mesh.mvp[5] = c.mesh.mvp[11] = c.mesh.mvp[14] = 1;
    gfx::DrawState state;
    state.fog = true;
    for (float depth : {-1.0f, 0.0f, 0.5f}) {
        const std::vector<gfx::Vertex3D> vertices = {
            Vert({-10, 1, depth}, {0, 0, 1}), Vert({10, 1, depth}, {0, 0, 1}),
            Vert({0, 1, 4}, {0, 0, 1})};
        Render(fixture, vertices, kTriangle, c, {}, state);
        // On this plane 1/w is screen y, including corners behind or exactly on the eye plane.
        for (int y : {32, 36, 40, 44}) {
            const float inverse_w = 2.0f * (y + 0.5f) / kHeight - 1.0f;
            const float fresh = std::floor(std::clamp(-255.0f + 1020.0f * inverse_w, 0.0f, 255.0f));
            const int   fog = static_cast<int>(EyePlaneFog(depth, inverse_w, fresh) + 0.5f);
            EXPECT_TRUE(fixture.PixelNear(kWidth / 2, y, fog, fog, fog)) << "row " << y << " depth " << depth;
        }
    }
}

TEST(GfxGpuMeshDc2, PlainFogCrossesTheEyeWithoutDividingByZero) {
    GfxFixture         fixture(kWidth, kHeight);
    gfx::MeshConstants c{};
    c.flags = gfx::kMeshFog;
    c.mvp[0] = c.mvp[5] = c.mvp[11] = c.mvp[14] = 1;
    Set(c.diffuse, {255.0f / 128.0f, 255.0f / 128.0f, 255.0f / 128.0f, 1});
    Set(c.fog, {-255, 1020, 0, 255});
    gfx::DrawState state;
    state.fog = true;
    for (float depth : {-1.0f, 0.0f, 0.5f}) {
        const std::vector<gfx::Vertex3D> vertices = {
            Vert({-10, 1, depth}, {0, 0, 1}), Vert({10, 1, depth}, {0, 0, 1}),
            Vert({0, 1, 4}, {0, 0, 1})};
        Render(fixture, vertices, kTriangle, c, {}, state);
        for (int y : {32, 36, 40, 44}) {
            const float inverse_w = 2.0f * (y + 0.5f) / kHeight - 1.0f;
            const float fresh = std::clamp(-255.0f + 1020.0f * inverse_w, 0.0f, 255.0f);
            const int   fog = static_cast<int>(EyePlaneFog(depth, inverse_w, fresh) + 0.5f);
            EXPECT_TRUE(fixture.PixelNear(kWidth / 2, y, fog, fog, fog, 1)) << "row " << y << " depth " << depth;
        }
    }
}

TEST(GfxGpuMeshDc2, PlainFogInterpolatesInScreenSpace) {
    GfxFixture         fixture(kWidth, kHeight);
    gfx::MeshConstants c{};
    c.flags = gfx::kMeshFog;
    c.mvp[0] = c.mvp[5] = c.mvp[11] = c.mvp[14] = 1;
    Set(c.diffuse, {255.0f / 128.0f, 255.0f / 128.0f, 255.0f / 128.0f, 1});
    Set(c.fog, {-85, 340, 0, 255});
    const std::vector<gfx::Vertex3D> vertices = {
        Vert({-1, -1, 1}, {0, 0, 1}), Vert({1, -1, 1}, {0, 0, 1}),
        Vert({4, 4, 4}, {0, 0, 1}), Vert({-4, 4, 4}, {0, 0, 1})};
    gfx::DrawState state;
    state.fog = true;
    Render(fixture, vertices, {0, 1, 2, 0, 2, 3}, c, {}, state);
    for (int y : {8, 24, 40}) {
        const int fog = static_cast<int>(255.0f * (1.0f - (y + 0.5f) / kHeight) + 0.5f);
        EXPECT_TRUE(fixture.PixelNear(kWidth / 2, y, fog, fog, fog)) << "row " << y;
    }
}

TEST(GfxGpuMeshDc2, FourLightFogWithWhiteVertexModulationMatchesMaterialOnly) {
    GfxFixture         fixture(kWidth, kHeight);
    gfx::MeshConstants constants = ChronicleLit();
    constants.flags |= gfx::kMeshFog;
    constants.light_count = 4;
    Set(constants.fog, {100, 50, 0, 255});
    for (uint32_t i = 0; i < 4; i++) {
        Set(constants.light_direction[i], {0, 0, -1, 0});
        const float scale = static_cast<float>(i + 1);
        Set(constants.light_color[i], {0.07f * scale, 0.03f * scale, 0.02f * scale, 0});
    }
    auto           vertices = ScreenVertices({0, 0, 2}, {128, 128, 128, 128});
    const auto     texture = gfx::CreateTexture({1, 1, gfx::TextureFormat::Rgba8, 1, true});
    const uint32_t white = Rgba(255, 255, 255, 255);
    ASSERT_TRUE(gfx::UpdateTexture(texture, 0, 0, 0, 1, 1, &white));
    gfx::TextureBinding binding;
    binding.texture = texture;
    gfx::DrawState state;
    state.blend = true;
    state.alpha = {0, 1, 0, 1, 0};
    state.alpha_test = true;
    state.alpha_func = gfx::AlphaFunc::GEqual;
    state.alpha_ref = 64;
    state.fog = true;
    state.fog_color[0] = 30;
    state.fog_color[1] = 60;
    state.fog_color[2] = 90;
    auto draw = [&] {
        gfx::DrawMeshImmediate(vertices, kTriangle, constants, binding, state);
    };
    fixture.Frame(kClear, draw);
    const auto material_only = fixture.pixels;
    constants.flags |= gfx::kMeshVertexColor;
    fixture.Frame(kClear, draw);
    EXPECT_EQ(fixture.pixels, material_only);
}

TEST(GfxGpuMeshDc2, ConstantFullFogFactorMatchesUnfoggedPixels) {
    GfxFixture fixture(kWidth, kHeight, 1.0f, Prog0Options());
    Prog0Draw  c = Prog0(gfx::kProg0Unlit);
    Set(c.prog0.object_color, {230, 170, 90, 128});
    std::memset(c.mesh.mvp, 0, sizeof c.mesh.mvp);
    c.mesh.mvp[0] = c.mesh.mvp[5] = c.mesh.mvp[11] = c.mesh.mvp[14] = 1;
    const std::vector<gfx::Vertex3D> vertices = {
        Vert({-1, -1, 1}, {0, 0, 1}), Vert({1, -1, 1}, {0, 0, 1}),
        Vert({4, 4, 4}, {0, 0, 1}), Vert({-4, 4, 4}, {0, 0, 1})};
    const std::vector<uint32_t> indices = {0, 1, 2, 0, 2, 3};
    gfx::DrawState              state;
    Render(fixture, vertices, indices, c, {}, state);
    const auto unfogged = fixture.pixels;
    state.fog = true;
    Render(fixture, vertices, indices, c, {}, state);
    EXPECT_EQ(fixture.pixels, unfogged);
}

namespace {

constexpr float kHugeUv = 4096.0f; // minifies a 4x4 texture to its last mip

// KZ_ALPHA_OPAQUE is read once by the renderer, so a case never changes it. The renderings below must
// match with it on and off; only the knowledge asserted through ExpectKnownMips needs it on.
bool AlphaOpaqueOn() {
    const char *setting = std::getenv("KZ_ALPHA_OPAQUE");
    return setting != nullptr && std::strcmp(setting, "1") == 0;
}

void ExpectKnownMips(gfx::TextureHandle texture, uint64_t mips) {
    if (AlphaOpaqueOn()) {
        EXPECT_EQ(gfx::detail::LookupTexture(texture)->opaque_alpha_mips, mips);
    }
}

std::vector<uint32_t> RedTexels(size_t count, uint8_t alpha) {
    return std::vector<uint32_t>(count, Rgba(255, 0, 0, alpha));
}

gfx::MeshConstants PlainMaterial(float alpha = 1.0f) {
    gfx::MeshConstants c = ChronicleLit();
    c.flags = 0;
    c.diffuse[3] = alpha;
    return c;
}

// An alpha-tested plain mesh at depth 0.5, then a blue triangle behind it at 0.25. Red: the texture drew
// and wrote depth. Blue: it was discarded without writing colour or depth.
void AlphaTestedOverBlue(const gfx::TextureBinding &binding, const gfx::MeshConstants &constants,
                         float uv_span = 0.0f, uint8_t alpha_ref = 1, std::array<uint8_t, 4> vertex_color = {},
                         uint8_t color_write_mask = gfx::kWriteRgba) {
    auto front = ScreenVertices({0, 0, 1}, vertex_color);
    front[1].uv[0] = uv_span;
    front[2].uv[1] = uv_span;
    gfx::DrawState state;
    state.depth_test = gfx::DepthTest::GEqual;
    state.depth_write = true;
    state.alpha_test = true;
    state.alpha_func = gfx::AlphaFunc::GEqual;
    state.alpha_ref = alpha_ref;
    state.color_write_mask = color_write_mask;
    gfx::DrawMeshImmediate(front, kTriangle, constants, binding, state);

    gfx::MeshConstants blue = PlainMaterial();
    Set(blue.diffuse, {0, 0, 255.0f / 128.0f, 1});
    auto back = ScreenVertices({0, 0, 1});
    for (gfx::Vertex3D &vertex : back) {
        vertex.position[2] = 0.25f;
    }
    gfx::DrawState behind;
    behind.depth_test = gfx::DepthTest::GEqual;
    behind.depth_write = true;
    gfx::DrawMeshImmediate(back, kTriangle, blue, {}, behind);
}

bool Shows(const GfxFixture &fixture, bool red) {
    const int r = red ? 255 : 0;
    const int b = red ? 0 : 255;
    return fixture.PixelNear(2, 2, r, 0, b) && Centre(fixture, r, 0, b) &&
           fixture.PixelNear(fixture.width - 3, fixture.height - 3, r, 0, b);
}

} // namespace

TEST(GfxGpuMeshDc2, OpaqueAlphaTexturesDrawAtEveryMipAndWriteDepth) {
    GfxFixture         fixture(kWidth, kHeight);
    gfx::TextureHandle texture = gfx::CreateTexture({4, 4, gfx::TextureFormat::Rgba8, 3, true});
    for (uint32_t mip = 0; mip < 3; mip++) {
        const uint32_t size = 4u >> mip;
        const auto     texels = RedTexels(size * size, 255);
        ASSERT_TRUE(gfx::UpdateTexture(texture, mip, 0, 0, size, size, texels.data()));
    }
    ExpectKnownMips(texture, 7);
    for (float span : {0.0f, kHugeUv}) {
        fixture.Frame(kClear, [&] { AlphaTestedOverBlue(NearestBinding(texture), PlainMaterial(), span); });
        EXPECT_TRUE(Shows(fixture, true)) << span;
    }

    // Padding between rows, here with zero alpha, is not part of the upload.
    gfx::TextureHandle    padded_texture = gfx::CreateTexture({4, 4, gfx::TextureFormat::Rgba8, 1, true});
    std::vector<uint32_t> padded(6 * 4, Rgba(255, 0, 0, 0));
    for (uint32_t y = 0; y < 4; y++) {
        std::fill_n(padded.begin() + y * 6, 4, Rgba(255, 0, 0, 255));
    }
    ASSERT_TRUE(gfx::UpdateTexture(padded_texture, 0, 0, 0, 4, 4, padded.data(), 6));
    ExpectKnownMips(padded_texture, 1);
    fixture.Frame(kClear, [&] { AlphaTestedOverBlue(NearestBinding(padded_texture), PlainMaterial()); });
    EXPECT_TRUE(Shows(fixture, true));
    fixture.Frame(kClear, [&] {
        AlphaTestedOverBlue(NearestBinding(padded_texture), PlainMaterial(), 0.0f, 1, {}, 0);
    });
    EXPECT_TRUE(Centre(fixture, 0, 0, 0));
    padded[0] = Rgba(255, 0, 0, 0);
    ASSERT_TRUE(gfx::UpdateTexture(padded_texture, 0, 0, 0, 4, 4, padded.data(), 6));
    ExpectKnownMips(padded_texture, 0);
    fixture.Frame(kClear, [&] { AlphaTestedOverBlue(NearestBinding(padded_texture), PlainMaterial()); });
    EXPECT_TRUE(Shows(fixture, false));
    fixture.Frame(kClear, [&] {
        AlphaTestedOverBlue(NearestBinding(padded_texture), PlainMaterial(), 0.0f, 1, {}, 0);
    });
    EXPECT_TRUE(Shows(fixture, false));
}

TEST(GfxGpuMeshDc2, CpuUpdatesToTransparentAlphaStopTheOpaqueShortcut) {
    GfxFixture         fixture(kWidth, kHeight);
    gfx::TextureHandle texture = gfx::CreateTexture({4, 4, gfx::TextureFormat::Rgba8, 1, true});
    const auto         opaque = RedTexels(16, 255);
    const auto         clear = RedTexels(16, 0);
    const auto         inner = RedTexels(4, 255);
    const uint32_t     hole = Rgba(255, 0, 0, 0);
    const uint32_t     solid = Rgba(255, 0, 0, 255);
    auto               expect = [&](bool red, uint64_t mips, const char *step) {
        SCOPED_TRACE(step);
        ExpectKnownMips(texture, mips);
        fixture.Frame(kClear, [&] { AlphaTestedOverBlue(NearestBinding(texture), PlainMaterial()); });
        EXPECT_TRUE(Shows(fixture, red));
    };
    ASSERT_TRUE(gfx::UpdateTexture(texture, 0, 0, 0, 4, 4, opaque.data()));
    expect(true, 1, "full opaque");
    // A partial opaque write keeps what is known, and proves nothing by itself.
    ASSERT_TRUE(gfx::UpdateTexture(texture, 0, 1, 1, 2, 2, inner.data()));
    expect(true, 1, "partial opaque over known");
    ASSERT_TRUE(gfx::UpdateTexture(texture, 0, 0, 0, 1, 1, &hole));
    expect(false, 0, "partial transparent");
    ASSERT_TRUE(gfx::UpdateTexture(texture, 0, 0, 0, 1, 1, &solid));
    expect(true, 0, "partial opaque over unknown");
    ASSERT_TRUE(gfx::UpdateTexture(texture, 0, 0, 0, 4, 4, opaque.data()));
    expect(true, 1, "full opaque again");
    ASSERT_TRUE(gfx::UpdateTexture(texture, 0, 0, 0, 4, 4, clear.data()));
    expect(false, 0, "full transparent replacement");
}

TEST(GfxGpuMeshDc2, OpaqueAlphaShortcutNeedsEveryMipKnown) {
    GfxFixture         fixture(kWidth, kHeight);
    gfx::TextureHandle texture = gfx::CreateTexture({4, 4, gfx::TextureFormat::Rgba8, 3, true});
    const auto         opaque = RedTexels(16, 255);
    const uint32_t     hole = Rgba(255, 0, 0, 0);
    const uint32_t     solid = Rgba(255, 0, 0, 255);
    ASSERT_TRUE(gfx::UpdateTexture(texture, 0, 0, 0, 4, 4, opaque.data()));
    ASSERT_TRUE(gfx::UpdateTexture(texture, 1, 0, 0, 2, 2, opaque.data()));
    ASSERT_TRUE(gfx::UpdateTexture(texture, 2, 0, 0, 1, 1, &hole));
    auto expect = [&](float span, bool red, uint64_t mips) {
        SCOPED_TRACE(span);
        ExpectKnownMips(texture, mips);
        fixture.Frame(kClear, [&] { AlphaTestedOverBlue(NearestBinding(texture), PlainMaterial(), span); });
        EXPECT_TRUE(Shows(fixture, red));
    };
    expect(0.0f, true, 3);
    expect(kHugeUv, false, 3); // the transparent last mip
    ASSERT_TRUE(gfx::UpdateTexture(texture, 2, 0, 0, 1, 1, &solid));
    expect(kHugeUv, true, 7);
    ASSERT_TRUE(gfx::UpdateTexture(texture, 0, 0, 0, 1, 1, &hole));
    expect(0.0f, false, 6);
    expect(kHugeUv, true, 6);

    // A mip that was never uploaded is not known, whatever the others hold.
    gfx::TextureHandle partial = gfx::CreateTexture({4, 4, gfx::TextureFormat::Rgba8, 3, true});
    ASSERT_TRUE(gfx::UpdateTexture(partial, 0, 0, 0, 4, 4, opaque.data()));
    ASSERT_TRUE(gfx::UpdateTexture(partial, 1, 0, 0, 2, 2, opaque.data()));
    ExpectKnownMips(partial, 3);
    fixture.Frame(kClear, [&] { AlphaTestedOverBlue(NearestBinding(partial), PlainMaterial()); });
    EXPECT_TRUE(Shows(fixture, true));
}

TEST(GfxGpuMeshDc2, CopyBlitAndSnapshotOverOpaqueAlphaTexturesKeepTheAlphaTest) {
    GfxFixture         fixture(kWidth, kHeight);
    gfx::TextureHandle source = gfx::CreateTexture({4, 4, gfx::TextureFormat::Rgba8, 1, true});
    gfx::TextureHandle target = gfx::CreateTexture({4, 4, gfx::TextureFormat::Rgba8, 1, true});
    const auto         clear = RedTexels(16, 0);
    const auto         opaque = RedTexels(16, 255);
    ASSERT_TRUE(gfx::UpdateTexture(source, 0, 0, 0, 4, 4, clear.data()));
    const std::array<uint8_t, 4> transparent_clear = {0, 0, 0, 0};
    const gfx::Rect              all = {0, 0, 4, 4};

    struct Overwrite {
        const char            *name;
        std::array<uint8_t, 4> clear;
        std::function<void()>  write;
    };

    const Overwrite overwrites[] = {
        {"copy",     kClear,            [&] { EXPECT_TRUE(gfx::CopyTexture(source, all, target, 0, 0)); }                     },
        {"blit",     kClear,            [&] { EXPECT_TRUE(gfx::BlitTexture(source, all, target, all, gfx::Filter::Nearest)); }},
        {"snapshot", transparent_clear, [&] { EXPECT_TRUE(gfx::SnapshotFrame(target)); }                                      },
    };
    for (const Overwrite &overwrite : overwrites) {
        SCOPED_TRACE(overwrite.name);
        ASSERT_TRUE(gfx::UpdateTexture(target, 0, 0, 0, 4, 4, opaque.data()));
        ExpectKnownMips(target, 1);
        fixture.Frame(kClear, [&] { AlphaTestedOverBlue(NearestBinding(target), PlainMaterial()); });
        EXPECT_TRUE(Shows(fixture, true));
        fixture.Frame(overwrite.clear, [&] {
            overwrite.write();
            AlphaTestedOverBlue(NearestBinding(target), PlainMaterial());
        });
        EXPECT_TRUE(Shows(fixture, false));
        ExpectKnownMips(target, 0);
    }
}

TEST(GfxGpuMeshDc2, RecordedTextureUpdatesReplayOverOpaqueAlphaKnowledge) {
    GfxFixture         fixture(kWidth, kHeight);
    gfx::TextureHandle texture = gfx::CreateTexture({4, 4, gfx::TextureFormat::Rgba8, 1, true});
    const auto         opaque = RedTexels(16, 255);
    const auto         clear = RedTexels(16, 0);
    ASSERT_TRUE(gfx::UpdateTextureNow(texture, 0, 0, 0, 4, 4, opaque.data()));
    auto record = [&](auto &&before_draw) {
        gfx::BeginRecording();
        gfx::Clear(true, kClear.data(), true, 0.0f);
        before_draw();
        AlphaTestedOverBlue(NearestBinding(texture), PlainMaterial());
        return gfx::EndRecording();
    };
    auto render = [&](const gfx::DisplayListRef &list) {
        gfx::RenderOptions options;
        options.canonical = true;
        ASSERT_TRUE(gfx::RenderList(*list, 1, options));
        ASSERT_TRUE(gfx::ReadbackFrame(fixture.pixels, fixture.width, fixture.height));
    };
    render(record([] {}));
    EXPECT_TRUE(Shows(fixture, true));

    // The recorded write holds its own copy of the pixels and lands at replay, before the recorded draw.
    std::vector<uint32_t> pixels = clear;
    auto                  list = record([&] {
        ASSERT_TRUE(gfx::UpdateTexture(texture, 0, 0, 0, 4, 4, pixels.data()));
        ExpectKnownMips(texture, 1);
        std::fill(pixels.begin(), pixels.end(), Rgba(255, 0, 0, 255));
    });
    render(list);
    EXPECT_TRUE(Shows(fixture, false));
    ExpectKnownMips(texture, 0);

    // UpdateTextureNow is not recorded: it lands at once, and the list only draws.
    ASSERT_TRUE(gfx::UpdateTextureNow(texture, 0, 0, 0, 4, 4, opaque.data()));
    ExpectKnownMips(texture, 1);
    render(record([] {}));
    EXPECT_TRUE(Shows(fixture, true));
    list = record([&] {
        ASSERT_TRUE(gfx::UpdateTextureNow(texture, 0, 0, 0, 4, 4, clear.data()));
        ExpectKnownMips(texture, 0);
    });
    render(list);
    EXPECT_TRUE(Shows(fixture, false));
}

TEST(GfxGpuMeshDc2, OpaqueAlphaShortcutKeepsTheTestWhereAlphaCanFail) {
    GfxFixture         fixture(kWidth, kHeight);
    gfx::TextureHandle texture = gfx::CreateTexture({4, 4, gfx::TextureFormat::Rgba8, 1, true});
    const auto         opaque = RedTexels(16, 255);
    ASSERT_TRUE(gfx::UpdateTexture(texture, 0, 0, 0, 4, 4, opaque.data()));
    ExpectKnownMips(texture, 1);

    struct Case {
        const char *name;
        float       diffuse_alpha;
        uint8_t     alpha_ref;
        bool        vertex_color;
        uint8_t     vertex_alpha;
        bool        red;
    };

    const Case cases[] = {
        {"diffuse 0",                       0.0f,            1,   false, 0, false},
        {"diffuse 1/1024",                  1.0f / 1024.0f,  1,   false, 0, false},
        {"diffuse 1/128 reaches ref 1",     1.0f / 128.0f,   1,   false, 0, true },
        {"diffuse 1",                       1.0f,            1,   false, 0, true },
        {"diffuse 0.25 under ref 64",       0.25f,           64,  false, 0, false},
        {"diffuse 0.5 reaches ref 64",      0.5f,            64,  false, 0, true },
        {"diffuse 1 over ref 64",           1.0f,            64,  false, 0, true },
        {"diffuse 127/128 reaches ref 127", 127.0f / 128.0f, 127, false, 0, true },
        {"diffuse 1 reaches ref 128",       1.0f,            128, false, 0, true },
        {"diffuse 1 under ref 129",         1.0f,            129, false, 0, false},
        {"vertex colour alpha 0",           1.0f,            1,   true,  0, false},
    };
    for (const Case &c : cases) {
        SCOPED_TRACE(c.name);
        gfx::MeshConstants constants = PlainMaterial(c.diffuse_alpha);
        if (c.vertex_color) {
            constants.flags |= gfx::kMeshVertexColor;
        }
        fixture.Frame(kClear, [&] {
            AlphaTestedOverBlue(NearestBinding(texture), constants, 0.0f, c.alpha_ref,
                                {255, 255, 255, c.vertex_alpha});
        });
        EXPECT_TRUE(Shows(fixture, c.red));
    }
}

TEST(GfxGpuMeshDc2, IndexedNearSafeFogMatchesImmediateReconstruction) {
    GfxFixture fixture(kWidth, kHeight);
    for (bool cutout : {false, true}) {
        gfx::TextureHandle texture = gfx::CreateTexture({4, 4, gfx::TextureFormat::Rgba8, 1, true});
        auto               texels = RedTexels(16, 255);
        if (cutout) {
            texels[0] = Rgba(255, 0, 0, 0);
        }
        ASSERT_TRUE(gfx::UpdateTexture(texture, 0, 0, 0, 4, 4, texels.data()));
        auto constants = PlainMaterial();
        constants.flags = gfx::kMeshFog;
        Set(constants.fog, {180, -60, 0, 255});
        std::memset(constants.mvp, 0, sizeof constants.mvp);
        constants.mvp[0] = constants.mvp[5] = constants.mvp[11] = 1.0f;
        constants.mvp[10] = 0.5f;
        gfx::DrawState state;
        state.blend = true;
        state.alpha = {0, 1, 0, 1, 0};
        state.alpha_test = true;
        state.alpha_func = gfx::AlphaFunc::GEqual;
        state.alpha_ref = 64;
        state.fog = true;
        state.fog_color[0] = 20;
        state.fog_color[1] = 40;
        state.fog_color[2] = 70;
        const auto binding = NearestBinding(texture);
        for (int crossing = 0; crossing < 3; crossing++) {
            SCOPED_TRACE(crossing);
            auto vertices = ScreenVertices({0, 0, 1});
            vertices[0].position[2] = crossing == 2 ? -1.0f : 2.0f;
            vertices[1].position[2] = 4.0f;
            vertices[2].position[2] = 6.0f;
            vertices[1].uv[0] = vertices[2].uv[1] = 4.0f;
            constants.mvp[14] = crossing == 1 ? 2.0f : 0.0f;
            const auto mesh = gfx::CreateMesh(vertices, kTriangle);
            ASSERT_NE(mesh, gfx::kNullMesh);
            fixture.Frame(kClear, [&] { gfx::DrawMeshImmediate(vertices, kTriangle, constants, binding, state); });
            const auto reference = fixture.pixels;
            fixture.Frame(kClear, [&] {
                gfx::DrawMesh(mesh, 0, 3, constants, binding, state);
                if (gfx::detail::NearSafeFogEnabled()) {
                    const bool safe = gfx::detail::MeshFogIsNearSafe(*gfx::detail::LookupMesh(mesh), constants);
                    EXPECT_EQ(safe, crossing == 0);
                    const uint32_t variant = !cutout && AlphaOpaqueOn() ? 0 : 1;
                    VkPipeline     expected = gfx::detail::g.near_safe_fog_pipelines[variant];
                    EXPECT_EQ(gfx::detail::g.bound.pipeline == expected, safe);
                }
            });
            EXPECT_EQ(fixture.pixels, reference);
            if (crossing == 0) {
                vertices[1].position[0] += 0.25f;
                ASSERT_TRUE(gfx::UpdateMeshVertices(mesh, 0, vertices));
                fixture.Frame(kClear, [&] { gfx::DrawMeshImmediate(vertices, kTriangle, constants, binding, state); });
                const auto updated = fixture.pixels;
                fixture.Frame(kClear, [&] {
                    gfx::DrawMesh(mesh, 0, 3, constants, binding, state);
                    EXPECT_FALSE(gfx::detail::LookupMesh(mesh)->bounds_known);
                });
                EXPECT_EQ(fixture.pixels, updated);
            }
            gfx::DestroyMesh(mesh);
        }
        gfx::DestroyTexture(texture);
    }
}

TEST(GfxMeshDc2, Prog0NearSafeFogRejectsUnsafeAndNonfiniteInputs) {
    gfx::detail::Mesh mesh;
    mesh.bounds_known = true;
    mesh.bounds_normal_inputs = true;
    mesh.bounds_min = {-1, -1, 2};
    mesh.bounds_max = {3, 3, 6};
    auto draw = Unlit(gfx::kProg0Unlit);
    SetWMvp(draw.mesh.mvp, 1);
    Set(draw.prog0.fog, {180, 255, 0, -60});
    const auto safe = [&] { return gfx::detail::Prog0FogIsNearSafe(mesh, draw.mesh, draw.prog0); };
    ASSERT_TRUE(safe());

    mesh.bounds_known = false;
    EXPECT_FALSE(safe());
    mesh.bounds_known = true;
    mesh.bounds_normal_inputs = false;
    EXPECT_FALSE(safe());
    mesh.bounds_normal_inputs = true;
    draw.prog0.flags |= gfx::kProg0Skinned;
    EXPECT_FALSE(safe());
    draw.prog0.flags &= ~gfx::kProg0Skinned;
    draw.prog0.fog[3] = 0;
    EXPECT_FALSE(safe());
    draw.prog0.fog[3] = -60;

    for (float value : {std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN(),
                        std::numeric_limits<float>::max()}) {
        SCOPED_TRACE(value);
        draw.prog0.fog[0] = value;
        EXPECT_FALSE(safe());
        draw.prog0.fog[0] = 180;
        draw.mesh.mvp[15] = value;
        EXPECT_FALSE(safe());
        draw.mesh.mvp[15] = 1;
    }
    draw.mesh.mvp[14] = 1;
    EXPECT_FALSE(safe());
    draw.mesh.mvp[14] = std::nextafter(1.0f, 0.0f);
    EXPECT_FALSE(safe());
    draw.mesh.mvp[14] = 1.0f - .001f;
    EXPECT_TRUE(safe());
    draw.mesh.mvp[14] = 1.001f;
    EXPECT_FALSE(safe());
    SetWMvp(draw.mesh.mvp, std::ldexp(1.0f, -22));
    EXPECT_FALSE(safe());
    SetWMvp(draw.mesh.mvp, std::ldexp(1.0f, -127));
    EXPECT_FALSE(safe());
    for (bool tiny_position : {false, true}) {
        SCOPED_TRACE(tiny_position);
        const float x = std::ldexp(1.0f, tiny_position ? -127 : 125);
        mesh.bounds_min = {x, 0, 0};
        mesh.bounds_max = mesh.bounds_min;
        mesh.bounds_normal_inputs = !tiny_position;
        std::memset(draw.mesh.mvp, 0, sizeof draw.mesh.mvp);
        draw.mesh.mvp[3] = std::ldexp(1.0f, tiny_position ? 125 : -127);
        draw.mesh.mvp[15] = .1f;
        draw.mesh.mvp[14] = .2f;
        // Flushing either multiplicand changes W from .35 to .1, crossing the near plane.
        EXPECT_FALSE(safe());
    }
}

TEST(GfxGpuMeshDc2, Prog0NearSafeFogPreservesFullRgbaAgainstUpdatedMeshReference) {
    if (!gfx::detail::NearSafeFogEnabled()) {
        GTEST_SKIP() << "Requires KZ_FOG_NEAR_SAFE=1 before process startup";
    }
    GfxFixture                   fixture(kWidth, kHeight, 1.0f, Prog0Options());
    const std::array<uint8_t, 4> clear = {17, 29, 43, 71};
    const auto                   texture = gfx::CreateTexture({4, 4, gfx::TextureFormat::Rgba8, 1, true});
    ASSERT_NE(texture, gfx::kNullTexture);
    std::array<uint32_t, 16> texels;
    for (size_t i = 0; i < texels.size(); ++i) {
        texels[i] = Rgba(211, 83, 147, std::array<uint8_t, 4>{0, 63, 127, 255}[i % 4]);
    }
    ASSERT_TRUE(gfx::UpdateTexture(texture, 0, 0, 0, 4, 4, texels.data()));
    const auto binding = NearestBinding(texture);
    /**
     * Selects a clip configuration independently of fog parameters.
     */
    enum class Projection {
        Perspective,
        FarClip,
        NearClip,
        EyeCrossing,
        UnitW,
        NearBoundary,
        SmallW,
        SubnormalW
    };

    /**
     * Defines expected immutable geometry eligibility and whether visible fragments are required.
     */
    struct Case {
        const char          *name;           /**< Diagnostic description. */
        Projection           projection;     /**< Actual uploaded clip matrix. */
        std::array<float, 4> fog;            /**< Prog0 fog coefficients. */
        bool                 safe;           /**< Whether the immutable proof should succeed. */
        bool                 visible = true; /**< Whether the projection must produce visible RGB. */
    };

    const std::array cases{
        Case{"perspective", Projection::Perspective, {180, 255, 0, -60}, true},
        Case{"far crossing retains vertex fog", Projection::FarClip, {180, 255, 0, -60}, true},
        Case{"visible near crossing", Projection::NearClip, {180, 255, 0, -60}, false},
        Case{"eye crossing", Projection::EyeCrossing, {180, 255, 0, -60}, false},
        Case{"constant fog layout", Projection::UnitW, {100.9f, 100.8f, 100.7f, 0}, false},
        Case{"below integer", Projection::UnitW, {std::nextafter(128.0f, 0.0f), 255, 0, -1}, true},
        Case{"at integer", Projection::UnitW, {128, 255, 0, -1}, true},
        Case{"above integer", Projection::UnitW, {std::nextafter(128.0f, 256.0f), 255, 0, -1}, true},
        Case{"upper clamp", Projection::UnitW, {400, 155.75f, 20.25f, -1}, true},
        Case{"lower clamp", Projection::UnitW, {-100, 155.75f, 20.25f, -1}, true},
        Case{"near equality", Projection::NearBoundary, {180, 255, 0, -60}, false},
        Case{"small positive W", Projection::SmallW, {128, 255, 0, -64 * std::ldexp(1.0f, -22)}, false},
        Case{"subnormal positive W", Projection::SubnormalW, {128, 255, 0, -64 * std::ldexp(1.0f, -127)}, false, false}
    };
    for (bool lit : {false, true}) {
        for (bool blend : {false, true}) {
            for (bool alpha_test : {false, true}) {
                for (const auto &test : cases) {
                    SCOPED_TRACE(test.name);
                    SCOPED_TRACE(lit ? "lit" : "unlit");
                    SCOPED_TRACE(blend ? "b4 source-over" : "b0 overwrite");
                    SCOPED_TRACE(alpha_test ? "GEqual cutout" : "no alpha test");
                    auto draw = lit ? FourLights() : Unlit(gfx::kProg0Unlit);
                    std::copy(test.fog.begin(), test.fog.end(), draw.prog0.fog);
                    std::memset(draw.mesh.mvp, 0, sizeof draw.mesh.mvp);
                    draw.mesh.mvp[0] = draw.mesh.mvp[5] = draw.mesh.mvp[11] = 1;
                    draw.mesh.mvp[10] = .5f;
                    auto vertices = ScreenVertices({0, 0, 1}, {123, 211, 157, 113});
                    vertices[0].position[2] = test.projection == Projection::EyeCrossing ? -.5f : 2;
                    vertices[1].position[2] = 4;
                    vertices[2].position[2] = 6;
                    vertices[1].uv[0] = vertices[2].uv[1] = 4;
                    switch (test.projection) {
                        case Projection::FarClip:
                            draw.mesh.mvp[14] = -1.5f;
                            break;
                        case Projection::NearClip:
                            draw.mesh.mvp[14] = 1.5f;
                            break;
                        case Projection::UnitW:
                        case Projection::NearBoundary:
                            SetWMvp(draw.mesh.mvp, 1);
                            if (test.projection == Projection::NearBoundary) {
                                draw.mesh.mvp[14] = 1;
                            }
                            break;
                        case Projection::SmallW:
                        case Projection::SubnormalW:
                            SetWMvp(draw.mesh.mvp, std::ldexp(1.0f, test.projection == Projection::SmallW ? -22 : -127));
                            break;
                        default:
                            break;
                    }
                    gfx::DrawState state;
                    state.blend = blend;
                    state.alpha = {0, 1, 0, 1, 0};
                    state.alpha_test = alpha_test;
                    state.alpha_func = gfx::AlphaFunc::GEqual;
                    state.alpha_ref = 1;
                    state.fog = true;
                    state.fog_color[0] = 20;
                    state.fog_color[1] = 40;
                    state.fog_color[2] = 70;
                    const auto mesh = gfx::CreateMesh(vertices, kTriangle);
                    const auto reference_mesh = gfx::CreateMesh(vertices, kTriangle);
                    ASSERT_NE(mesh, gfx::kNullMesh);
                    ASSERT_NE(reference_mesh, gfx::kNullMesh);
                    ASSERT_TRUE(gfx::UpdateMeshVertices(reference_mesh, 0, vertices));
                    ASSERT_FALSE(gfx::detail::LookupMesh(reference_mesh)->bounds_known);
                    ASSERT_FALSE(gfx::detail::LookupMesh(reference_mesh)->bounds_normal_inputs);
                    EXPECT_EQ(gfx::detail::Prog0FogIsNearSafe(*gfx::detail::LookupMesh(mesh), draw.mesh, draw.prog0), test.safe);
                    const auto render = [&](gfx::MeshHandle handle, bool specialized) {
                        fixture.Frame(clear, [&] {
                            const auto before = gfx::detail::g.counters.near_safe_prog0_fog;
                            gfx::DrawMeshProg0(handle, 0, 3, draw.mesh, draw.prog0, binding, state);
                            EXPECT_EQ(gfx::detail::g.counters.near_safe_prog0_fog - before, specialized ? 1u : 0u);
                        });
                    };
                    render(reference_mesh, false);
                    const auto reference = fixture.pixels;
                    const bool supported_pipeline = (!blend && alpha_test) || (blend && !alpha_test);
                    render(mesh, test.safe && supported_pipeline);
                    EXPECT_EQ(fixture.pixels, reference);
                    if (test.visible) {
                        size_t changed = 0;
                        for (size_t i = 0; i + 3 < reference.size(); i += 4) {
                            changed += reference[i] != clear[0] || reference[i + 1] != clear[1] || reference[i + 2] != clear[2] ? 1u : 0u;
                        }
                        EXPECT_GT(changed, 0u);
                    }
                    if (test.projection == Projection::Perspective) {
                        ASSERT_TRUE(gfx::UpdateMeshVertices(mesh, 0, vertices));
                        EXPECT_FALSE(gfx::detail::Prog0FogIsNearSafe(*gfx::detail::LookupMesh(mesh), draw.mesh, draw.prog0));
                        render(mesh, false);
                        EXPECT_EQ(fixture.pixels, reference);
                    }
                    gfx::DestroyMesh(reference_mesh);
                    gfx::DestroyMesh(mesh);
                }
            }
        }
    }
    auto skin_draw = Unlit(gfx::kProg0Unlit | gfx::kProg0Skinned);
    SetWMvp(skin_draw.mesh.mvp, 1);
    Set(skin_draw.prog0.fog, {180, 255, 0, -60});
    for (uint32_t bone = 0; bone < gfx::kProg0Bones; ++bone) {
        SetBone(skin_draw.palette, bone, kIdentityBone);
    }
    const auto skin_vertices = Skinned(ScreenVertices({0, 0, 1}), {1, 0, 0, 0}, {0, 0, 0, 0});
    const auto skin_mesh = gfx::CreateMesh(skin_vertices, kTriangle);
    const auto skin_reference = gfx::CreateMesh(skin_vertices, kTriangle);
    ASSERT_NE(skin_mesh, gfx::kNullMesh);
    ASSERT_NE(skin_reference, gfx::kNullMesh);
    ASSERT_TRUE(gfx::UpdateMeshVertices(skin_reference, 0, skin_vertices));
    EXPECT_FALSE(gfx::detail::Prog0FogIsNearSafe(*gfx::detail::LookupMesh(skin_mesh), skin_draw.mesh, skin_draw.prog0));
    gfx::DrawState skin_state;
    skin_state.fog = true;
    skin_state.alpha_test = true;
    skin_state.alpha_func = gfx::AlphaFunc::GEqual;
    skin_state.alpha_ref = 1;
    const auto render_skin = [&](gfx::MeshHandle handle) {
        fixture.Frame(clear, [&] {
            gfx::DrawMeshProg0(handle, 0, 3, skin_draw.mesh, skin_draw.prog0, binding, skin_state, nullptr, &skin_draw.palette);
            EXPECT_EQ(gfx::detail::g.counters.near_safe_prog0_fog, 0u);
        });
    };
    render_skin(skin_reference);
    const auto skin_expected = fixture.pixels;
    render_skin(skin_mesh);
    EXPECT_EQ(fixture.pixels, skin_expected);
    gfx::DestroyMesh(skin_reference);
    gfx::DestroyMesh(skin_mesh);
    auto interior_vertices = ScreenVertices({0, 0, 1});
    interior_vertices.push_back(Vert({std::ldexp(1.0f, -127), 0, .5f}, {0, 0, 1}));
    const auto interior_mesh = gfx::CreateMesh(interior_vertices, kTriangle);
    ASSERT_NE(interior_mesh, gfx::kNullMesh);
    const auto *interior_bounds = gfx::detail::LookupMesh(interior_mesh);
    ASSERT_NE(interior_bounds, nullptr);
    EXPECT_TRUE(interior_bounds->bounds_known);
    EXPECT_FALSE(interior_bounds->bounds_normal_inputs);
    auto interior_draw = Unlit(gfx::kProg0Unlit);
    SetWMvp(interior_draw.mesh.mvp, 1);
    Set(interior_draw.prog0.fog, {180, 255, 0, -60});
    EXPECT_FALSE(gfx::detail::Prog0FogIsNearSafe(*interior_bounds, interior_draw.mesh, interior_draw.prog0));
    gfx::DestroyMesh(interior_mesh);
    gfx::DestroyTexture(texture);
}

TEST(GfxGpuMeshDc2, Prog0NearSafeFogUsesActualInterpolatedMvp) {
    if (!gfx::detail::NearSafeFogEnabled()) {
        GTEST_SKIP() << "Requires KZ_FOG_NEAR_SAFE=1 before process startup";
    }
    GfxFixture fixture(kWidth, kHeight, 1.0f, Prog0Options());
    auto       vertices = ScreenVertices({0, 0, 1});
    vertices[0].position[2] = 2;
    vertices[1].position[2] = 4;
    vertices[2].position[2] = 6;
    const auto mesh = gfx::CreateMesh(vertices, kTriangle);
    const auto reference_mesh = gfx::CreateMesh(vertices, kTriangle);
    ASSERT_NE(mesh, gfx::kNullMesh);
    ASSERT_NE(reference_mesh, gfx::kNullMesh);
    ASSERT_TRUE(gfx::UpdateMeshVertices(reference_mesh, 0, vertices));
    const auto texture = gfx::CreateTexture({2, 2, gfx::TextureFormat::Rgba8, 1, true});
    ASSERT_NE(texture, gfx::kNullTexture);
    const std::array<uint32_t, 4> texels = {Rgba(255, 255, 255, 191), Rgba(255, 255, 255, 191),
                                            Rgba(255, 255, 255, 191), Rgba(255, 255, 255, 191)};
    ASSERT_TRUE(gfx::UpdateTexture(texture, 0, 0, 0, 2, 2, texels.data()));
    const auto                   binding = NearestBinding(texture);
    const std::array<uint8_t, 4> clear = {17, 29, 43, 71};
    for (bool current_safe : {false, true}) {
        SCOPED_TRACE(current_safe);
        const auto record = [&](gfx::MeshHandle handle, bool current) {
            auto transform = gfx::IdentityMeshTransform();
            transform.model[14] = current == current_safe ? 4.0f : 0.0f;
            std::fill_n(transform.projection, 16, 0);
            transform.projection[0] = transform.projection[5] = transform.projection[11] = 1;
            transform.projection[10] = .5f;
            transform.projection[14] = 1.5f;
            auto draw = Unlit(gfx::kProg0Unlit);
            Set(draw.prog0.fog, {180, 255, 0, -60});
            gfx::detail::Mat4 projection;
            gfx::detail::Mat4 model;
            std::memcpy(projection.data(), transform.projection, sizeof projection);
            std::memcpy(model.data(), transform.model, sizeof model);
            const auto mvp = gfx::detail::Multiply(projection, model);
            std::memcpy(draw.mesh.mvp, mvp.data(), sizeof draw.mesh.mvp);
            gfx::DrawState state;
            state.blend = true;
            state.alpha = {0, 1, 0, 1, 0};
            state.fog = true;
            state.fog_color[0] = 20;
            state.fog_color[1] = 40;
            state.fog_color[2] = 70;
            gfx::BeginRecording();
            gfx::Clear(true, clear.data(), true, 0);
            gfx::SetInterpKey(511);
            gfx::DrawMeshProg0(handle, 0, 3, draw.mesh, draw.prog0, binding, state, &transform);
            return gfx::EndRecording();
        };
        const auto previous = record(mesh, false);
        const auto current = record(mesh, true);
        const auto reference_previous = record(reference_mesh, false);
        const auto reference = record(reference_mesh, true);
        ASSERT_NE(previous, nullptr);
        ASSERT_NE(current, nullptr);
        ASSERT_NE(reference_previous, nullptr);
        ASSERT_NE(reference, nullptr);
        for (float alpha : {.1f, .5f, .9f}) {
            SCOPED_TRACE(alpha);
            ASSERT_TRUE(gfx::RenderList(*reference, 1, {.canonical = true}));
            ASSERT_TRUE(gfx::RenderList(*reference, alpha, {.previous = reference_previous.get(), .present = true}));
            EXPECT_EQ(gfx::detail::g.counters.near_safe_prog0_fog, 0u);
            ASSERT_TRUE(gfx::ReadbackFrame(fixture.pixels, fixture.width, fixture.height));
            const auto expected = fixture.pixels;
            ASSERT_TRUE(gfx::RenderList(*current, 1, {.canonical = true}));
            ASSERT_TRUE(gfx::RenderList(*current, alpha, {.previous = previous.get(), .present = true}));
            const float translation = current_safe ? 4 * alpha : 4 * (1 - alpha);
            EXPECT_EQ(gfx::detail::g.counters.near_safe_prog0_fog, translation > 1 ? 1u : 0u);
            ASSERT_TRUE(gfx::ReadbackFrame(fixture.pixels, fixture.width, fixture.height));
            EXPECT_EQ(fixture.pixels, expected);
            ASSERT_EQ(current->cache.match.size(), 1u);
            EXPECT_GE(current->cache.match[0], 0);
            size_t changed = 0;
            for (size_t i = 0; i + 3 < expected.size(); i += 4) {
                changed += expected[i] != clear[0] || expected[i + 1] != clear[1] || expected[i + 2] != clear[2] ? 1u : 0u;
            }
            EXPECT_GT(changed, 0u);
        }
    }
    gfx::DestroyTexture(texture);
    gfx::DestroyMesh(reference_mesh);
    gfx::DestroyMesh(mesh);
}

TEST(GfxGpuMeshDc2, SharedPaletteBlendsPreserveThresholdsAndCallLifetime) {
    GfxFixture fixture(192, 144, 1.0f, Prog0Options());
    const auto vertices = Skinned({Vert({-.07f, -.07f, .5f}, {0, 0, 1}),
                                   Vert({.07f, -.07f, .5f}, {0, 0, 1}),
                                   Vert({0, .07f, .5f}, {0, 0, 1})},
                                  {1, 0, 0, 0}, {0, 0, 0, 0});
    const auto mesh = gfx::CreateMesh(vertices, kTriangle);
    ASSERT_NE(mesh, gfx::kNullMesh);

    /**
     * Selects a palette pair and the record conditions under which it may interpolate.
     */
    struct Case {
        size_t before;                /**< Palette variant in the preceding list. */
        size_t after;                 /**< Palette variant in the current list. */
        float  threshold;             /**< Per-bone teleport cutoff. */
        bool   before_palette = true; /**< Whether the earlier record supplied skin data. */
        bool   before_skinned = true; /**< Whether the earlier shader performed skinning. */
        bool   matched = true;        /**< Whether both records share an interpolation identity. */
    };

    const std::array cases{
        Case{0, 0, 200},
        Case{0, 0, 200},
        Case{0, 0, .05f},
        Case{0, 0, 200},
        Case{1, 0, 200},
        Case{0, 1, 200},
        Case{0, 0, 200},
        Case{0, 0, -1},
        Case{0, 0, -1},
        Case{0, 0, 200, false},
        Case{0, 0, 200, true, false},
        Case{0, 0, 200, true, true, false},
        Case{2, 2, 200},
        Case{2, 2, 200},
        Case{0, 0, 0.0f},
        Case{0, 0, -0.0f}
    };
    auto material = [](size_t index, bool skinned) {
        auto draw = Unlit(skinned ? gfx::kProg0Skinned : 0);
        draw.prog0.diffuse[0] = draw.prog0.diffuse[1] = draw.prog0.diffuse[2] = 1;
        draw.prog0.object_color[0] = static_cast<float>(70 + index * 7);
        draw.prog0.object_color[1] = 135;
        draw.prog0.object_color[2] = 190;
        return draw;
    };
    auto transform_for = [](size_t index) {
        auto transform = gfx::IdentityMeshTransform();
        transform.local[12] = -.72f + static_cast<float>(index % 4) * .48f;
        transform.local[13] = -.72f + static_cast<float>(index / 4) * .48f;
        return transform;
    };
    for (int generation = 0; generation < 2; ++generation) {
        std::array<gfx::Prog0Palette, 3> before{};
        std::array<gfx::Prog0Palette, 3> after{};
        for (size_t variant = 0; variant < before.size(); ++variant) {
            for (uint32_t bone = 0; bone < gfx::kProg0Bones; ++bone) {
                SetBone(before[variant], bone, kIdentityBone);
                SetBone(after[variant], bone, kIdentityBone);
            }
            const float from = -.16f + .07f * static_cast<float>(variant + generation);
            const float to = variant == 2 ? from : .16f - .04f * static_cast<float>(variant + generation);
            const float angle_before = -.3f + .08f * static_cast<float>(variant) + .11f * static_cast<float>(generation);
            const float angle_after = variant == 2 ? angle_before : .45f - .07f * static_cast<float>(variant) + .09f * static_cast<float>(generation);
            const float c0 = std::cos(angle_before);
            const float s0 = std::sin(angle_before);
            const float c1 = std::cos(angle_after);
            const float s1 = std::sin(angle_after);
            SetBone(before[variant], 0, {c0, -s0, 0, from, s0, c0, 0, 0, 0, 0, 1, 0});
            SetBone(after[variant], 0, {c1, -s1, 0, to, s1, c1, 0, 0, 0, 0, 1, 0});
        }
        auto record = [&](bool current) {
            gfx::BeginRecording();
            gfx::Clear(true, kClear.data(), true, 0);
            for (size_t i = 0; i < cases.size(); ++i) {
                const auto &test = cases[i];
                const bool  skin = current || test.before_skinned;
                auto        draw = material(i, skin);
                const auto  transform = transform_for(i);
                std::copy(std::begin(transform.local), std::end(transform.local), draw.mesh.mvp);
                gfx::SetInterpKey(1 + i + (!current && !test.matched ? 1000 : 0), false, test.threshold);
                const auto *palette = !current && !test.before_palette ? nullptr : &(current ? after[test.after] : before[test.before]);
                gfx::DrawMeshProg0(mesh, 0, 3, draw.mesh, draw.prog0, {}, {}, &transform, palette);
            }
            return gfx::EndRecording();
        };
        const auto previous = record(false);
        const auto current = record(true);
        ASSERT_NE(previous, nullptr);
        ASSERT_NE(current, nullptr);
        for (const float alpha : {.25f, .75f, .5f, .25f}) {
            fixture.Frame(kClear, [&] {
                for (size_t i = 0; i < cases.size(); ++i) {
                    const auto &test = cases[i];
                    auto        draw = material(i, true);
                    const auto  transform = transform_for(i);
                    std::copy(std::begin(transform.local), std::end(transform.local), draw.mesh.mvp);
                    auto palette = after[test.after];
                    if (test.before_palette && test.before_skinned && test.matched) {
                        gfx::detail::BlendProg0Bones(before[test.before], after[test.after], alpha, test.threshold, palette);
                    }
                    gfx::DrawMeshProg0(mesh, 0, 3, draw.mesh, draw.prog0, {}, {}, &transform, &palette);
                }
            });
            const auto expected = fixture.pixels;
            ASSERT_TRUE(gfx::RenderList(*current, 1, {.canonical = true}));
            ASSERT_TRUE(gfx::RenderList(*current, alpha, {.previous = previous.get(), .present = true}));
            ASSERT_TRUE(gfx::ReadbackFrame(fixture.pixels, fixture.width, fixture.height));
            EXPECT_EQ(fixture.pixels, expected) << "generation " << generation << " alpha " << alpha;
        }
    }
    gfx::DestroyMesh(mesh);
}

TEST(GfxGpuMeshDc2, MatrixProductsPreservePerDrawStateAcrossExactReuse) {
    using gfx::detail::Mat4;
    /**
     * Changes one matrix input or selects a different per-draw shader path.
     */
    enum class Variant {
        Base,
        Plain,
        Clip,
        Local,
        Projection,
        Middle,
        Model,
        View,
        SignedZero,
        Singular
    };
    constexpr std::array variants = {
        Variant::Base, Variant::Base, Variant::Plain, Variant::Clip, Variant::Local, Variant::Local,
        Variant::Projection, Variant::Base, Variant::Middle, Variant::Middle, Variant::Model, Variant::View,
        Variant::Base, Variant::SignedZero, Variant::SignedZero, Variant::Singular, Variant::Base, Variant::Plain};
    GfxFixture                 fixture(200, 160, 1.0f, Prog0Options());
    std::vector<gfx::Vertex3D> vertices;
    std::vector<uint32_t>      indices;
    for (size_t i = 0; i < variants.size(); ++i) {
        const float x = -.8f + .4f * static_cast<float>(i % 5);
        const float y = -.7f + .466f * static_cast<float>(i / 5);
        for (const auto &point : std::array<std::array<float, 2>, 3>{
                 {{-.045f, -.04f}, {.045f, -.04f}, {0, .05f}}
        }) {
            indices.push_back(static_cast<uint32_t>(vertices.size()));
            vertices.push_back(Vert({x + point[0], y + point[1], .5f}, {0, 0, 1}));
        }
    }
    const auto mesh = gfx::CreateMesh(vertices, indices);
    ASSERT_NE(mesh, gfx::kNullMesh);
    const auto matrix = [](const float *data) {
        Mat4 result;
        std::memcpy(result.data(), data, sizeof result);
        return result;
    };
    for (int generation = 0; generation < 2; ++generation) {
        const auto transform_for = [&](Variant variant, bool current, bool breaker) {
            auto transform = gfx::IdentityMeshTransform();
            transform.model[12] = (current ? .035f : -.025f) + .005f * static_cast<float>(generation);
            transform.model[13] = current ? -.02f : .01f;
            transform.view[12] = (current ? .06f : -.04f) + .015f * static_cast<float>(generation);
            switch (variant) {
                case Variant::Local:
                    transform.local[13] = -.03f;
                    break;
                case Variant::Projection:
                    transform.projection[0] = .95f;
                    transform.projection[5] = 1.02f;
                    break;
                case Variant::Middle:
                    transform.middle[4] = .05f;
                    break;
                case Variant::Model:
                    transform.model[12] += .035f;
                    break;
                case Variant::View:
                    transform.view[12] += .15f;
                    break;
                case Variant::SignedZero:
                    transform.local[1] = std::bit_cast<float>(0x80000000u);
                    break;
                case Variant::Singular:
                    transform.middle[10] = 0;
                    break;
                default:
                    break;
            }
            if (breaker) {
                transform.model[12] += 100.0f;
            }
            return transform;
        };
        const auto record = [&](bool current, bool break_cache) {
            gfx::BeginRecording();
            gfx::Clear(true, kClear.data(), true, 0);
            for (size_t i = 0; i < variants.size(); ++i) {
                const auto emit = [&](bool breaker) {
                    const auto transform = transform_for(variants[i], current, breaker);
                    auto       draw = FourLights();
                    draw.prog0.flags = gfx::kProg0Clip | (i % 2 == 0 ? gfx::kProg0Real : gfx::kProg0PointLights);
                    Set(draw.prog0.object_color, {45.0f + 5.0f * static_cast<float>(i), 160.0f - 3.0f * static_cast<float>(i),
                                                  70.0f + 3.0f * static_cast<float>(i), 128});
                    Set(draw.prog0.diffuse, {.35f, .45f, .55f, 1});
                    Set(draw.prog0.eye, {.1f * static_cast<float>(i), -.2f, 2});
                    SetPoint(draw.prog0, 0, {.1f * static_cast<float>(i % 4) - .15f, .2f, 1.4f, 3}, {25, 70, 45, 0});
                    std::memcpy(draw.prog0.model_world, transform.model, sizeof transform.model);
                    Identity(draw.prog0.clip_or_view);
                    if (variants[i] == Variant::Clip) {
                        draw.prog0.clip_or_view[0] = 1.2f;
                        draw.prog0.clip_or_view[12] = .6f;
                    }
                    if (variants[i] == Variant::Plain) {
                        draw.mesh = ChronicleLit();
                        draw.mesh.normal_matrix[8] = .1f * static_cast<float>(i % 3);
                        draw.mesh.diffuse[0] = .25f + .02f * static_cast<float>(i);
                        draw.mesh.diffuse[1] = .6f;
                    }
                    const auto world_to_clip = gfx::detail::Multiply(
                        gfx::detail::Multiply(matrix(transform.projection), matrix(transform.view)), matrix(transform.middle));
                    const auto mvp = gfx::detail::Multiply(world_to_clip,
                                                           gfx::detail::Multiply(matrix(transform.model), matrix(transform.local)));
                    std::memcpy(draw.mesh.mvp, mvp.data(), sizeof draw.mesh.mvp);
                    gfx::DrawState state;
                    state.color_write_mask = breaker ? 0 : gfx::kWriteRgba;
                    state.depth_write = false;
                    state.stencil_test = false;
                    gfx::SetInterpKey((breaker ? 10000 : 100) + i);
                    const auto first = static_cast<uint32_t>(i * 3);
                    if (!breaker && variants[i] == Variant::Plain) {
                        gfx::DrawMesh(mesh, first, 3, draw.mesh, {}, state, &transform);
                    } else {
                        gfx::DrawMeshProg0(mesh, first, 3, draw.mesh, draw.prog0, {}, state, &transform);
                    }
                };
                if (break_cache) {
                    emit(true);
                }
                emit(false);
            }
            return gfx::EndRecording();
        };
        const auto previous = record(false, false);
        const auto current = record(true, false);
        const auto reference_previous = record(false, true);
        const auto reference = record(true, true);
        ASSERT_EQ(current->records.size(), variants.size());
        ASSERT_EQ(reference->records.size(), variants.size() * 2);
        const auto &first = current->records[0];
        const auto &repeat = current->records[1];
        EXPECT_EQ(first.camera, repeat.camera);
        EXPECT_EQ(std::memcmp(first.model.data(), repeat.model.data(), sizeof first.model), 0);
        EXPECT_EQ(std::memcmp(first.local.data(), repeat.local.data(), sizeof first.local), 0);
        EXPECT_EQ(std::memcmp(first.middle.data(), repeat.middle.data(), sizeof first.middle), 0);
        EXPECT_EQ(std::memcmp(first.projection.data(), repeat.projection.data(), sizeof first.projection), 0);
        const auto &first_entry = std::get<gfx::detail::MeshEntry>(current->entries[first.entry]);
        const auto &repeat_entry = std::get<gfx::detail::MeshEntry>(current->entries[repeat.entry]);
        ASSERT_GE(first_entry.prog0, 0);
        ASSERT_GE(repeat_entry.prog0, 0);
        const auto &first_block = current->prog0_blocks[static_cast<size_t>(first_entry.prog0)];
        const auto &repeat_block = current->prog0_blocks[static_cast<size_t>(repeat_entry.prog0)];
        EXPECT_EQ(std::memcmp(first_block.clip_or_view, repeat_block.clip_or_view, sizeof first_block.clip_or_view), 0);
        EXPECT_NE(std::memcmp(first_block.object_color, repeat_block.object_color, sizeof first_block.object_color), 0);
        EXPECT_EQ(std::bit_cast<uint32_t>(current->records[12].local[1]), 0u);
        EXPECT_EQ(std::bit_cast<uint32_t>(current->records[13].local[1]), 0x80000000u);
        EXPECT_EQ(std::memcmp(current->records[13].local.data(), current->records[14].local.data(), sizeof first.local), 0);
        for (size_t i = 0; i < variants.size(); ++i) {
            const auto &breaker = reference->records[i * 2];
            const auto &real = reference->records[i * 2 + 1];
            EXPECT_EQ(breaker.camera, real.camera);
            EXPECT_NE(std::memcmp(breaker.model.data(), real.model.data(), sizeof real.model), 0);
            EXPECT_GT(std::fabs(breaker.model[12] - real.model[12]), 99.0f);
            const auto &entry = std::get<gfx::detail::MeshEntry>(reference->entries[breaker.entry]);
            EXPECT_EQ(entry.state.color_write_mask, 0);
            EXPECT_FALSE(entry.state.depth_write);
            EXPECT_FALSE(entry.state.stencil_test);
            EXPECT_GE(entry.prog0, 0);
        }
        for (const float alpha : {.25f, .75f, .5f, .25f}) {
            ASSERT_TRUE(gfx::RenderList(*reference, 1, {.canonical = true}));
            ASSERT_TRUE(gfx::RenderList(*reference, alpha, {.previous = reference_previous.get(), .present = true}));
            ASSERT_TRUE(gfx::ReadbackFrame(fixture.pixels, fixture.width, fixture.height));
            const auto expected = fixture.pixels;
            size_t     colored_pixels = 0;
            for (size_t i = 0; i + 3 < expected.size(); i += 4) {
                colored_pixels += expected[i] != 0 || expected[i + 1] != 0 || expected[i + 2] != 0 ? 1u : 0u;
            }
            EXPECT_GT(colored_pixels, 0u);
            ASSERT_TRUE(gfx::RenderList(*current, 1, {.canonical = true}));
            ASSERT_TRUE(gfx::RenderList(*current, alpha, {.previous = previous.get(), .present = true}));
            ASSERT_TRUE(gfx::ReadbackFrame(fixture.pixels, fixture.width, fixture.height));
            EXPECT_EQ(fixture.pixels, expected) << "generation " << generation << " alpha " << alpha;
            ASSERT_EQ(current->cache.match.size(), variants.size());
            for (const auto match : current->cache.match) {
                EXPECT_GE(match, 0);
            }
            ASSERT_EQ(reference->cache.match.size(), variants.size() * 2);
            for (const auto match : reference->cache.match) {
                EXPECT_GE(match, 0);
            }
            for (size_t i = 0; i < variants.size(); ++i) {
                Mat4 breaker_model;
                Mat4 real_model;
                ASSERT_TRUE(gfx::detail::InterpolateAffine(reference_previous->records[i * 2].model,
                                                           reference->records[i * 2].model, alpha, breaker_model));
                ASSERT_TRUE(gfx::detail::InterpolateAffine(reference_previous->records[i * 2 + 1].model,
                                                           reference->records[i * 2 + 1].model, alpha, real_model));
                EXPECT_GT(std::fabs(breaker_model[12] - real_model[12]), 99.0f);
            }
        }
    }
    gfx::DestroyMesh(mesh);
}
