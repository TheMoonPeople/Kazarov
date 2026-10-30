#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "gfx/context.hpp"
#include "tests/gfx_fixture.hpp"

using namespace dc::test;

namespace {

constexpr int kWidth = 64;
constexpr int kHeight = 48;
const std::array<uint8_t, 4> kClear = {0, 0, 0, 0x80};
const std::vector<uint32_t> kTriangle = {0, 1, 2};
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
    gfx::Prog0Palette   palette; // read only by a kProg0Skinned draw
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
    return {{position[0], position[1], position[2]},
            {normal[0], normal[1], normal[2]},
            {0.0f, 0.0f},
            {color[0], color[1], color[2], color[3]}};
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
    const uint8_t texels[16] = {255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 255, 255, 255, 255};
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
    GfxFixture fixture(kWidth, kHeight, 1.0f, Prog0Options());
    gfx::MeshConstants plain = ChronicleLit();
    Render(fixture, ScreenVertices({0, 0, 1}), kTriangle, plain);
    const std::array<uint8_t, 4> expected = fixture.Pixel(fixture.width / 2, fixture.height / 2);
    gfx::MeshConstants flagged = plain;
    flagged.flags |= gfx::kMeshProg0;
    Render(fixture, ScreenVertices({0, 0, 1}), kTriangle, flagged);
    EXPECT_EQ(fixture.Pixel(fixture.width / 2, fixture.height / 2), expected);
}

TEST(GfxGpuMeshDc2, IndexedProg0MeshMatchesImmediate) {
    GfxFixture fixture(kWidth, kHeight, 1.0f, Prog0Options());
    const Prog0Draw c = FourLights();
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
    GfxFixture fixture(kWidth, kHeight, 1.0f, Prog0Options());
    const Prog0Draw c = FourLights();
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
    Prog0Draw c = Prog0(0);
    Set(c.prog0.ambient, {200, 200, 200, 128});
    SetDirectional(c.prog0, 0, {0, 0, 1}, {100, 100, 100, 0});
    Render(fixture, ScreenVertices({0, 0, 1}), kTriangle, c);
    EXPECT_TRUE(Centre(fixture, 255, 255, 255));
}

TEST(GfxGpuMeshDc2, LightsTurnIntoModelSpace) {
    GfxFixture fixture(kWidth, kHeight, 1.0f, Prog0Options());
    Prog0Draw c = Prog0(0);
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
    Prog0Draw c = Prog0(gfx::kProg0PointLights);
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
    Prog0Draw c = Prog0(gfx::kProg0Real);
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
    Prog0Draw c = Prog0(gfx::kProg0Unlit, gfx::kProg0FaceColor);
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
    GfxFixture fixture(kWidth, kHeight, 1.0f, Prog0Options());
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
    GfxFixture fixture(kWidth, kHeight, 1.0f, Prog0Options());
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
    Prog0Draw c = Prog0(gfx::kProg0Unlit | gfx::kProg0Skinned);
    Set(c.prog0.object_color, {200, 200, 200, 128});
    SetBone(c.palette, 0, kIdentityBone);
    SetBone(c.palette, 1, {1, 0, 0, 1, 0, 1, 0, 0, 0, 0, 1, 0});
    Render(fixture, Skinned(QuadVertices(), {0.5f, 0.5f, 0, 0}, {0, 1, 0, 0}), {0, 1, 2, 0, 2, 3}, c);
    EXPECT_TRUE(fixture.PixelNear(fixture.width * 5 / 8, fixture.height / 2, 200, 200, 200));
    EXPECT_TRUE(fixture.PixelNear(fixture.width / 8, fixture.height / 2, 0, 0, 0));
}

TEST(GfxGpuMeshDc2, SkinnedNormals) {
    GfxFixture fixture(kWidth, kHeight, 1.0f, Prog0Options());
    Prog0Draw c = Prog0(gfx::kProg0Skinned);
    SetBone(c.palette, 0, kIdentityBone);
    SetDirectional(c.prog0, 0, {0, 0, 1}, {100, 100, 100, 0});
    Render(fixture, Skinned(ScreenVertices({0, 0, 1}), {1, 0, 0, 0}, {0, 0, 0, 0}), kTriangle, c);
    EXPECT_TRUE(Centre(fixture, 100, 100, 100));
}

// prog0.md 10.6, bone-3 sub-case (O-8).
TEST(GfxGpuMeshDc2, SkinnedFourthInfluenceNormal) {
    GfxFixture fixture(kWidth, kHeight, 1.0f, Prog0Options());
    Prog0Draw c = Prog0(gfx::kProg0Skinned);
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
    Prog0Draw c = Prog0(gfx::kProg0Unlit | gfx::kProg0Scissor);
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

    GfxFixture fixture(kWidth, kHeight, 1.0f, Prog0Options());
    const std::vector<gfx::Vertex3D> triangle = {Vert({0.0f, 0.0f, 0.5f}, {0, 0, 1}),
                                                 Vert({0.5f, 0.0f, 0.5f}, {0, 0, 1}),
                                                 Vert({0.0f, 0.5f, 0.5f}, {0, 0, 1})};
    Prog0Draw c = Prog0(gfx::kProg0Unlit | gfx::kProg0Cull);
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
    Prog0Draw c = Prog0(gfx::kProg0Unlit);
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
    Prog0Draw c = Prog0(gfx::kProg0Unlit);
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
