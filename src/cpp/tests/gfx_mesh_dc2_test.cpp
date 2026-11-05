#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <format>
#include <fstream>
#include <functional>
#include <limits>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "gfx/alpha_trace.hpp"
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

/**
 * Rejects unsupported numerical premises before replacing finite zero-alpha shading.
 */
/**
 * Owns a small generic full-byte allowlist and restores its opt-in environment.
 */
struct AlphaTraceTestInput {
    EnvSnapshot                trace = SaveEnv("KZ_PERF_ATTACHMENT_ALPHA_TRACE");                                                           /**< Surrounding diagnostic request. */
    EnvSnapshot                allowlist = SaveEnv("KZ_PERF_ATTACHMENT_ALPHA_ALLOWLIST");                                                   /**< Surrounding private input path. */
    EnvSnapshot                fog = SaveEnv("KZ_FOG_NEAR_SAFE");                                                                           /**< Surrounding fog policy. */
    EnvSnapshot                child_fog = SaveEnv("KZ_PROG0_FOG_NEAR_SAFE");                                                               /**< Surrounding child fog policy. */
    std::filesystem::path      path;                                                                                                        /**< Disposable synthetic identity input. */
    std::vector<gfx::Vertex3D> vertices = ScreenVertices({0, 0, 1});                                                                        /**< Complete synthetic geometry. */
    std::vector<uint32_t>      indices = {0, 1, 2};                                                                                         /**< Complete synthetic triangle order. */
    std::array<uint32_t, 4>    pixels = {Rgba(31, 71, 127, 255), Rgba(191, 53, 89, 255), Rgba(83, 149, 211, 255), Rgba(237, 181, 43, 255)}; /**< Complete synthetic image. */

    /**
     * Builds explicit converted geometry, image bytes and their sole permitted relationship.
     */
    explicit AlphaTraceTestInput(bool enabled, bool varied_alpha = false, float uv_scale = 1, bool relationship = true, bool repeated_triangle = false, bool full_quad = false, float u_offset = 0) {
        path = std::filesystem::temp_directory_path() / std::format("kz-alpha-trace-{}.bin", std::chrono::steady_clock::now().time_since_epoch().count());
        if (full_quad) {
            vertices = {Vert({-1, -1, .5f}, {0, 0, 1}), Vert({1, -1, .5f}, {0, 0, 1}), Vert({1, 1, .5f}, {0, 0, 1}), Vert({-1, 1, .5f}, {0, 0, 1})};
            indices = {0, 1, 2, 0, 2, 3};
        }
        if (varied_alpha) {
            for (auto &vertex : vertices) {
                vertex.uv[0] = (vertex.position[0] + 1) * .5f * uv_scale + u_offset;
                vertex.uv[1] = full_quad ? 0 : (vertex.position[1] + 1) * .5f * uv_scale;
            }
            constexpr std::array alpha = {255u, 85u, 0u, 170u};
            for (size_t i = 0; i < pixels.size(); ++i) {
                pixels[i] = (pixels[i] & 0xffffffu) | (alpha[i] << 24);
            }
        }
        std::vector<uint8_t> bytes = {'K', 'Z', 'A', 'L', 'P', 'H', 'A', '1'};
        if (repeated_triangle) {
            indices.insert(indices.end(), {0, 1, 2});
        }
        const auto word = [&](uint32_t value) { for (unsigned i=0;i<4;++i){ bytes.push_back(static_cast<uint8_t>(value >> (8*i)));
} };
        for (uint32_t value : {1u, 1u, 1u, relationship ? 1u : 0u, static_cast<uint32_t>(vertices.size()), static_cast<uint32_t>(indices.size())}) {
            word(value);
        }
        for (const auto &vertex : vertices) {
            for (float value : vertex.position) {
                word(std::bit_cast<uint32_t>(value));
            }
            for (float value : vertex.uv) {
                word(std::bit_cast<uint32_t>(value));
            }
        }
        for (uint32_t index : indices) {
            word(index);
        }
        word(2);
        word(2);
        for (uint32_t pixel : pixels) {
            word(pixel);
        }
        if (relationship) {
            word(0);
            word(0);
        }
        std::ofstream file(path, std::ios::binary);
        file.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        file.close();
        EXPECT_EQ(SetEnv("KZ_PERF_ATTACHMENT_ALPHA_TRACE", enabled ? "1" : "0", 1), 0);
        EXPECT_EQ(SetEnv("KZ_PERF_ATTACHMENT_ALPHA_ALLOWLIST", path.string().c_str(), 1), 0);
        EXPECT_EQ(SetEnv("KZ_FOG_NEAR_SAFE", "1", 1), 0);
        EXPECT_EQ(SetEnv("KZ_PROG0_FOG_NEAR_SAFE", "1", 1), 0);
    }

    /**
     * Restores the caller and removes only this synthetic identity file.
     */
    ~AlphaTraceTestInput() {
        RestoreEnv(trace);
        RestoreEnv(allowlist);
        RestoreEnv(fog);
        RestoreEnv(child_fog);
        std::error_code error;
        std::filesystem::remove(path, error);
    }
};

/**
 * Samples the observation bridge clock without changing renderer execution.
 */
uint64_t AlphaTestNow() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

/**
 * Preserves full rendering while filling the six reserved actual canonical and replay stages.
 */
TEST(GfxGpuMeshDc2, AttachmentAlphaTracePreservesPixelsAndReservesAllStages) {
    std::vector<std::vector<uint8_t>> reference;
    for (bool enabled : {false, true}) {
        AlphaTraceTestInput input(enabled);
        GfxFixture          fixture(kWidth, kHeight, 1, Prog0Options());
        EXPECT_EQ(gfx::detail::AttachmentAlphaTraceEnabled(), enabled);
        if (!enabled) {
            EXPECT_EQ(gfx::detail::g.alpha_trace, nullptr);
        }
        const auto mesh = gfx::CreateMesh(input.vertices, kTriangle);
        const auto texture = gfx::CreateTexture({2, 2, gfx::TextureFormat::Rgba8, 1, true});
        ASSERT_TRUE(gfx::UpdateTexture(texture, 0, 0, 0, 2, 2, input.pixels.data()));
        EXPECT_EQ(gfx::detail::LookupMesh(mesh)->alpha_trace_aliases, enabled ? 1u : 0u);
        EXPECT_EQ(gfx::detail::LookupTexture(texture)->alpha_trace_aliases, enabled ? 1u : 0u);
        auto draw = Unlit(gfx::kProg0Unlit);
        SetWMvp(draw.mesh.mvp, 4);
        Set(draw.prog0.fog, {-153, 255, 0, 612000});
        gfx::DrawState state;
        state.blend = state.fog = true;
        state.alpha = {0, 2, 0, 1, 0};
        state.depth_write = state.stencil_test = true;
        state.stencil_front.pass = state.stencil_back.pass = gfx::StencilOp::Replace;
        state.stencil_front.reference = state.stencil_back.reference = 7;
        const auto record = [&](unsigned stage) {
            gfx::BeginRecording();
            gfx::Clear(true, kClear.data(), true, 0);
            gfx::ClearStencil(0);
            auto current = draw;
            current.prog0.diffuse[3] = stage == 0 ? .03260761499404907f : .005984187126159668f;
            if (stage == 2) {
                current.mesh.mvp[2] = 1;
                current.mesh.mvp[14] = 2.5f;
            }
            gfx::SetInterpKey(9021);
            gfx::DrawMeshProg0(mesh, 0, 3, current.mesh, current.prog0, NearestBinding(texture), state);
            gfx::DrawState consumer;
            consumer.depth_test = gfx::DepthTest::GEqual;
            consumer.stencil_test = true;
            consumer.stencil_front.compare = consumer.stencil_back.compare = gfx::CompareOp::Equal;
            consumer.stencil_front.reference = consumer.stencil_back.reference = 7;
            consumer.color_write_mask = gfx::kWriteRgba;
            const auto corner = Quad(0, 0, gfx::LogicalWidth(), gfx::LogicalHeight(), {37, 83, 191, 128}, 0, 0, 0, 0, .95f);
            gfx::Draw2D(gfx::Primitive::Quads, corner, {}, consumer);
            const auto rejected_depth = Quad(0, 0, gfx::LogicalWidth(), gfx::LogicalHeight(), {255, 0, 0, 128}, 0, 0, 0, 0, 0);
            gfx::Draw2D(gfx::Primitive::Quads, rejected_depth, {}, consumer);
            consumer.stencil_front.reference = consumer.stencil_back.reference = 9;
            const auto rejected_stencil = Quad(0, 0, gfx::LogicalWidth(), gfx::LogicalHeight(), {0, 255, 0, 128}, 0, 0, 0, 0, .99f);
            gfx::Draw2D(gfx::Primitive::Quads, rejected_stencil, {}, consumer);
            return gfx::EndRecording();
        };
        auto previous = record(0);
        // Warmup renders and resource identification cannot consume measured slots.
        ASSERT_TRUE(gfx::RenderList(*previous, 1, {.canonical = true}));
        EXPECT_EQ(gfx::detail::InspectAttachmentAlphaTrace().retained, 0u);
        gfx::BeginAttachmentAlphaTrace(AlphaTestNow());
        for (unsigned stage = 0; stage < 3; ++stage) {
            previous = record(stage);
            auto current = record(stage);
            ASSERT_TRUE(gfx::RenderList(*current, 1, {.canonical = true}));
            ASSERT_TRUE(gfx::RenderList(*current, .5f, {.previous = previous.get()}));
            ASSERT_TRUE(gfx::ReadbackFrame(fixture.pixels, fixture.width, fixture.height));
            const auto center = (static_cast<size_t>(fixture.height / 2) * fixture.width + fixture.width / 2) * 4;
            EXPECT_EQ(fixture.pixels[center], 37u);
            EXPECT_EQ(fixture.pixels[center + 1], 83u);
            EXPECT_EQ(fixture.pixels[center + 2], 191u);
            EXPECT_EQ(gfx::detail::g.counters.mesh_draws, 1u);
            if (!enabled) {
                reference.push_back(fixture.pixels);
            } else {
                EXPECT_EQ(fixture.pixels, reference[stage]);
            }
            if (enabled) {
                const auto stats = gfx::detail::InspectAttachmentAlphaTrace();
                EXPECT_EQ(stats.slots[stage] >= 0, true);
                EXPECT_EQ(stats.slots[3 + stage] >= 0, true);
                if (stage == 0) {
                    EXPECT_EQ(stats.slots[1], -1);
                    EXPECT_EQ(stats.slots[2], -1);
                    EXPECT_EQ(stats.slots[4], -1);
                    EXPECT_EQ(stats.slots[5], -1);
                }
            }
            previous = std::move(current);
        }
        gfx::EndAttachmentAlphaTrace(AlphaTestNow());
        gfx::LogAttachmentAlphaTrace();
        if (enabled) {
            const auto stats = gfx::detail::InspectAttachmentAlphaTrace();
            EXPECT_EQ(stats.retained, 6u);
            EXPECT_EQ(stats.overflow, 0u);
            EXPECT_EQ(stats.identity_overflow, 0u);
            for (size_t execution = 0; execution < 6; ++execution) {
                const auto events = gfx::detail::InspectAttachmentAlphaEvents(execution);
                ASSERT_FALSE(events.empty());
                EXPECT_NE(events.back().find("\"kind\":10"), std::string::npos);
                EXPECT_TRUE(std::any_of(events.begin(), events.end(), [](const std::string &event) { return event.find("\"quad\":[[") != std::string::npos; }));
            }
        }
        gfx::DestroyMesh(mesh);
        gfx::DestroyTexture(texture);
    }
}

/**
 * Invalidates identities only when actual updates execute and rejects modified or reused uploads.
 */
TEST(GfxGpuMeshDc2, AttachmentAlphaTraceIdentitiesFollowActualUploadsAndReuse) {
    AlphaTraceTestInput input(true);
    GfxFixture          fixture(kWidth, kHeight, 1, Prog0Options());
    auto                mesh = gfx::CreateMesh(input.vertices, kTriangle);
    auto                texture = gfx::CreateTexture({2, 2, gfx::TextureFormat::Rgba8, 1, true});
    ASSERT_TRUE(gfx::UpdateTexture(texture, 0, 0, 0, 2, 2, input.pixels.data()));
    ASSERT_EQ(gfx::detail::LookupMesh(mesh)->alpha_trace_aliases, 1u);
    ASSERT_EQ(gfx::detail::LookupTexture(texture)->alpha_trace_aliases, 1u);
    gfx::BeginRecording();
    EXPECT_TRUE(gfx::UpdateMeshVertices(mesh, 0, input.vertices));
    EXPECT_TRUE(gfx::UpdateTexture(texture, 0, 0, 0, 1, 1, input.pixels.data()));
    auto list = gfx::EndRecording();
    EXPECT_EQ(gfx::detail::LookupMesh(mesh)->alpha_trace_aliases, 1u);
    EXPECT_EQ(gfx::detail::LookupTexture(texture)->alpha_trace_aliases, 1u);
    ASSERT_TRUE(gfx::RenderList(*list, 1, {.canonical = true}));
    EXPECT_EQ(gfx::detail::LookupMesh(mesh)->alpha_trace_aliases, 0u);
    EXPECT_EQ(gfx::detail::LookupTexture(texture)->alpha_trace_aliases, 0u);
    ASSERT_TRUE(gfx::UpdateTexture(texture, 0, 0, 0, 2, 2, input.pixels.data()));
    EXPECT_EQ(gfx::detail::LookupTexture(texture)->alpha_trace_aliases, 1u);
    auto changed = input.vertices;
    changed[0].uv[0] = std::nextafter(0.0f, 1.0f);
    EXPECT_EQ(gfx::detail::AttachmentAlphaMatchMesh(changed, kTriangle), 0u);
    auto bad_indices = kTriangle;
    std::swap(bad_indices[0], bad_indices[1]);
    EXPECT_EQ(gfx::detail::AttachmentAlphaMatchMesh(input.vertices, bad_indices), 0u);
    const auto allocation = gfx::detail::LookupMesh(mesh)->alpha_trace_allocation;
    gfx::DestroyMesh(mesh);
    mesh = gfx::CreateMesh(input.vertices, kTriangle);
    EXPECT_GT(gfx::detail::LookupMesh(mesh)->alpha_trace_allocation, allocation);
    EXPECT_EQ(gfx::detail::LookupMesh(mesh)->alpha_trace_aliases, 1u);
    for (unsigned repeat = 0; repeat < 1100; ++repeat) {
        EXPECT_EQ(gfx::detail::AttachmentAlphaMatchTexture(gfx::detail::LookupTexture(texture)->desc, 0, 0, 0, 2, 2, input.pixels.data(), 0), 1u);
    }
    EXPECT_EQ(gfx::detail::InspectAttachmentAlphaTrace().identity_overflow, 0u);
    const auto target = gfx::CreateRenderTarget(2, 2, true);
    fixture.Frame(kClear, [&] { EXPECT_TRUE(gfx::CopyTexture(target, {0, 0, 2, 2}, texture, 0, 0)); });
    EXPECT_EQ(gfx::detail::LookupTexture(texture)->alpha_trace_aliases, 0u);
    gfx::DestroyMesh(mesh);
    gfx::DestroyTexture(texture);
    gfx::DestroyTexture(target);
}

/**
 * Keeps epochs, deduplication, signed provenance and bounded failures independent of rendering.
 */
TEST(GfxGpuMeshDc2, AttachmentAlphaTraceEpochSlotsAndCapsRemainConservative) {
    AlphaTraceTestInput input(true);
    GfxFixture          fixture(kWidth, kHeight, 1, Prog0Options());
    gfx::BeginAttachmentAlphaTrace(AlphaTestNow());
    gfx::detail::BeginAttachmentAlphaExecution(true, false);
    gfx::detail::AttachmentAlphaOrigin(101, 0, 1);
    gfx::detail::AlphaTraceEvent event;
    event.kind = gfx::detail::AlphaTraceKind::Draw;
    event.stages = 1;
    event.bound.scissor.offset = {-3, -7};
    gfx::detail::AttachmentAlphaObserve(event);
    gfx::detail::EndAttachmentAlphaExecution();
    auto stats = gfx::detail::InspectAttachmentAlphaTrace();
    ASSERT_EQ(stats.retained, 1u);
    EXPECT_GE(stats.slots[0], 0);
    EXPECT_EQ(stats.slots[1], -1);
    const auto events = gfx::detail::InspectAttachmentAlphaEvents(0);
    ASSERT_FALSE(events.empty());
    EXPECT_NE(events[1].find("[-3,-7,"), std::string::npos);
    gfx::detail::BeginAttachmentAlphaExecution(true, false);
    gfx::detail::AttachmentAlphaOrigin(102, 0, 1);
    event.stages = 6;
    gfx::detail::AttachmentAlphaObserve(event);
    gfx::detail::EndAttachmentAlphaExecution();
    stats = gfx::detail::InspectAttachmentAlphaTrace();
    EXPECT_EQ(stats.retained, 2u);
    EXPECT_EQ(stats.slots[1], stats.slots[2]);
    gfx::detail::BeginAttachmentAlphaExecution(false, true);
    EXPECT_FALSE(gfx::detail::AttachmentAlphaTraceRecording());
    gfx::EndAttachmentAlphaTrace(AlphaTestNow());
    gfx::BeginAttachmentAlphaTrace(AlphaTestNow());
    gfx::EndAttachmentAlphaTrace(AlphaTestNow());
    gfx::LogAttachmentAlphaTrace();
    stats = gfx::detail::InspectAttachmentAlphaTrace();
    EXPECT_EQ(stats.retained, 0u);
    for (auto slot : stats.slots) {
        EXPECT_EQ(slot, -1);
    }
    gfx::BeginAttachmentAlphaTrace(AlphaTestNow());
    gfx::detail::BeginAttachmentAlphaExecution(true, false);
    gfx::detail::AttachmentAlphaObserve(event);
    gfx::detail::BeginAttachmentAlphaExecution(false, true);
    EXPECT_FALSE(gfx::detail::AttachmentAlphaTraceRecording());
    gfx::detail::EndAttachmentAlphaExecution();
    EXPECT_EQ(gfx::detail::InspectAttachmentAlphaTrace().retained, 0u);
    gfx::detail::BeginAttachmentAlphaExecution(true, false);
    gfx::detail::AttachmentAlphaOrigin(103, 0, 1);
    event.stages = 1;
    for (unsigned i = 0; i < 5000; ++i) {
        gfx::detail::AttachmentAlphaObserve(event);
    }
    gfx::detail::EndAttachmentAlphaExecution();
    stats = gfx::detail::InspectAttachmentAlphaTrace();
    EXPECT_EQ(stats.retained, 0u);
    EXPECT_GT(stats.rejected, 0u);
    EXPECT_GT(stats.overflow, 0u);
    gfx::BeginAttachmentAlphaTrace(AlphaTestNow());
    gfx::detail::BeginAttachmentAlphaExecution(true, false);
    gfx::detail::AttachmentAlphaOrigin(104, 0, 1);
    gfx::detail::AttachmentAlphaObserve(event);
    gfx::EndAttachmentAlphaTrace(AlphaTestNow());
    gfx::detail::EndAttachmentAlphaExecution();
    EXPECT_EQ(gfx::detail::InspectAttachmentAlphaTrace().retained, 0u);
    // An execution already open when the next measurement begins cannot enter that epoch.
    gfx::BeginAttachmentAlphaTrace(AlphaTestNow());
    gfx::detail::BeginAttachmentAlphaExecution(true, false);
    gfx::detail::AttachmentAlphaOrigin(105, 0, 1);
    gfx::detail::AttachmentAlphaObserve(event);
    gfx::BeginAttachmentAlphaTrace(AlphaTestNow());
    gfx::detail::EndAttachmentAlphaExecution();
    EXPECT_EQ(gfx::detail::InspectAttachmentAlphaTrace().retained, 0u);
    std::ifstream           valid_input(input.path, std::ios::binary);
    const std::vector<char> valid_bytes{std::istreambuf_iterator<char>(valid_input), {}};
    valid_input.close();
    auto excessive_geometry = valid_bytes;
    excessive_geometry[12] = 17;
    for (const auto &invalid_bytes : {std::vector<char>{}, std::vector<char>(valid_bytes.begin(), valid_bytes.begin() + 20),
                                      std::vector<char>(256 * 1024 + 1), excessive_geometry}) {
        std::ofstream invalid_input(input.path, std::ios::binary | std::ios::trunc);
        invalid_input.write(invalid_bytes.data(), static_cast<std::streamsize>(invalid_bytes.size()));
        invalid_input.close();
        gfx::detail::InitializeAttachmentAlphaTrace();
        EXPECT_FALSE(gfx::detail::AttachmentAlphaTraceEnabled());
        EXPECT_FALSE(gfx::detail::AttachmentAlphaTraceRecording());
    }
}

/**
 * Rejects invalid zero-alpha inputs while retaining a finite positive interpolation denominator.
 */
TEST(GfxMeshDc2, Prog0ZeroSourceAlphaRequiresFinitePositiveWeightPremises) {
    gfx::detail::Mesh mesh;
    mesh.bounds_known = mesh.bounds_normal_inputs = mesh.bounded_uv_inputs = true;
    mesh.bounds_min = {-1, -1, 0};
    mesh.bounds_max = {3, 3, 0};
    auto draw = Unlit(gfx::kProg0Unlit);
    SetWMvp(draw.mesh.mvp, 4);
    Set(draw.prog0.fog, {-153, 255, 0, 612000});
    draw.prog0.diffuse[3] = .005984187126159668f;
    const auto eligible = [&] { return gfx::detail::Prog0HasZeroSourceAlpha(mesh, draw.mesh, draw.prog0); };
    ASSERT_TRUE(eligible());
    draw.prog0.flags = gfx::kProg0Unlit | gfx::kProg0Clip | gfx::kProg0Scissor;
    Identity(draw.prog0.clip_or_view);
    ASSERT_TRUE(eligible());
    const auto initial_mesh = mesh;
    const auto initial_draw = draw;
    const auto rejects = [&](const auto &change) {
        mesh = initial_mesh;
        draw = initial_draw;
        change();
        EXPECT_FALSE(eligible());
    };
    rejects([&] { mesh.bounds_known = false; });
    rejects([&] { mesh.bounds_normal_inputs = false; });
    rejects([&] { mesh.bounded_uv_inputs = false; });
    rejects([&] { draw.prog0.flags |= gfx::kProg0Skinned; });
    rejects([&] { draw.prog0.flags |= gfx::kProg0RefSphere; });
    rejects([&] { draw.prog0.flags = gfx::kProg0Unlit | gfx::kProg0Clip; });
    rejects([&] { draw.prog0.flags = gfx::kProg0Unlit | gfx::kProg0Scissor; });
    rejects([&] { draw.prog0.clip_or_view[0] = 0x1p21f; });
    rejects([&] { draw.prog0.flags = 0; });
    rejects([&] { draw.mesh.flags |= gfx::kMeshVertexColor; });
    rejects([&] { draw.mesh.mvp[15] = -4; });
    rejects([&] { draw.mesh.mvp[14] = 1; });
    rejects([&] { draw.mesh.mvp[14] = 8; });
    rejects([&] { draw.mesh.mvp[0] = 0x1p20f; });
    rejects([&] { draw.mesh.mvp[3] = 1; draw.mesh.mvp[15] = 1.01f; draw.mesh.mvp[14] = .001f; });
    rejects([&] {
        draw.mesh.mvp[3] = 255.5f;
        draw.mesh.mvp[15] = 257.5f;
        draw.mesh.mvp[14] = 1.5f;
        ASSERT_TRUE(gfx::detail::Prog0FogIsNearSafe(mesh, draw.mesh, draw.prog0));
    });
    rejects([&] { draw.prog0.fog[3] = 0; });
    rejects([&] { draw.prog0.fog[3] = 0x1p21f; });
    rejects([&] { draw.prog0.fog[1] = 254; });
    rejects([&] { draw.prog0.fog[2] = -0.0f; });
    for (float invalid : {INFINITY, -INFINITY, std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::denorm_min()}) {
        rejects([&] { draw.mesh.mvp[0] = invalid; });
        rejects([&] { draw.prog0.clip_or_view[0] = invalid; });
        rejects([&] { draw.prog0.object_color[0] = invalid; });
        rejects([&] { draw.prog0.diffuse[3] = invalid; });
        rejects([&] { draw.prog0.fog[0] = invalid; });
    }
    rejects([&] { draw.prog0.object_color[1] = 0x1p21f; });
    rejects([&] { draw.prog0.diffuse[3] = -.001f; });
    rejects([&] { draw.prog0.diffuse[3] = .03260761499404907f; });
    mesh = initial_mesh;
    draw = initial_draw;
    draw.prog0.object_color[3] = 1;
    for (float alpha : {0.0f, -0.0f, std::numeric_limits<float>::min(), 1.0f - 0x1p-20f}) {
        draw.prog0.diffuse[3] = alpha;
        EXPECT_TRUE(eligible());
    }
    for (float alpha : {std::nextafter(1.0f - 0x1p-20f, 1.0f), std::nextafter(1.0f, 0.0f), 1.0f, 1.5f}) {
        draw.prog0.diffuse[3] = alpha;
        EXPECT_FALSE(eligible());
    }
}

/**
 * Compares independent updated-mesh references while retaining zero-alpha coverage and side effects.
 */
TEST(GfxGpuMeshDc2, Prog0ZeroSourceAlphaMatchesGenericPixelsAndPreservesWrites) {
    /**
     * Restores the surrounding renderer fog policy after the independent comparison.
     */
    struct Restore {
        EnvSnapshot parent = SaveEnv("KZ_FOG_NEAR_SAFE"); /**< Original parent fog override. */

        /**
         * Restores the caller's policy after either success or an assertion return.
         */
        ~Restore() { RestoreEnv(parent); }
    } restore;

    ASSERT_EQ(SetEnv("KZ_FOG_NEAR_SAFE", "0", 1), 0);
    GfxFixture fixture(kWidth, kHeight, 1, Prog0Options());
    ASSERT_NE(gfx::detail::g.prog0_zero_source_alpha_pipeline, VK_NULL_HANDLE);

    /**
     * Owns the draw inputs and independently expected specialization outcome.
     */
    struct Case {
        std::string                name;                   /**< Descriptive numerical or state boundary. */
        Prog0Draw                  draw;                   /**< Actual uploaded shader arguments. */
        std::vector<gfx::Vertex3D> vertices;               /**< Complete immutable geometry and texture coordinates. */
        std::vector<uint32_t>      indices = kTriangle;    /**< Actual triangle-list indices. */
        bool                       eligible = true;        /**< Expected constant-output selection. */
        bool                       depth_write = false;    /**< Preserves depth changes for a following draw. */
        bool                       stencil = false;        /**< Preserves stencil replacement for a following draw. */
        bool                       stencil_reject = false; /**< Rejects source coverage through its stencil comparison. */
        bool                       consumer = false;       /**< Adds a draw that depends on the stored depth or stencil. */
        bool                       source_over = false;    /**< Uses a blend outside the exact additive target. */
        bool                       alpha_test = false;     /**< Uses a real positive alpha-test threshold. */
        bool                       ignore_alpha = false;   /**< Selects TEXA instead of ordinary sampled alpha. */
        bool                       mipmapped = false;      /**< Excludes a multi-level sampler resource. */
        bool                       opaque_target = false;  /**< Preserves a target without stored source alpha. */
        bool                       clip_witness = false;   /**< Requires real clipped coverage against a no-clip control. */
        bool                       tint = false;           /**< Keeps the existing interpolation status color path. */
    };

    Case basic;
    basic.name = "finite near-safe zero alpha";
    basic.draw = Unlit(gfx::kProg0Unlit);
    SetWMvp(basic.draw.mesh.mvp, 4);
    Set(basic.draw.prog0.fog, {-153, 255, 0, 612000});
    basic.draw.prog0.diffuse[3] = .005984187126159668f;
    basic.vertices = ScreenVertices({0, 0, 1}, {128, 64, 32, 128});
    basic.vertices[0].uv[0] = -4096;
    basic.vertices[1].uv[0] = 4096;
    basic.vertices[2].uv[1] = .99999994f;
    std::vector<Case> cases{basic};
    const auto        append = [&](std::string name, const auto &change) {
        Case test = basic;
        test.name = std::move(name);
        change(test);
        cases.push_back(std::move(test));
    };
    append("second positive-alpha instance", [](Case &test) { test.draw.prog0.diffuse[3] = .03260761499404907f; test.eligible = false; });
    append("vertex alpha input", [](Case &test) { test.draw.face = gfx::kProg0FaceColor; test.eligible = false; });
    append("source-over blend", [](Case &test) { test.source_over = true; test.eligible = false; });
    append("alpha-test discard", [](Case &test) { test.alpha_test = true; test.eligible = false; });
    append("TEXA path", [](Case &test) { test.ignore_alpha = true; test.eligible = false; });
    append("multiple mip levels", [](Case &test) { test.mipmapped = true; test.eligible = false; });
    append("opaque target", [](Case &test) { test.opaque_target = true; test.eligible = false; });
    append("status tint", [](Case &test) { test.tint = true; test.eligible = false; });
    append("UV bound exceeded", [](Case &test) { test.vertices[0].uv[0] = 4097; test.eligible = false; });
    append("positive Z boundary fallback", [](Case &test) { test.draw.mesh.mvp[14] = 1; test.eligible = false; });
    append("positive Z boundary inside", [](Case &test) { test.draw.mesh.mvp[14] = 1.001f; });
    append("near crossing fallback", [](Case &test) { test.draw.mesh.mvp[14] = 5; test.eligible = false; });
    append("tiny original W fallback", [](Case &test) {
        test.draw.mesh.mvp[3] = 1;
        test.draw.mesh.mvp[15] = 1.00001f;
        test.draw.mesh.mvp[14] = 1.5f;
        test.eligible = false;
    });
    append("negative original W crossing fallback", [](Case &test) {
        test.draw.mesh.mvp[3] = 2;
        test.draw.mesh.mvp[15] = 1;
        test.draw.mesh.mvp[14] = 1.5f;
        test.eligible = false;
    });
    append("fractional finite fog", [](Case &test) { Set(test.draw.prog0.fog, {180.25f, 255, 0, -60.5f}); });
    for (bool depth : {false, true}) {
        for (bool stencil : {false, true}) {
            append("depth/stencil consumer " + std::to_string(depth) + std::to_string(stencil), [=](Case &test) {
                test.depth_write = depth;
                test.stencil = stencil;
                test.consumer = true;
            });
        }
    }
    for (bool consumer : {false, true}) {
        append("failed stencil preserves alpha and depth " + std::to_string(consumer), [=](Case &test) {
            test.stencil = test.stencil_reject = test.depth_write = true;
            test.consumer = consumer;
        });
    }
    append("captured near-safe zero instance", [](Case &test) {
        const std::array<uint32_t, 16> bits = {0x40aa93e4, 0xbde7aa7d, 0xb7ca0938, 0x3e7699f0, 0xbe251a33, 0xc11f0c4f, 0xb8ce9814, 0x3f7c2a30, 0x3ead470c, 0xc03d0f61, 0x39ae483d, 0xc054b9bc, 0xc134710d, 0x42d71c99, 0x409fa8f6, 0x42de7b60};
        for (size_t i = 0; i < bits.size(); ++i) {
            test.draw.mesh.mvp[i] = std::bit_cast<float>(bits[i]);
        }
        test.vertices.clear();
        for (const auto &position : std::array{
                 std::array<float, 3>{-34.89485168457031f, 34.89485168457031f,  1.5253024230332812e-6f },
                 std::array<float, 3>{-34.89485168457031f, -34.89485168457031f, -1.5253024230332812e-6f},
                 std::array<float, 3>{34.89485168457031f,  -34.89485168457031f, -1.5253024230332812e-6f},
                 std::array<float, 3>{-34.89485168457031f, 34.89485168457031f,  1.5253024230332812e-6f },
                 std::array<float, 3>{34.89485168457031f,  -34.89485168457031f, -1.5253024230332812e-6f},
                 std::array<float, 3>{34.89485168457031f,  34.89485168457031f,  1.5253024230332812e-6f }
        }) {
            test.vertices.push_back(Vert(position, {0, 0, 1}, {128, 64, 32, 128}));
        }
        test.indices = {0, 1, 2, 3, 4, 5};
    });
    const auto side_effect_cases = cases;
    for (const auto &source : side_effect_cases) {
        if (source.consumer || source.stencil_reject) {
            Case value = source;
            value.name += " with six clip planes";
            value.draw.prog0.flags = gfx::kProg0Unlit | gfx::kProg0Clip | gfx::kProg0Scissor;
            Identity(value.draw.prog0.clip_or_view);
            cases.push_back(std::move(value));
        }
    }
    append("near-safe flags35 grid", [](Case &test) {
        test.draw.prog0.flags = gfx::kProg0Unlit | gfx::kProg0Clip | gfx::kProg0Scissor;
        Identity(test.draw.prog0.clip_or_view);
        test.vertices.clear();
        for (const auto &position : std::array{
                 std::array<float, 3>{-1, -1, .5f},
                 std::array<float, 3>{1,  -1, .5f},
                 std::array<float, 3>{1,  1,  .5f},
                 std::array<float, 3>{-1, 1,  .5f}
        }) {
            test.vertices.push_back(Vert(position, {0, 0, 1}));
        }
        test.indices = {0, 1, 2, 0, 2, 3};
    });
    append("flags35 positive alpha stays ordinary", [](Case &test) {
        test.draw.prog0.flags = gfx::kProg0Unlit | gfx::kProg0Clip | gfx::kProg0Scissor;
        Identity(test.draw.prog0.clip_or_view);
        test.draw.prog0.diffuse[3] = .03260761499404907f;
        test.eligible = false;
    });
    for (size_t plane = 0; plane < 6; ++plane) {
        append("user clip plane " + std::to_string(plane), [=](Case &test) {
            test.draw.prog0.flags = gfx::kProg0Unlit | gfx::kProg0Clip | gfx::kProg0Scissor;
            std::fill_n(test.draw.prog0.clip_or_view, 16, 0);
            const float sign = plane % 2 ? -1.0f : 1.0f;
            test.draw.prog0.clip_or_view[plane / 2] = sign;
            test.draw.prog0.clip_or_view[12 + plane / 2] = 2.0f * sign;
            test.draw.prog0.clip_or_view[15] = 2.0f;
            test.clip_witness = true;
        });
    }
    append("captured near-safe flags35 cylinder", [](Case &value) {
        value.draw.prog0.flags = gfx::kProg0Unlit | gfx::kProg0Clip | gfx::kProg0Scissor;
        const std::array<uint32_t, 16> mvp = {0xbf975cdau, 0xbfb49b0au, 0xb99d8193u, 0x40403fc0u, 0x331a57fbu, 0xc113f2fbu, 0x384c918eu, 0xbef9b149u, 0x4095a20fu, 0xbeb6b17au, 0xb89f53abu, 0x3f4278a8u, 0x3f6b9111u, 0x42798c6bu, 0x409fad36u, 0x42d41beeu};
        for (size_t i = 0; i < mvp.size(); ++i) {
            value.draw.mesh.mvp[i] = std::bit_cast<float>(mvp[i]);
        }
        const std::array<uint32_t, 16> clip = {0xbf899a3cu, 0xbfa42fdbu, 0x40404999u, 0x40403fc0u, 0x339b4756u, 0xc1067fceu, 0xbef9be12u, 0xbef9b149u, 0x408807b0u, 0xbea615b6u, 0x3f42829eu, 0x3f4278a8u, 0x3f5626efu, 0x4262dcc0u, 0x42c02646u, 0x42d41beeu};
        for (size_t i = 0; i < clip.size(); ++i) {
            value.draw.prog0.clip_or_view[i] = std::bit_cast<float>(clip[i]);
        }
        const std::array<uint32_t, 4> diffuse = {0x3f800000u, 0x3f800000u, 0x3f800000u, 0x00000000u};
        for (size_t i = 0; i < diffuse.size(); ++i) {
            value.draw.prog0.diffuse[i] = std::bit_cast<float>(diffuse[i]);
        }
        const std::array<uint32_t, 4> object = {0x43000000u, 0x43000000u, 0x43000000u, 0x43000000u};
        for (size_t i = 0; i < object.size(); ++i) {
            value.draw.prog0.object_color[i] = std::bit_cast<float>(object[i]);
        }
        value.vertices.clear();
        for (const auto &position : std::array{
                 std::array<float, 3>{0x1.09ec7a0000000p+3f,  0x0.0p+0f,             0x1.4bda7e0000000p+4f },
                 std::array<float, 3>{0x1.81c3700000000p+4f,  0x1.259aea0000000p+2f, 0x1.f934d80000000p+2f },
                 std::array<float, 3>{0x1.4c67800000000p+3f,  0x1.259aea0000000p+2f, 0x1.9ed11e0000000p+4f },
                 std::array<float, 3>{-0x1.09ec7a0000000p+3f, 0x0.0p+0f,             -0x1.4bda7e0000000p+4f},
                 std::array<float, 3>{-0x1.81c3780000000p+4f, 0x1.259aea0000000p+2f, -0x1.f934ce0000000p+2f},
                 std::array<float, 3>{-0x1.4c67800000000p+3f, 0x1.259aea0000000p+2f, -0x1.9ed11e0000000p+4f},
                 std::array<float, 3>{-0x1.e24c1a0000000p+2f, 0x0.0p+0f,             0x1.7045360000000p+4f },
                 std::array<float, 3>{0x1.09ec7a0000000p+3f,  0x0.0p+0f,             0x1.4bda7e0000000p+4f },
                 std::array<float, 3>{0x1.4c67800000000p+3f,  0x1.259aea0000000p+2f, 0x1.9ed11e0000000p+4f },
                 std::array<float, 3>{-0x1.e24c1a0000000p+2f, 0x0.0p+0f,             0x1.7045360000000p+4f },
                 std::array<float, 3>{0x1.4c67800000000p+3f,  0x1.259aea0000000p+2f, 0x1.9ed11e0000000p+4f },
                 std::array<float, 3>{-0x1.2d6f900000000p+3f, 0x1.259aea0000000p+2f, 0x1.cc56840000000p+4f },
                 std::array<float, 3>{0x1.2f7ad40000000p+4f,  0x0.0p+0f,             -0x1.79eb380000000p+3f},
                 std::array<float, 3>{0x1.e24c1a0000000p+2f,  0x0.0p+0f,             -0x1.7045360000000p+4f},
                 std::array<float, 3>{0x1.2d6f900000000p+3f,  0x1.259aea0000000p+2f, -0x1.cc56840000000p+4f},
                 std::array<float, 3>{-0x1.2f7adc0000000p+4f, 0x0.0p+0f,             0x1.79eb380000000p+3f },
                 std::array<float, 3>{-0x1.e24c1a0000000p+2f, 0x0.0p+0f,             0x1.7045360000000p+4f },
                 std::array<float, 3>{-0x1.2d6f900000000p+3f, 0x1.259aea0000000p+2f, 0x1.cc56840000000p+4f },
                 std::array<float, 3>{0x1.09ec7a0000000p+3f,  0x0.0p+0f,             0x1.4bda7e0000000p+4f },
                 std::array<float, 3>{0x1.349c520000000p+4f,  0x0.0p+0f,             0x1.942a560000000p+2f },
                 std::array<float, 3>{0x1.81c3700000000p+4f,  0x1.259aea0000000p+2f, 0x1.f934d80000000p+2f },
                 std::array<float, 3>{0x1.7b59720000000p+4f,  0x1.259aea0000000p+2f, -0x1.d8661c0000000p+3f},
                 std::array<float, 3>{0x1.349c520000000p+4f,  0x0.0p+0f,             0x1.942a560000000p+2f },
                 std::array<float, 3>{0x1.2f7ad40000000p+4f,  0x0.0p+0f,             -0x1.79eb380000000p+3f},
                 std::array<float, 3>{0x1.7b59720000000p+4f,  0x1.259aea0000000p+2f, -0x1.d8661c0000000p+3f},
                 std::array<float, 3>{0x1.2d6f900000000p+3f,  0x1.259aea0000000p+2f, -0x1.cc56840000000p+4f},
                 std::array<float, 3>{-0x1.09ec7a0000000p+3f, 0x0.0p+0f,             -0x1.4bda7e0000000p+4f},
                 std::array<float, 3>{-0x1.349c5a0000000p+4f, 0x0.0p+0f,             -0x1.942a620000000p+2f},
                 std::array<float, 3>{-0x1.81c3780000000p+4f, 0x1.259aea0000000p+2f, -0x1.f934ce0000000p+2f},
                 std::array<float, 3>{-0x1.7b59840000000p+4f, 0x1.259aea0000000p+2f, 0x1.d8662e0000000p+3f },
                 std::array<float, 3>{-0x1.349c5a0000000p+4f, 0x0.0p+0f,             -0x1.942a620000000p+2f},
                 std::array<float, 3>{-0x1.2f7adc0000000p+4f, 0x0.0p+0f,             0x1.79eb380000000p+3f },
                 std::array<float, 3>{-0x1.7b59840000000p+4f, 0x1.259aea0000000p+2f, 0x1.d8662e0000000p+3f },
                 std::array<float, 3>{-0x1.2d6f900000000p+3f, 0x1.259aea0000000p+2f, 0x1.cc56840000000p+4f }
        }) {
            value.vertices.push_back(Vert(position, {0, 0, 1}, {128, 64, 32, 128}));
        }
        value.indices = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 20, 19, 21, 22, 23, 24, 24, 23, 25, 26, 27, 28, 28, 27, 29, 30, 31, 32, 32, 31, 33};
    });
    const auto       texture = gfx::CreateTexture({2, 2, gfx::TextureFormat::Rgba8, 1, true});
    const auto       mipmapped = gfx::CreateTexture({2, 2, gfx::TextureFormat::Rgba8, 2, true});
    const auto       opaque_target = gfx::CreateRenderTarget(kWidth, kHeight, false);
    const std::array texels = {Rgba(0, 0, 0, 255), Rgba(227, 71, 119, 0), Rgba(83, 191, 107, 1), Rgba(199, 167, 47, 127)};
    ASSERT_TRUE(gfx::UpdateTexture(texture, 0, 0, 0, 2, 2, texels.data()));
    ASSERT_TRUE(gfx::UpdateTexture(mipmapped, 0, 0, 0, 2, 2, texels.data()));
    const std::array<uint8_t, 4> clear = {17, 29, 43, 71};
    fixture.Frame(clear, [] {});
    const auto clear_control = fixture.pixels;
    for (const auto &test : cases) {
        SCOPED_TRACE(test.name);
        std::vector<uint8_t> reference;
        uint32_t             reference_draws = 0;
        for (bool candidate : {false, true}) {
            const auto mesh = gfx::CreateMesh(test.vertices, test.indices);
            ASSERT_NE(mesh, gfx::kNullMesh);
            if (!candidate) {
                ASSERT_TRUE(gfx::UpdateMeshVertices(mesh, 0, test.vertices));
            }
            auto binding = NearestBinding(test.mipmapped ? mipmapped : texture);
            binding.filter = gfx::Filter::Linear;
            binding.wrap_u = binding.wrap_v = gfx::Wrap::Repeat;
            binding.ignore_alpha = test.ignore_alpha;
            std::vector<uint8_t> unclipped;
            if (test.clip_witness && !candidate) {
                auto ordinary = test.draw.prog0;
                ordinary.flags = gfx::kProg0Unlit;
                fixture.Frame(clear, [&] {
                    gfx::DrawState state;
                    state.blend = state.fog = true;
                    state.alpha = {0, 2, 0, 1, 0};
                    state.depth_test = gfx::DepthTest::GEqual;
                    gfx::DrawState occluder;
                    occluder.depth_write = true;
                    occluder.color_write_mask = 0;
                    const auto cover = Quad(0, 0, gfx::LogicalWidth() / 4, gfx::LogicalHeight(), {0, 0, 0, 128}, 0, 0, 0, 0, .8f);
                    gfx::Draw2D(gfx::Primitive::Quads, cover, {}, occluder);
                    gfx::DrawMeshProg0(mesh, 0, static_cast<uint32_t>(test.indices.size()), test.draw.mesh, ordinary, binding, state);
                });
                unclipped = fixture.pixels;
            }
            const auto before = gfx::detail::g.prog0_zero_source_alpha_selected;
            const auto guarded_before = gfx::detail::g.prog0_guarded_zero_alpha_selected;
            fixture.Frame(clear, [&] {
                if (test.opaque_target) {
                    gfx::SetRenderTarget(opaque_target);
                    gfx::Clear(true, clear.data(), true, 0);
                }
                gfx::ClearStencil(0);
                gfx::DrawState occluder;
                occluder.depth_write = true;
                occluder.color_write_mask = 0;
                const auto cover = Quad(0, 0, gfx::LogicalWidth() / 4, gfx::LogicalHeight(), {0, 0, 0, 128}, 0, 0, 0, 0, .8f);
                gfx::Draw2D(gfx::Primitive::Quads, cover, {}, occluder);
                gfx::DrawState state;
                state.blend = state.fog = true;
                state.alpha = {0, static_cast<uint8_t>(test.source_over ? 1 : 2), 0, 1, 0};
                state.alpha_test = test.alpha_test;
                state.alpha_func = gfx::AlphaFunc::GEqual;
                state.alpha_ref = 1;
                state.prog0_face = test.draw.face;
                state.depth_test = gfx::DepthTest::GEqual;
                state.depth_write = test.depth_write;
                state.stencil_test = test.stencil;
                state.stencil_front.compare = test.stencil_reject ? gfx::CompareOp::Equal : gfx::CompareOp::Always;
                state.stencil_front.pass = gfx::StencilOp::Replace;
                state.stencil_front.reference = 7;
                state.stencil_back = state.stencil_front;
                gfx::detail::tls.interpolation_tint = test.tint ? 1u : 0u;
                for (int layer = 0; layer < 2; ++layer) {
                    gfx::DrawMeshProg0(mesh, 0, static_cast<uint32_t>(test.indices.size()), test.draw.mesh, test.draw.prog0, binding, state);
                }
                gfx::detail::tls.interpolation_tint = 0;
                EXPECT_EQ(gfx::detail::g.prog0_zero_source_alpha_selected - before, candidate && test.eligible ? 2u : 0u);
                if (!candidate) {
                    EXPECT_EQ(gfx::detail::g.prog0_guarded_zero_alpha_selected, guarded_before);
                }
                if (test.consumer) {
                    gfx::DrawState consumer;
                    consumer.depth_test = gfx::DepthTest::GEqual;
                    consumer.stencil_test = test.stencil;
                    consumer.stencil_front.compare = gfx::CompareOp::Equal;
                    consumer.stencil_front.reference = test.stencil_reject ? 0 : 7;
                    consumer.stencil_back = consumer.stencil_front;
                    const auto blue = Quad(0, 0, gfx::LogicalWidth(), gfx::LogicalHeight(), {0, 0, 128, 128}, 0, 0, 0, 0, .25f);
                    gfx::Draw2D(gfx::Primitive::Quads, blue, {}, consumer);
                }
                if (test.opaque_target) {
                    gfx::SetRenderTarget(gfx::kMainTarget);
                    const auto quad = Quad(0, 0, gfx::LogicalWidth(), gfx::LogicalHeight(), {128, 128, 128, 128}, 0, 0, 1, 1);
                    gfx::Draw2D(gfx::Primitive::Quads, quad, NearestBinding(opaque_target), {});
                }
            });
            if (!unclipped.empty()) {
                size_t clipped_coverage = 0, unclipped_coverage = 0;
                for (size_t pixel = 3; pixel < fixture.pixels.size(); pixel += 4) {
                    clipped_coverage += fixture.pixels[pixel] == 0;
                    unclipped_coverage += unclipped[pixel] == 0;
                }
                EXPECT_GT(clipped_coverage, 0u);
                EXPECT_LT(clipped_coverage, unclipped_coverage);
                EXPECT_NE(fixture.pixels, unclipped);
            }
            if (!candidate) {
                reference = fixture.pixels;
                reference_draws = gfx::detail::g.counters.mesh_draws;
            } else {
                EXPECT_EQ(fixture.pixels, reference);
                EXPECT_EQ(gfx::detail::g.counters.mesh_draws, reference_draws);
            }
            if (test.eligible && !test.consumer && !test.stencil_reject) {
                size_t alpha_zero = 0;
                for (size_t pixel = 0; pixel + 3 < fixture.pixels.size(); pixel += 4) {
                    EXPECT_EQ(fixture.pixels[pixel], clear[0]);
                    EXPECT_EQ(fixture.pixels[pixel + 1], clear[1]);
                    EXPECT_EQ(fixture.pixels[pixel + 2], clear[2]);
                    alpha_zero += fixture.pixels[pixel + 3] == 0 ? 1u : 0u;
                }
                EXPECT_GT(alpha_zero, 0u);
            }
            if (test.stencil_reject && !test.consumer) {
                EXPECT_EQ(fixture.pixels, clear_control);
            }
            if (test.consumer) {
                const auto *center = &fixture.pixels[(fixture.height / 2 * fixture.width + fixture.width / 2) * 4];
                EXPECT_EQ(center[2], test.depth_write && !test.stencil_reject ? clear[2] : 128);
            }
            gfx::DestroyMesh(mesh);
        }
    }
    gfx::DestroyTexture(texture);
    gfx::DestroyTexture(mipmapped);
    gfx::DestroyTexture(opaque_target);
}

/**
 * Rejects invalid guarded inputs while admitting bounded near and eye crossings with user clip planes.
 */
TEST(GfxMeshDc2, Prog0GuardedZeroAlphaRequiresBoundedMaterialAndClipInputs) {
    gfx::detail::Mesh mesh;
    mesh.bounds_known = mesh.bounds_normal_inputs = mesh.bounded_uv_inputs = true;
    mesh.bounds_min = {-1, -1, 0};
    mesh.bounds_max = {3, 3, 0};
    auto draw = Unlit(gfx::kProg0Unlit);
    SetWMvp(draw.mesh.mvp, 4);
    draw.mesh.mvp[2] = 1;
    draw.mesh.mvp[14] = 2.5f;
    Set(draw.prog0.fog, {-153, 255, 0, 612000});
    draw.prog0.diffuse[3] = .005984187126159668f;
    const auto eligible = [&] { return gfx::detail::Prog0CanGuardZeroSourceAlpha(mesh, draw.mesh, draw.prog0); };
    ASSERT_FALSE(gfx::detail::Prog0FogIsNearSafe(mesh, draw.mesh, draw.prog0));
    ASSERT_TRUE(eligible());
    draw.prog0.flags = gfx::kProg0Unlit | gfx::kProg0Clip | gfx::kProg0Scissor;
    Identity(draw.prog0.clip_or_view);
    ASSERT_TRUE(eligible());
    const auto original_mesh = mesh;
    const auto original_draw = draw;
    const auto rejects = [&](const auto &change) {
        mesh = original_mesh;
        draw = original_draw;
        change();
        EXPECT_FALSE(eligible());
    };
    for (uint32_t flags : std::array<uint32_t, 7>{0, gfx::kProg0Unlit | gfx::kProg0Clip, gfx::kProg0Unlit | gfx::kProg0Scissor, gfx::kProg0Unlit | gfx::kProg0Cull, gfx::kProg0Skinned, gfx::kProg0ColorAdd, gfx::kProg0RefSphere}) {
        rejects([&] { draw.prog0.flags = flags; });
    }
    rejects([&] { mesh.bounds_known = false; });
    rejects([&] { mesh.bounds_normal_inputs = false; });
    rejects([&] { mesh.bounded_uv_inputs = false; });
    rejects([&] { draw.mesh.flags = gfx::kMeshVertexColor; });
    rejects([&] { mesh.bounds_min[0] = 99; });
    rejects([&] { draw.mesh.mvp[2] = 0; draw.mesh.mvp[14] = 1; });
    rejects([&] { draw.mesh.mvp[0] = 0x1p20f; });
    rejects([&] { draw.prog0.clip_or_view[0] = 0x1p20f; });
    rejects([&] { draw.prog0.diffuse[3] = -.1f; });
    rejects([&] { draw.prog0.diffuse[3] = .03260761499404907f; });
    rejects([&] { draw.prog0.fog[3] = 0; });
    rejects([&] { draw.prog0.fog[3] = 0x1p21f; });
    rejects([&] { draw.prog0.fog[1] = 254; });
    rejects([&] { draw.prog0.fog[2] = -0.0f; });
    for (float value : {INFINITY, -INFINITY, std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::denorm_min()}) {
        rejects([&] { draw.mesh.mvp[0] = value; });
        rejects([&] { draw.prog0.clip_or_view[0] = value; });
        rejects([&] { draw.prog0.object_color[0] = value; });
        rejects([&] { draw.prog0.diffuse[3] = value; });
        rejects([&] { draw.prog0.fog[3] = value; });
    }
    mesh = original_mesh;
    draw = original_draw;
    draw.mesh.mvp[2] = 0;
    draw.mesh.mvp[14] = 1.5f;
    draw.mesh.mvp[3] = 2;
    draw.mesh.mvp[15] = 1;
    EXPECT_FALSE(gfx::detail::Prog0FogIsNearSafe(mesh, draw.mesh, draw.prog0));
    EXPECT_TRUE(eligible());
    draw.prog0.object_color[3] = 1;
    draw.prog0.diffuse[3] = 1.0f - 0x1p-20f;
    EXPECT_TRUE(eligible());
    draw.prog0.diffuse[3] = std::nextafter(1.0f - 0x1p-20f, 1.0f);
    EXPECT_FALSE(eligible());
}

/**
 * Preserves crossing coverage and validates visible positive-alpha guarded sampling against independent references.
 */
TEST(GfxGpuMeshDc2, Prog0GuardedZeroAlphaCrossingsMatchIndependentPixelsAndFallback) {
    /**
     * Restores the surrounding renderer fog policy after the independent comparison.
     */
    struct Restore {
        EnvSnapshot child = SaveEnv("KZ_PROG0_FOG_NEAR_SAFE"); /**< Original child fog override. */
        EnvSnapshot parent = SaveEnv("KZ_FOG_NEAR_SAFE");      /**< Original parent fog override. */

        /**
         * Restores the caller's policy after either success or an assertion return.
         */
        ~Restore() {
            RestoreEnv(parent);
            RestoreEnv(child);
        }
    } restore;

    ASSERT_EQ(SetEnv("KZ_FOG_NEAR_SAFE", "1", 1), 0);
    ASSERT_EQ(SetEnv("KZ_PROG0_FOG_NEAR_SAFE", "1", 1), 0);
    GfxFixture fixture(kWidth, kHeight, 1, Prog0Options());
    ASSERT_NE(gfx::detail::g.prog0_guarded_zero_alpha_pipeline, VK_NULL_HANDLE);

    /**
     * Owns the draw inputs and independently expected specialization outcome.
     */
    struct Case {
        bool                       anisotropy = false;           /**< Excludes anisotropic filtering through the public sampler policy. */
        bool                       constant_expected = false;    /**< Requires the earlier unconditional sibling to retain priority. */
        bool                       force_fallback = false;       /**< Compares positive-alpha shader sampling through a scoped pipeline handle. */
        bool                       nonblack = false;             /**< Excludes other fog RGB inputs from the black sibling. */
        gfx::Filter                filter = gfx::Filter::Linear; /**< Selects the equal minification and magnification filter. */
        gfx::Wrap                  wrap = gfx::Wrap::Repeat;     /**< Selects both normalized sampler address modes. */
        std::string                name;                         /**< Descriptive numerical or state boundary. */
        Prog0Draw                  draw;                         /**< Actual uploaded shader arguments. */
        std::vector<gfx::Vertex3D> vertices;                     /**< Complete immutable geometry and texture coordinates. */
        std::vector<uint32_t>      indices = kTriangle;          /**< Actual triangle-list indices. */
        bool                       eligible = true;              /**< Expected guarded-sibling selection. */
        bool                       depth_write = false;          /**< Preserves depth changes for a following draw. */
        bool                       stencil = false;              /**< Preserves stencil replacement for a following draw. */
        bool                       stencil_reject = false;       /**< Rejects source coverage through its stencil comparison. */
        bool                       consumer = false;             /**< Adds a draw that depends on the stored depth or stencil. */
        bool                       source_over = false;          /**< Uses a blend outside the exact additive target. */
        bool                       alpha_test = false;           /**< Uses a real positive alpha-test threshold. */
        bool                       ignore_alpha = false;         /**< Selects TEXA instead of ordinary sampled alpha. */
        bool                       mipmapped = false;            /**< Excludes a multi-level sampler resource. */
        bool                       opaque_target = false;        /**< Preserves a target without stored source alpha. */
        bool                       tint = false;                 /**< Keeps the existing interpolation status color path. */
    };

    Case basic;
    basic.name = "mode1 near-crossing zero alpha";
    basic.draw = Unlit(gfx::kProg0Unlit);
    SetWMvp(basic.draw.mesh.mvp, 4);
    basic.draw.mesh.mvp[2] = 1;
    basic.draw.mesh.mvp[14] = 2.5f;
    Set(basic.draw.prog0.fog, {-153, 255, 0, 612000});
    basic.draw.prog0.diffuse[3] = .005984187126159668f;
    basic.vertices = ScreenVertices({0, 0, 1}, {128, 64, 32, 128});
    basic.vertices[0].uv[0] = basic.vertices[2].uv[0] = .25f;
    basic.vertices[1].uv[0] = .75f;
    basic.vertices[2].uv[1] = .75f;
    std::vector<Case> cases{basic};
    const auto        append = [&](std::string name, const auto &change) {
        Case value = basic;
        value.name = std::move(name);
        change(value);
        cases.push_back(std::move(value));
    };
    append("anisotropic sampler stays original", [](Case &value) { value.anisotropy = true; value.eligible = false; });
    append("six user clip planes", [](Case &value) { value.draw.prog0.flags = gfx::kProg0Unlit | gfx::kProg0Clip | gfx::kProg0Scissor; Identity(value.draw.prog0.clip_or_view); });
    append("negative original W", [](Case &value) { value.draw.mesh.mvp[2] = 0; value.draw.mesh.mvp[14] = 1.5f; value.draw.mesh.mvp[3] = 2; value.draw.mesh.mvp[15] = 1; });
    append("unconditional path priority", [](Case &value) { value.draw.mesh.mvp[2] = 0; value.draw.mesh.mvp[14] = 2; value.eligible = false; value.constant_expected = true; });
    append("positive instance stays original", [](Case &value) { value.draw.prog0.diffuse[3] = .03260761499404907f; value.eligible = false; });
    append("unsupported flag combination", [](Case &value) { value.draw.prog0.flags = 33; value.eligible = false; });
    append("vertex color stays original", [](Case &value) { value.draw.face = gfx::kProg0FaceColor; value.eligible = false; });
    append("source-over stays original", [](Case &value) { value.source_over = true; value.eligible = false; });
    append("alpha test stays original", [](Case &value) { value.alpha_test = true; value.eligible = false; });
    append("TEXA stays original", [](Case &value) { value.ignore_alpha = true; value.eligible = false; });
    append("multiple mips stay original", [](Case &value) { value.mipmapped = true; value.eligible = false; });
    append("opaque target stays original", [](Case &value) { value.opaque_target = true; value.eligible = false; });
    append("status tint stays original", [](Case &value) { value.tint = true; value.eligible = false; });
    append("nonblack mode1 stays original", [](Case &value) { value.nonblack = true; value.eligible = false; });
    append("UV CPU bound stays original", [](Case &value) { value.vertices[0].uv[0] = 4097; value.eligible = false; });
    for (bool depth : {false, true}) {
        for (bool stencil : {false, true}) {
            append("depth stencil consumer " + std::to_string(depth) + std::to_string(stencil), [=](Case &value) { value.depth_write = depth; value.stencil = stencil; value.consumer = true; });
        }
    }
    for (bool consumer : {false, true}) {
        append("failed stencil preserves writes " + std::to_string(consumer), [=](Case &value) { value.depth_write = value.stencil = value.stencil_reject = true; value.consumer = consumer; });
    }
    for (const auto filter : {gfx::Filter::Nearest, gfx::Filter::Linear}) {
        for (const auto wrap : {gfx::Wrap::Clamp, gfx::Wrap::Repeat}) {
            for (const auto interval : {
                     std::array<float, 2>{.25f,      .75f     },
                     {-.001f,    1.001f   },
                     {-512,      512      },
                     {1023.75f,  1024.25f },
                     {-1024.25f, -1023.75f}
            }) {
                for (bool forced : {false, true}) {
                    append("sampling " + std::to_string(static_cast<int>(filter)) + std::to_string(static_cast<int>(wrap)) + " low=" + std::to_string(interval[0]) + " forced=" + std::to_string(forced), [=](Case &value) {
                        value.filter = filter;
                        value.wrap = wrap;
                        value.force_fallback = forced;
                        value.vertices[0].uv[0] = value.vertices[2].uv[0] = interval[0];
                        value.vertices[1].uv[0] = interval[1];
                        value.vertices[2].uv[1] = interval[1];
                        if (forced) {
                            value.draw.prog0.diffuse[3] = .37f;
                            value.eligible = false;
                        }
                    });
                }
            }
        }
    }
    append("captured crossing grid", [](Case &value) {
        value.draw.prog0.flags = gfx::kProg0Unlit | gfx::kProg0Clip | gfx::kProg0Scissor;
        const std::array<uint32_t, 16> mvp = {0x40bd78b7u, 0xbd87977au, 0xb76c7f8du, 0x3e105528u, 0x00000000u, 0xc135bf10u, 0x387b4cbfu, 0xbf195daau, 0x3e60ad50u, 0x3fe4b0b9u, 0x39c770dbu, 0xc0736f08u, 0xc11ad338u, 0x42a6f369u, 0x409fab76u, 0x42d85f35u};
        for (size_t i = 0; i < mvp.size(); ++i) {
            value.draw.mesh.mvp[i] = std::bit_cast<float>(mvp[i]);
        }
        const std::array<uint32_t, 16> clip = {0x40ac3f34u, 0xbd7687c0u, 0x3e105c90u, 0x3e105528u, 0x335122a2u, 0xc1253955u, 0xbf196584u, 0xbf195daau, 0x3e4c4089u, 0x3fcfe67au, 0xc0737b7fu, 0xc0736f08u, 0xc10cbfe9u, 0x4297c602u, 0x42c469c6u, 0x42d85f35u};
        for (size_t i = 0; i < clip.size(); ++i) {
            value.draw.prog0.clip_or_view[i] = std::bit_cast<float>(clip[i]);
        }
        const std::array<uint32_t, 4> diffuse = {0x3f800000u, 0x3f800000u, 0x3f800000u, 0x00000000u};
        for (size_t i = 0; i < diffuse.size(); ++i) {
            value.draw.prog0.diffuse[i] = std::bit_cast<float>(diffuse[i]);
        }
        const std::array<uint32_t, 4> object = {0x43000000u, 0x43000000u, 0x43000000u, 0x43000000u};
        for (size_t i = 0; i < object.size(); ++i) {
            value.draw.prog0.object_color[i] = std::bit_cast<float>(object[i]);
        }
        value.vertices.clear();
        for (const auto &position : std::array{
                 std::array<float, 3>{-0x1.1728a80000000p+5f, 0x0.0p+0f, -0x1.1728a80000000p+5f},
                 std::array<float, 3>{-0x1.1728a80000000p+5f, 0x0.0p+0f, 0x1.1728a80000000p+5f },
                 std::array<float, 3>{0x1.1728a80000000p+5f,  0x0.0p+0f, 0x1.1728a80000000p+5f },
                 std::array<float, 3>{-0x1.1728a80000000p+5f, 0x0.0p+0f, -0x1.1728a80000000p+5f},
                 std::array<float, 3>{0x1.1728a80000000p+5f,  0x0.0p+0f, 0x1.1728a80000000p+5f },
                 std::array<float, 3>{0x1.1728a80000000p+5f,  0x0.0p+0f, -0x1.1728a80000000p+5f}
        }) {
            value.vertices.push_back(Vert(position, {0, 0, 1}, {128, 64, 32, 128}));
        }
        value.indices = {0, 1, 2, 0, 2, 5};
    });
    append("captured crossing cylinder", [](Case &value) {
        value.draw.prog0.flags = gfx::kProg0Unlit | gfx::kProg0Clip | gfx::kProg0Scissor;
        const std::array<uint32_t, 16> mvp = {0xbf676946u, 0xbfae4be4u, 0xb9980102u, 0x4039887bu, 0x33134647u, 0xc10d2c43u, 0x384332f7u, 0xbeee419du, 0x409067e5u, 0xbe8ba7ceu, 0xb8739653u, 0x3f14a8b2u, 0xbf1e0b27u, 0x4292fb31u, 0x409facbcu, 0x42d54535u};
        for (size_t i = 0; i < mvp.size(); ++i) {
            value.draw.mesh.mvp[i] = std::bit_cast<float>(mvp[i]);
        }
        const std::array<uint32_t, 16> clip = {0xbf525fb6u, 0xbf9e738au, 0x403991fcu, 0x4039887bu, 0x33942aaeu, 0xc10056c9u, 0xbeee4dd0u, 0xbeee419du, 0x4083472eu, 0xbe7deb48u, 0x3f14b04eu, 0x3f14a8b2u, 0xbf0fac91u, 0x42859e8au, 0x42c14f9du, 0x42d54535u};
        for (size_t i = 0; i < clip.size(); ++i) {
            value.draw.prog0.clip_or_view[i] = std::bit_cast<float>(clip[i]);
        }
        const std::array<uint32_t, 4> diffuse = {0x3f800000u, 0x3f800000u, 0x3f800000u, 0x00000000u};
        for (size_t i = 0; i < diffuse.size(); ++i) {
            value.draw.prog0.diffuse[i] = std::bit_cast<float>(diffuse[i]);
        }
        const std::array<uint32_t, 4> object = {0x43000000u, 0x43000000u, 0x43000000u, 0x43000000u};
        for (size_t i = 0; i < object.size(); ++i) {
            value.draw.prog0.object_color[i] = std::bit_cast<float>(object[i]);
        }
        value.vertices.clear();
        for (const auto &position : std::array{
                 std::array<float, 3>{0x1.c2fe640000000p+4f,  0x0.0p+0f,             -0x1.2d32260000000p+3f},
                 std::array<float, 3>{0x1.2980de0000000p+3f,  0x1.a6caa40000000p+1f, -0x1.79acb40000000p+4f},
                 std::array<float, 3>{0x1.a7ddd00000000p+4f,  0x1.a6caa40000000p+1f, -0x1.1b141c0000000p+3f},
                 std::array<float, 3>{-0x1.c2fe640000000p+4f, 0x0.0p+0f,             0x1.2d32260000000p+3f },
                 std::array<float, 3>{-0x1.2980ea0000000p+3f, 0x1.a6caa40000000p+1f, 0x1.79acc60000000p+4f },
                 std::array<float, 3>{-0x1.a7ddd00000000p+4f, 0x1.a6caa40000000p+1f, 0x1.1b141c0000000p+3f },
                 std::array<float, 3>{0x1.df877e0000000p+4f,  0x0.0p+0f,             0x1.79bc8a0000000p+3f },
                 std::array<float, 3>{0x1.c2fe640000000p+4f,  0x0.0p+0f,             -0x1.2d32260000000p+3f},
                 std::array<float, 3>{0x1.a7ddd00000000p+4f,  0x1.a6caa40000000p+1f, -0x1.1b141c0000000p+3f},
                 std::array<float, 3>{0x1.df877e0000000p+4f,  0x0.0p+0f,             0x1.79bc8a0000000p+3f },
                 std::array<float, 3>{0x1.a7ddd00000000p+4f,  0x1.a6caa40000000p+1f, -0x1.1b141c0000000p+3f},
                 std::array<float, 3>{0x1.c2af820000000p+4f,  0x1.a6caa40000000p+1f, 0x1.63040a0000000p+3f },
                 std::array<float, 3>{-0x1.c653be0000000p+3f, 0x0.0p+0f,             -0x1.a1b2ac0000000p+4f},
                 std::array<float, 3>{-0x1.df877e0000000p+4f, 0x0.0p+0f,             -0x1.79bc8a0000000p+3f},
                 std::array<float, 3>{-0x1.c2af820000000p+4f, 0x1.a6caa40000000p+1f, -0x1.63040a0000000p+3f},
                 std::array<float, 3>{0x1.c653be0000000p+3f,  0x0.0p+0f,             0x1.a1b2ae0000000p+4f },
                 std::array<float, 3>{0x1.df877e0000000p+4f,  0x0.0p+0f,             0x1.79bc8a0000000p+3f },
                 std::array<float, 3>{0x1.c2af820000000p+4f,  0x1.a6caa40000000p+1f, 0x1.63040a0000000p+3f },
                 std::array<float, 3>{0x1.c2fe640000000p+4f,  0x0.0p+0f,             -0x1.2d32260000000p+3f},
                 std::array<float, 3>{0x1.3c8b240000000p+3f,  0x0.0p+0f,             -0x1.91d88e0000000p+4f},
                 std::array<float, 3>{0x1.2980de0000000p+3f,  0x1.a6caa40000000p+1f, -0x1.79acb40000000p+4f},
                 std::array<float, 3>{-0x1.aaffee0000000p+3f, 0x1.a6caa40000000p+1f, -0x1.8892e60000000p+4f},
                 std::array<float, 3>{0x1.3c8b240000000p+3f,  0x0.0p+0f,             -0x1.91d88e0000000p+4f},
                 std::array<float, 3>{-0x1.c653be0000000p+3f, 0x0.0p+0f,             -0x1.a1b2ac0000000p+4f},
                 std::array<float, 3>{-0x1.aaffee0000000p+3f, 0x1.a6caa40000000p+1f, -0x1.8892e60000000p+4f},
                 std::array<float, 3>{-0x1.c2af820000000p+4f, 0x1.a6caa40000000p+1f, -0x1.63040a0000000p+3f},
                 std::array<float, 3>{-0x1.c2fe640000000p+4f, 0x0.0p+0f,             0x1.2d32260000000p+3f },
                 std::array<float, 3>{-0x1.3c8b280000000p+3f, 0x0.0p+0f,             0x1.91d89a0000000p+4f },
                 std::array<float, 3>{-0x1.2980ea0000000p+3f, 0x1.a6caa40000000p+1f, 0x1.79acc60000000p+4f },
                 std::array<float, 3>{0x1.aafff60000000p+3f,  0x1.a6caa40000000p+1f, 0x1.8892e80000000p+4f },
                 std::array<float, 3>{-0x1.3c8b280000000p+3f, 0x0.0p+0f,             0x1.91d89a0000000p+4f },
                 std::array<float, 3>{0x1.c653be0000000p+3f,  0x0.0p+0f,             0x1.a1b2ae0000000p+4f },
                 std::array<float, 3>{0x1.aafff60000000p+3f,  0x1.a6caa40000000p+1f, 0x1.8892e80000000p+4f },
                 std::array<float, 3>{0x1.c2af820000000p+4f,  0x1.a6caa40000000p+1f, 0x1.63040a0000000p+3f }
        }) {
            value.vertices.push_back(Vert(position, {0, 0, 1}, {128, 64, 32, 128}));
        }
        value.indices = {0, 1, 2, 3, 4, 5, 6, 0, 2, 6, 2, 11, 12, 13, 14, 15, 6, 11, 0, 19, 1, 19, 1, 21, 19, 12, 21, 12, 21, 14, 3, 27, 4, 27, 4, 29, 27, 15, 29, 15, 29, 11};
    });
    const auto       texture = gfx::CreateTexture({2, 2, gfx::TextureFormat::Rgba8, 1, true});
    const auto       mipmapped = gfx::CreateTexture({2, 2, gfx::TextureFormat::Rgba8, 2, true});
    const auto       opaque_target = gfx::CreateRenderTarget(kWidth, kHeight, false);
    const std::array texels = {Rgba(0, 0, 0, 255), Rgba(227, 71, 119, 0), Rgba(83, 191, 107, 1), Rgba(199, 167, 47, 127)};
    ASSERT_TRUE(gfx::UpdateTexture(texture, 0, 0, 0, 2, 2, texels.data()));
    ASSERT_TRUE(gfx::UpdateTexture(mipmapped, 0, 0, 0, 2, 2, texels.data()));
    const std::array<uint8_t, 4> clear = {17, 29, 43, 71};
    fixture.Frame(clear, [] {});
    const auto clear_control = fixture.pixels;
    for (const auto &test : cases) {
        SCOPED_TRACE(test.name);
        gfx::SetAnisotropy(test.anisotropy ? 2 : 0);
        EXPECT_EQ(gfx::detail::g.anisotropy_level > 0, test.anisotropy);
        std::vector<uint8_t> reference;
        uint32_t             reference_draws = 0;
        for (bool candidate : {false, true}) {
            const auto mesh = gfx::CreateMesh(test.vertices, test.indices);
            ASSERT_NE(mesh, gfx::kNullMesh);
            if (!candidate && !test.force_fallback) {
                ASSERT_TRUE(gfx::UpdateMeshVertices(mesh, 0, test.vertices));
            }
            auto binding = NearestBinding(test.mipmapped ? mipmapped : texture);
            binding.filter = test.filter;
            binding.wrap_u = binding.wrap_v = test.wrap;
            binding.ignore_alpha = test.ignore_alpha;

            /**
             * Restores the original owning pipeline slot before fixture destruction.
             */
            struct PipelineRestore {
                VkPipeline original = gfx::detail::g.prog0_mode1_black_fog_pipeline; /**< Original owned mode1-black handle. */

                /**
                 * Restores unique handle ownership after the visible positive-alpha comparison.
                 */
                ~PipelineRestore() { gfx::detail::g.prog0_mode1_black_fog_pipeline = original; }
            } restore_pipeline;

            if (candidate && test.force_fallback) {
                gfx::detail::g.prog0_mode1_black_fog_pipeline = gfx::detail::g.prog0_guarded_zero_alpha_pipeline;
            }
            const auto constant_before = gfx::detail::g.prog0_zero_source_alpha_selected;
            const auto before = gfx::detail::g.prog0_guarded_zero_alpha_selected;
            fixture.Frame(clear, [&] {
                if (test.opaque_target) {
                    gfx::SetRenderTarget(opaque_target);
                    gfx::Clear(true, clear.data(), true, 0);
                }
                gfx::ClearStencil(0);
                gfx::DrawState occluder;
                occluder.depth_write = true;
                occluder.color_write_mask = 0;
                const auto cover = Quad(0, 0, gfx::LogicalWidth() / 4, gfx::LogicalHeight(), {0, 0, 0, 128}, 0, 0, 0, 0, .8f);
                gfx::Draw2D(gfx::Primitive::Quads, cover, {}, occluder);
                gfx::DrawState state;
                state.blend = state.fog = true;
                state.fog_color[0] = test.nonblack ? 1 : 0;
                state.alpha = {0, static_cast<uint8_t>(test.source_over ? 1 : 2), 0, 1, 0};
                state.alpha_test = test.alpha_test;
                state.alpha_func = gfx::AlphaFunc::GEqual;
                state.alpha_ref = 1;
                state.prog0_face = test.draw.face;
                state.depth_test = gfx::DepthTest::GEqual;
                state.depth_write = test.depth_write;
                state.stencil_test = test.stencil;
                state.stencil_front.compare = test.stencil_reject ? gfx::CompareOp::Equal : gfx::CompareOp::Always;
                state.stencil_front.pass = gfx::StencilOp::Replace;
                state.stencil_front.reference = 7;
                state.stencil_back = state.stencil_front;
                gfx::detail::tls.interpolation_tint = test.tint ? 1u : 0u;
                for (int layer = 0; layer < 2; ++layer) {
                    gfx::DrawMeshProg0(mesh, 0, static_cast<uint32_t>(test.indices.size()), test.draw.mesh, test.draw.prog0, binding, state);
                }
                if (test.force_fallback) {
                    EXPECT_EQ(gfx::detail::g.bound.pipeline, candidate ? gfx::detail::g.prog0_guarded_zero_alpha_pipeline : restore_pipeline.original);
                }
                EXPECT_EQ(gfx::detail::g.prog0_zero_source_alpha_selected - constant_before, candidate && test.constant_expected ? 2u : 0u);
                gfx::detail::tls.interpolation_tint = 0;
                EXPECT_EQ(gfx::detail::g.prog0_guarded_zero_alpha_selected - before, candidate && test.eligible ? 2u : 0u);
                if (test.consumer) {
                    gfx::DrawState consumer;
                    consumer.depth_test = gfx::DepthTest::GEqual;
                    consumer.stencil_test = test.stencil;
                    consumer.stencil_front.compare = gfx::CompareOp::Equal;
                    consumer.stencil_front.reference = test.stencil_reject ? 0 : 7;
                    consumer.stencil_back = consumer.stencil_front;
                    const auto blue = Quad(0, 0, gfx::LogicalWidth(), gfx::LogicalHeight(), {0, 0, 128, 128}, 0, 0, 0, 0, .1f);
                    gfx::Draw2D(gfx::Primitive::Quads, blue, {}, consumer);
                }
                if (test.opaque_target) {
                    gfx::SetRenderTarget(gfx::kMainTarget);
                    const auto quad = Quad(0, 0, gfx::LogicalWidth(), gfx::LogicalHeight(), {128, 128, 128, 128}, 0, 0, 1, 1);
                    gfx::Draw2D(gfx::Primitive::Quads, quad, NearestBinding(opaque_target), {});
                }
            });
            if (!candidate) {
                reference = fixture.pixels;
                reference_draws = gfx::detail::g.counters.mesh_draws;
            } else {
                EXPECT_EQ(fixture.pixels, reference);
                EXPECT_EQ(gfx::detail::g.counters.mesh_draws, reference_draws);
            }
            if (test.eligible && !test.consumer && !test.stencil_reject) {
                size_t alpha_zero = 0;
                for (size_t pixel = 0; pixel + 3 < fixture.pixels.size(); pixel += 4) {
                    EXPECT_EQ(fixture.pixels[pixel], clear[0]);
                    EXPECT_EQ(fixture.pixels[pixel + 1], clear[1]);
                    EXPECT_EQ(fixture.pixels[pixel + 2], clear[2]);
                    alpha_zero += fixture.pixels[pixel + 3] == 0 ? 1u : 0u;
                }
                EXPECT_GT(alpha_zero, 0u);
            }
            if (test.force_fallback) {
                EXPECT_NE(fixture.pixels, clear_control);
            }
            if (test.stencil_reject && !test.consumer) {
                EXPECT_EQ(fixture.pixels, clear_control);
            }
            if (test.consumer) {
                const auto *center = &fixture.pixels[(fixture.height / 2 * fixture.width + fixture.width / 2) * 4];
                EXPECT_EQ(center[2], test.depth_write && !test.stencil_reject ? clear[2] : 128);
            }
            gfx::DestroyMesh(mesh);
        }
    }
    gfx::DestroyTexture(texture);
    gfx::DestroyTexture(mipmapped);
    gfx::DestroyTexture(opaque_target);
}

/**
 * Preserves upload metadata, recorded-update timing and eager cached pipelines across lifetimes.
 */
TEST(GfxGpuMeshDc2, Prog0ZeroSourceAlphaMetadataReplayAndPipelineLifetimes) {
    /**
     * Restores the caller's renderer construction policies after both lifetimes.
     */
    struct Restore {
        EnvSnapshot prewarm = SaveEnv("KZ_PIPELINE_PREWARM"); /**< Original eager creation override. */
        EnvSnapshot fog = SaveEnv("KZ_FOG_NEAR_SAFE");        /**< Original parent fog override. */

        /**
         * Restores both settings even when a fixture assertion returns early.
         */
        ~Restore() {
            RestoreEnv(prewarm);
            RestoreEnv(fog);
        }
    } restore;

    ASSERT_EQ(SetEnv("KZ_FOG_NEAR_SAFE", "0", 1), 0);
    for (bool prewarm : {false, true}) {
        SCOPED_TRACE(prewarm);
        ASSERT_EQ(SetEnv("KZ_PIPELINE_PREWARM", prewarm ? "1" : "0", 1), 0);
        {
            GfxFixture fixture(kWidth, kHeight, 1, Prog0Options());
            ASSERT_NE(gfx::detail::g.prog0_zero_source_alpha_pipeline, VK_NULL_HANDLE);
            const auto base = gfx::detail::g.prog0_zero_source_alpha_base;
            ASSERT_LT(base, gfx::detail::g.pipelines.size());
            EXPECT_EQ(gfx::detail::g.pipelines[base] != VK_NULL_HANDLE, prewarm);
            auto vertices = ScreenVertices({0, 0, 1});
            auto mesh = gfx::CreateMesh(vertices, kTriangle);
            ASSERT_TRUE(gfx::detail::LookupMesh(mesh)->bounded_uv_inputs);
            auto changed = vertices;
            changed[0].uv[0] = std::numeric_limits<float>::denorm_min();
            const auto invalid_uv = gfx::CreateMesh(changed, kTriangle);
            EXPECT_FALSE(gfx::detail::LookupMesh(invalid_uv)->bounded_uv_inputs);
            gfx::DestroyMesh(invalid_uv);
            const auto bad_index = gfx::CreateMesh(vertices, std::array<uint32_t, 3>{0, 1, 99});
            EXPECT_FALSE(gfx::detail::LookupMesh(bad_index)->bounds_known);
            EXPECT_FALSE(gfx::detail::LookupMesh(bad_index)->bounded_uv_inputs);
            gfx::DestroyMesh(bad_index);
            const auto     texture = gfx::CreateTexture({1, 1, gfx::TextureFormat::Rgba8, 1, true});
            const uint32_t texel = Rgba(127, 83, 191, 255);
            ASSERT_TRUE(gfx::UpdateTexture(texture, 0, 0, 0, 1, 1, &texel));
            const auto                        binding = NearestBinding(texture);
            const std::array<uint8_t, 4>      clear = {17, 29, 43, 71};
            std::vector<std::vector<uint8_t>> reference;
            for (bool candidate : {false, true}) {
                SCOPED_TRACE(candidate);
                for (size_t phase = 0; phase < 3; ++phase) {
                    {
                        const auto record = [&](bool current) {
                            gfx::BeginRecording();
                            gfx::Clear(true, clear.data(), true, 0);
                            if (!candidate && current) {
                                EXPECT_TRUE(gfx::UpdateMeshVertices(mesh, 0, vertices));
                                EXPECT_TRUE(gfx::detail::LookupMesh(mesh)->bounded_uv_inputs);
                            }
                            for (uint32_t layer = 0; layer < 2; ++layer) {
                                auto transform = gfx::IdentityMeshTransform();
                                std::fill_n(transform.projection, 16, 0);
                                transform.projection[0] = transform.projection[5] = transform.projection[15] = 4;
                                transform.projection[2] = phase == 1 ? 1.0f : 0.0f;
                                transform.projection[14] = phase == 1 ? 2.5f : 2.0f;
                                transform.model[12] = (current ? .2f : -.2f) + .1f * static_cast<float>(layer);
                                gfx::detail::Mat4 projection, model;
                                std::copy_n(transform.projection, 16, projection.begin());
                                std::copy_n(transform.model, 16, model.begin());
                                const auto mvp = gfx::detail::Multiply(projection, model);
                                auto       draw = Unlit(gfx::kProg0Unlit | gfx::kProg0Clip | gfx::kProg0Scissor);
                                Identity(draw.prog0.clip_or_view);
                                std::copy(mvp.begin(), mvp.end(), draw.mesh.mvp);
                                Set(draw.prog0.fog, {-153, 255, 0, 612000});
                                draw.prog0.diffuse[3] = layer == 0 ? .005984187126159668f : .03260761499404907f;
                                gfx::DrawState state;
                                state.blend = state.fog = true;
                                state.alpha = {0, 2, 0, 1, 0};
                                gfx::SetInterpKey(800 + layer);
                                gfx::DrawMeshProg0(mesh, 0, 3, draw.mesh, draw.prog0, binding, state, &transform);
                            }
                            return gfx::EndRecording();
                        };
                        const auto previous = record(false);
                        const auto current = record(true);
                        ASSERT_TRUE(gfx::detail::LookupMesh(mesh)->bounded_uv_inputs);
                        for (size_t step = 0; step < 3; ++step) {
                            const auto before = gfx::detail::g.prog0_zero_source_alpha_selected;
                            const auto guarded_before = gfx::detail::g.prog0_guarded_zero_alpha_selected;
                            ASSERT_TRUE(gfx::RenderList(*current, 1, {.canonical = true}));
                            EXPECT_EQ(gfx::detail::g.prog0_zero_source_alpha_selected - before, candidate && phase != 1 ? 1u : 0u);
                            EXPECT_EQ(gfx::detail::g.prog0_guarded_zero_alpha_selected, guarded_before);
                            EXPECT_EQ(gfx::detail::LookupMesh(mesh)->bounded_uv_inputs, candidate);
                            ASSERT_TRUE(gfx::RenderList(*current, .1f + .4f * static_cast<float>(step), {.previous = previous.get(), .present = true}));
                            ASSERT_TRUE(gfx::ReadbackFrame(fixture.pixels, fixture.width, fixture.height));
                            EXPECT_EQ(gfx::detail::g.prog0_zero_source_alpha_selected - before, candidate && phase != 1 ? 2u : 0u);
                            EXPECT_EQ(gfx::detail::g.prog0_guarded_zero_alpha_selected, guarded_before);
                            EXPECT_EQ(gfx::detail::g.counters.mesh_draws, 2u);
                            if (!candidate) {
                                reference.push_back(fixture.pixels);
                            } else {
                                EXPECT_EQ(fixture.pixels, reference[phase * 3 + step]);
                            }
                        }
                    }
                    gfx::DestroyMesh(mesh);
                    mesh = gfx::CreateMesh(vertices, kTriangle);
                    ASSERT_TRUE(gfx::detail::LookupMesh(mesh)->bounded_uv_inputs);
                }
            }
            EXPECT_TRUE(gfx::UpdateMeshVertices(mesh, 0, vertices));
            EXPECT_FALSE(gfx::detail::LookupMesh(mesh)->bounded_uv_inputs);
            EXPECT_FALSE(gfx::detail::LookupMesh(mesh)->bounds_known);
            gfx::DestroyMesh(mesh);
            changed = vertices;
            changed[0].uv[1] = 4097;
            mesh = gfx::CreateMesh(changed, kTriangle);
            EXPECT_FALSE(gfx::detail::LookupMesh(mesh)->bounded_uv_inputs);
            gfx::DestroyMesh(mesh);
            gfx::DestroyTexture(texture);
        }
        EXPECT_EQ(gfx::detail::g.prog0_zero_source_alpha_pipeline, VK_NULL_HANDLE);
        EXPECT_EQ(gfx::detail::g.prog0_zero_source_alpha_base, UINT32_MAX);
    }
}

/**
 * Preserves upload metadata, recorded-update timing and eager cached pipelines across lifetimes.
 */
TEST(GfxGpuMeshDc2, Prog0GuardedZeroAlphaRecordedUpdatesAndPipelineLifetimes) {
    /**
     * Restores the caller's renderer construction policies after both lifetimes.
     */
    struct Restore {
        EnvSnapshot prewarm = SaveEnv("KZ_PIPELINE_PREWARM");  /**< Original eager creation override. */
        EnvSnapshot child = SaveEnv("KZ_PROG0_FOG_NEAR_SAFE"); /**< Original child fog override. */
        EnvSnapshot fog = SaveEnv("KZ_FOG_NEAR_SAFE");         /**< Original parent fog override. */

        /**
         * Restores both settings even when a fixture assertion returns early.
         */
        ~Restore() {
            RestoreEnv(prewarm);
            RestoreEnv(fog);
            RestoreEnv(child);
        }
    } restore;

    ASSERT_EQ(SetEnv("KZ_FOG_NEAR_SAFE", "1", 1), 0);
    ASSERT_EQ(SetEnv("KZ_PROG0_FOG_NEAR_SAFE", "1", 1), 0);
    for (bool prewarm : {false, true}) {
        SCOPED_TRACE(prewarm);
        ASSERT_EQ(SetEnv("KZ_PIPELINE_PREWARM", prewarm ? "1" : "0", 1), 0);
        {
            GfxFixture fixture(kWidth, kHeight, 1, Prog0Options());
            ASSERT_NE(gfx::detail::g.prog0_guarded_zero_alpha_pipeline, VK_NULL_HANDLE);
            const auto base = gfx::detail::g.prog0_mode1_fog_base;
            ASSERT_LT(base, gfx::detail::g.pipelines.size());
            EXPECT_EQ(gfx::detail::g.pipelines[base] != VK_NULL_HANDLE, prewarm);
            auto vertices = ScreenVertices({0, 0, 1});
            auto mesh = gfx::CreateMesh(vertices, kTriangle);
            ASSERT_TRUE(gfx::detail::LookupMesh(mesh)->bounded_uv_inputs);
            auto changed = vertices;
            changed[0].uv[0] = std::numeric_limits<float>::denorm_min();
            const auto invalid_uv = gfx::CreateMesh(changed, kTriangle);
            EXPECT_FALSE(gfx::detail::LookupMesh(invalid_uv)->bounded_uv_inputs);
            gfx::DestroyMesh(invalid_uv);
            const auto bad_index = gfx::CreateMesh(vertices, std::array<uint32_t, 3>{0, 1, 99});
            EXPECT_FALSE(gfx::detail::LookupMesh(bad_index)->bounds_known);
            EXPECT_FALSE(gfx::detail::LookupMesh(bad_index)->bounded_uv_inputs);
            gfx::DestroyMesh(bad_index);
            const auto     texture = gfx::CreateTexture({1, 1, gfx::TextureFormat::Rgba8, 1, true});
            const uint32_t texel = Rgba(127, 83, 191, 255);
            ASSERT_TRUE(gfx::UpdateTexture(texture, 0, 0, 0, 1, 1, &texel));
            const auto                        binding = NearestBinding(texture);
            const std::array<uint8_t, 4>      clear = {17, 29, 43, 71};
            std::vector<std::vector<uint8_t>> reference;
            for (bool candidate : {false, true}) {
                SCOPED_TRACE(candidate);
                const auto record = [&](bool current) {
                    gfx::BeginRecording();
                    gfx::Clear(true, clear.data(), true, 0);
                    if (!candidate && current) {
                        EXPECT_TRUE(gfx::UpdateMeshVertices(mesh, 0, vertices));
                        EXPECT_TRUE(gfx::detail::LookupMesh(mesh)->bounded_uv_inputs);
                    }
                    for (uint32_t layer = 0; layer < 2; ++layer) {
                        auto transform = gfx::IdentityMeshTransform();
                        std::fill_n(transform.projection, 16, 0);
                        transform.projection[0] = transform.projection[5] = transform.projection[15] = 4;
                        transform.projection[2] = 1;
                        transform.projection[14] = 2.5f;
                        transform.model[12] = (current ? .2f : -.2f) + .1f * static_cast<float>(layer);
                        gfx::detail::Mat4 projection, model;
                        std::copy_n(transform.projection, 16, projection.begin());
                        std::copy_n(transform.model, 16, model.begin());
                        const auto mvp = gfx::detail::Multiply(projection, model);
                        auto       draw = Unlit(gfx::kProg0Unlit);
                        std::copy(mvp.begin(), mvp.end(), draw.mesh.mvp);
                        Set(draw.prog0.fog, {-153, 255, 0, 612000});
                        draw.prog0.diffuse[3] = layer == 0 ? .005984187126159668f : .03260761499404907f;
                        gfx::DrawState state;
                        state.blend = state.fog = true;
                        state.alpha = {0, 2, 0, 1, 0};
                        gfx::SetInterpKey(800 + layer);
                        gfx::DrawMeshProg0(mesh, 0, 3, draw.mesh, draw.prog0, binding, state, &transform);
                    }
                    return gfx::EndRecording();
                };
                const auto previous = record(false);
                const auto current = record(true);
                ASSERT_TRUE(gfx::detail::LookupMesh(mesh)->bounded_uv_inputs);
                for (size_t step = 0; step < 3; ++step) {
                    const auto before = gfx::detail::g.prog0_guarded_zero_alpha_selected;
                    ASSERT_TRUE(gfx::RenderList(*current, 1, {.canonical = true}));
                    EXPECT_EQ(gfx::detail::LookupMesh(mesh)->bounded_uv_inputs, candidate);
                    ASSERT_TRUE(gfx::RenderList(*current, .1f + .4f * static_cast<float>(step), {.previous = previous.get(), .present = true}));
                    ASSERT_TRUE(gfx::ReadbackFrame(fixture.pixels, fixture.width, fixture.height));
                    EXPECT_EQ(gfx::detail::g.prog0_guarded_zero_alpha_selected - before, candidate ? 2u : 0u);
                    EXPECT_EQ(gfx::detail::g.counters.mesh_draws, 2u);
                    if (!candidate) {
                        reference.push_back(fixture.pixels);
                    } else {
                        EXPECT_EQ(fixture.pixels, reference[step]);
                    }
                }
                gfx::DestroyMesh(mesh);
                mesh = gfx::CreateMesh(vertices, kTriangle);
                ASSERT_TRUE(gfx::detail::LookupMesh(mesh)->bounded_uv_inputs);
            }
            EXPECT_TRUE(gfx::UpdateMeshVertices(mesh, 0, vertices));
            EXPECT_FALSE(gfx::detail::LookupMesh(mesh)->bounded_uv_inputs);
            EXPECT_FALSE(gfx::detail::LookupMesh(mesh)->bounds_known);
            gfx::DestroyMesh(mesh);
            changed = vertices;
            changed[0].uv[1] = 4097;
            mesh = gfx::CreateMesh(changed, kTriangle);
            EXPECT_FALSE(gfx::detail::LookupMesh(mesh)->bounded_uv_inputs);
            gfx::DestroyMesh(mesh);
            gfx::DestroyTexture(texture);
        }
        EXPECT_EQ(gfx::detail::g.prog0_guarded_zero_alpha_pipeline, VK_NULL_HANDLE);
        EXPECT_EQ(gfx::detail::g.prog0_mode1_fog_base, UINT32_MAX);
    }
}

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
    const auto safe = [&] {
        const bool result = gfx::detail::Prog0FogIsNearSafe(mesh, draw.mesh, draw.prog0);
        EXPECT_EQ(gfx::detail::ExplainProg0Fog(mesh, draw.mesh, draw.prog0).reason == gfx::detail::Prog0FogReason::Safe, result);
        return result;
    };
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

/**
 * Reports first failures without changing the original finite or near-plane proof results.
 */
TEST(GfxMeshDc2, Prog0FogExplanationsIdentifyFirstFailureAndAvailableStages) {
    using Reason = gfx::detail::Prog0FogReason;

    /**
     * Defines an independently expected first rejection for one changed proof input.
     */
    struct Case {
        Reason                                                reason; /**< Expected first rejection. */
        std::function<void(gfx::detail::Mesh &, Prog0Draw &)> change; /**< Single input perturbation. */
    };

    const std::array cases{
        Case{Reason::Safe,               [](auto &,     auto &) {}                                                                  },
        Case{Reason::UnknownBounds,      [](auto &mesh, auto &) { mesh.bounds_known = false; }                                      },
        Case{Reason::NonnormalPositions, [](auto &mesh, auto &) { mesh.bounds_normal_inputs = false; }                              },
        Case{Reason::SkinnedFlag,        [](auto &,     auto &draw) { draw.prog0.flags |= gfx::kProg0Skinned; }                     },
        Case{Reason::ZeroFogScale,       [](auto &,     auto &draw) { draw.prog0.fog[3] = -0.0f; }                                  },
        Case{Reason::InvalidBounds,      [](auto &mesh, auto &) { mesh.bounds_min[0] = 4; }                                         },
        Case{Reason::InvalidBounds,      [](auto &mesh, auto &) { mesh.bounds_min[0] = NAN; }                                       },
        Case{Reason::NonnormalMvp,       [](auto &,     auto &draw) { draw.mesh.mvp[0] = INFINITY; }                                },
        Case{Reason::NonnormalMvp,       [](auto &,     auto &draw) { draw.mesh.mvp[0] = std::numeric_limits<float>::denorm_min(); }},
        Case{Reason::NonnormalFog,       [](auto &,     auto &draw) { draw.prog0.fog[1] = NAN; }                                    },
        Case{Reason::ClipOverflow,       [](auto &mesh, auto &draw) { mesh.bounds_max[0] = std::numeric_limits<float>::max(); draw.mesh.mvp[0] = std::numeric_limits<float>::max(); }                                                             },
        Case{Reason::WMargin,            [](auto &,     auto &draw) { draw.mesh.mvp[15] = 0; }                                      },
        Case{Reason::NearMargin,         [](auto &,     auto &draw) { draw.mesh.mvp[14] = 1; }                                      },
        Case{Reason::FogOverflow,        [](auto &,     auto &draw) { draw.prog0.fog[0] = std::numeric_limits<float>::max(); }      }
    };
    for (const auto &test : cases) {
        SCOPED_TRACE(static_cast<int>(test.reason));
        gfx::detail::Mesh mesh;
        mesh.bounds_known = mesh.bounds_normal_inputs = true;
        mesh.bounds_min = {-1, -1, 2};
        mesh.bounds_max = {3, 3, 6};
        auto draw = Unlit(gfx::kProg0Unlit);
        SetWMvp(draw.mesh.mvp, 1);
        Set(draw.prog0.fog, {180, 255, 0, -60});
        test.change(mesh, draw);
        const auto report = gfx::detail::ExplainProg0Fog(mesh, draw.mesh, draw.prog0);
        EXPECT_EQ(report.reason, test.reason);
        EXPECT_EQ(report.reason == Reason::Safe, gfx::detail::Prog0FogIsNearSafe(mesh, draw.mesh, draw.prog0));
        const bool clip = test.reason == Reason::Safe || test.reason == Reason::ClipOverflow || test.reason == Reason::WMargin || test.reason == Reason::NearMargin || test.reason == Reason::FogOverflow;
        EXPECT_EQ(report.clip_computed, clip);
        EXPECT_EQ(report.margins_computed, clip && test.reason != Reason::ClipOverflow);
        EXPECT_EQ(report.fog_computed, test.reason == Reason::Safe || test.reason == Reason::FogOverflow);
    }
}

/**
 * Preserves exact counts and by-value late observations when representative bounds are exceeded.
 */
TEST(GfxMeshDc2, Prog0FogObservationsBoundRepresentativesWithoutLosingCounts) {
    using Reason = gfx::detail::Prog0FogReason;
    gfx::detail::Prog0FogObservations frame;
    frame.capacity = 2;
    gfx::detail::Prog0FogObservation sample;
    sample.mesh = 0x10001;
    sample.generation = 1;
    sample.index_count = 3;
    sample.proof.reason = Reason::WMargin;
    sample.proof.margins_computed = true;
    sample.proof.w_min = -1;
    sample.first_frame = sample.last_frame = sample.representative_frame = 1;
    frame.Add(sample);
    sample.first_frame = sample.last_frame = sample.representative_frame = 9;
    sample.mvp[0] = 9;
    sample.proof.w_min = -4;
    frame.Add(sample);
    ASSERT_EQ(frame.records.size(), 1u);
    EXPECT_EQ(frame.records[0].draws, 2u);
    EXPECT_EQ(frame.records[0].first_frame, 1u);
    EXPECT_EQ(frame.records[0].last_frame, 9u);
    EXPECT_EQ(frame.records[0].mvp[0], 9);
    EXPECT_EQ(frame.records[0].representative_frame, 9u);
    sample.first_frame = sample.last_frame = sample.representative_frame = 12;
    sample.proof.w_min = -2;
    frame.Add(sample);
    EXPECT_EQ(frame.records[0].last_frame, 12u);
    EXPECT_EQ(frame.records[0].representative_frame, 9u);
    sample.mesh = 0x10002;
    frame.Add(sample);
    sample.generation = 2;
    frame.Add(sample);
    sample.generation = 1;
    sample.first_index = 3;
    frame.Add(sample);
    EXPECT_EQ(frame.counts[static_cast<size_t>(Reason::WMargin)], 6u);
    EXPECT_EQ(frame.dropped_draws, 2u);
    gfx::detail::Prog0FogObservations bucket;
    bucket.capacity = 1;
    bucket.Merge(frame);
    EXPECT_EQ(bucket.counts[static_cast<size_t>(Reason::WMargin)], 6u);
    EXPECT_EQ(bucket.dropped_draws, 3u);
    ASSERT_EQ(bucket.records.size(), 1u);
    EXPECT_EQ(bucket.records[0].draws, 3u);
    const auto capacity = bucket.records.capacity();
    bucket.Clear();
    EXPECT_EQ(bucket.records.capacity(), capacity);
    EXPECT_EQ(bucket.dropped_draws, 0u);
    sample.mesh = 0x10009;
    sample.first_frame = sample.last_frame = 100;
    sample.proof.reason = Reason::NearMargin;
    bucket.Add(sample);
    ASSERT_EQ(bucket.records.size(), 1u);
    EXPECT_EQ(bucket.records[0].mesh, 0x10009u);
    EXPECT_EQ(bucket.records[0].first_frame, 100u);
    EXPECT_EQ(bucket.counts[static_cast<size_t>(Reason::WMargin)], 0u);
    sample.mvp[0] = -999;
    EXPECT_EQ(bucket.records[0].mvp[0], 9);
}

/**
 * Rejects every layout that could differ from the exact mode1 vertex fog contract.
 */
TEST(GfxMeshDc2, Prog0Mode1FogRequiresExactNormalImmutableLayout) {
    gfx::detail::Mesh mesh;
    mesh.bounds_known = mesh.bounds_normal_inputs = true;
    auto draw = Unlit(gfx::kProg0Unlit);
    Set(draw.prog0.fog, {0, 255, 0, 255});
    const auto eligible = [&] { return gfx::detail::Prog0FogHasExactMode1(mesh, draw.prog0); };
    ASSERT_TRUE(eligible());
    for (float scale : {255.0f, -255.0f, std::numeric_limits<float>::min(), -std::numeric_limits<float>::min()}) {
        draw.prog0.fog[3] = scale;
        EXPECT_TRUE(eligible());
    }
    for (float scale : {0.0f, -0.0f, std::numeric_limits<float>::denorm_min(), -std::numeric_limits<float>::denorm_min(), INFINITY, -INFINITY, NAN}) {
        draw.prog0.fog[3] = scale;
        EXPECT_FALSE(eligible());
    }
    draw.prog0.fog[3] = 255;
    for (float offset : {0.0f, -0.0f, -153.0f, 300.0f}) {
        draw.prog0.fog[0] = offset;
        EXPECT_TRUE(eligible());
    }
    for (float offset : {std::numeric_limits<float>::denorm_min(), INFINITY, -INFINITY, NAN}) {
        draw.prog0.fog[0] = offset;
        EXPECT_FALSE(eligible());
    }
    draw.prog0.fog[0] = 0;
    for (float limit : {254.0f, std::nextafter(255.0f, 256.0f), INFINITY, NAN}) {
        draw.prog0.fog[1] = limit;
        EXPECT_FALSE(eligible());
    }
    draw.prog0.fog[1] = 255;
    for (float limit : {-0.0f, 1.0f, std::numeric_limits<float>::denorm_min(), INFINITY, NAN}) {
        draw.prog0.fog[2] = limit;
        EXPECT_FALSE(eligible());
    }
    draw.prog0.fog[2] = 0;
    mesh.bounds_known = false;
    EXPECT_FALSE(eligible());
    mesh.bounds_known = true;
    mesh.bounds_normal_inputs = false;
    EXPECT_FALSE(eligible());
    mesh.bounds_normal_inputs = true;
    draw.prog0.flags |= gfx::kProg0Skinned;
    EXPECT_FALSE(eligible());
}

/**
 * Matches exact mode1 pixels against an independent generic renderer across clipping and side effects.
 */
TEST(GfxGpuMeshDc2, Prog0Mode1FogMatchesGenericRendererForCrossingsAndFreshThresholds) {
    /**
     * Restores the caller's Prog0 fog policy after independent renderer lifetimes.
     */
    struct SettingRestore {
        EnvSnapshot original = SaveEnv("KZ_PROG0_FOG_NEAR_SAFE"); /**< Original optimization override. */
        EnvSnapshot parent = SaveEnv("KZ_FOG_NEAR_SAFE");         /**< Parent fog policy restored after the test. */

        /**
         * Restores the process policy after either fixture succeeds or returns early.
         */
        ~SettingRestore() {
            RestoreEnv(original);
            RestoreEnv(parent);
        }
    } restore;

    ASSERT_EQ(SetEnv("KZ_FOG_NEAR_SAFE", "1", 1), 0);
    /**
     * Specifies the pipeline independently expected for one complete scene.
     */
    enum class Selection {
        Generic,
        NearSafe,
        Mode1
    };

    /**
     * Owns actual geometry, clip arguments and observable blend/depth/stencil conditions.
     */
    struct Case {
        std::string                name;                         /**< Independent case description. */
        std::vector<gfx::Vertex3D> vertices;                     /**< Actual uploaded model positions and attributes. */
        std::vector<uint32_t>      indices;                      /**< Full triangle-list topology. */
        std::array<float, 16>      mvp;                          /**< Actual matrix supplied to the vertex shader. */
        std::array<float, 4>       fog = {0, 255, 0, 255};       /**< Exact source fog arguments. */
        std::array<uint8_t, 3>     fog_color = {23, 47, 79};     /**< Exact RGB bytes used by push-constant packing. */
        bool                       black_texture = false;        /**< Uses black RGB with fully opaque sampled alpha. */
        bool                       rgb_expected = true;          /**< Requires visible RGB unless this case explicitly witnesses alpha-only coverage. */
        Selection                  selection = Selection::Mode1; /**< Independently required pipeline. */
        bool                       updated = false;              /**< Invalidates immutable geometry before rendering. */
        bool                       alpha_test = false;           /**< Enables a deliberately unsupported alpha-test variant. */
        bool                       fog_enabled = true;           /**< Enables the targeted fog feature. */
        bool                       source_over = false;          /**< Uses the unsupported source-over blend equation. */
        bool                       flat = false;                 /**< Removes RGBA sampling from the target pipeline. */
        bool                       skinned = false;              /**< Uses a real packed skin stream and identity palette. */
        bool                       layered = false;              /**< Adds overlap, an occluder and a later dependent consumer. */
        bool                       depth_write = false;          /**< Preserves effect depth writes for the later consumer. */
        bool                       stencil = false;              /**< Preserves effect stencil writes for the later consumer. */
        float                      alpha = 1;                    /**< Object alpha multiplier, including invisible and over-one values. */
    };

    auto synthetic = Unlit(gfx::kProg0Unlit);
    SetWMvp(synthetic.mesh.mvp, 1);
    synthetic.mesh.mvp[2] = .75f;
    Case basic;
    basic.vertices = ScreenVertices({0, 0, 1}, {173, 113, 197, 128});
    basic.vertices[1].uv[0] = basic.vertices[2].uv[1] = 3.7f;
    basic.indices = kTriangle;
    std::copy_n(synthetic.mesh.mvp, basic.mvp.size(), basic.mvp.begin());
    std::vector<Case> cases;
    /**
     * Appends a fully owned variant without sharing mutable fixture state.
     */
    const auto append = [&](std::string name, const std::function<void(Case &)> &change) {
        Case test = basic;
        test.name = std::move(name);
        change(test);
        cases.push_back(std::move(test));
    };
    append("fresh below255", [](Case &test) { test.fog[3] = std::nextafter(255.0f, 0.0f); });
    append("fresh at255", [](Case &) {});
    append("fresh above255", [](Case &test) { test.fog[3] = std::nextafter(255.0f, INFINITY); });
    append("negative scale", [](Case &test) { test.fog = {300, 255, 0, -60}; });
    append("negative fresh", [](Case &test) { test.fog = {0, 255, 0, -300}; });
    append("near-safe retains priority", [](Case &test) { test.mvp[2] = 0; test.selection = Selection::NearSafe; });
    append("updated geometry", [](Case &test) { test.updated = true; test.selection = Selection::Generic; });
    append("alpha test", [](Case &test) { test.alpha_test = true; test.selection = Selection::Generic; });
    append("source-over", [](Case &test) { test.source_over = true; test.selection = Selection::Generic; });
    append("no fog", [](Case &test) { test.fog_enabled = false; test.selection = Selection::Generic; });
    append("untextured", [](Case &test) { test.flat = true; test.selection = Selection::Generic; });
    append("skinned stream", [](Case &test) { test.skinned = true; test.selection = Selection::Generic; });
    append("constant fog", [](Case &test) { test.fog = {127, 255, 0, 0}; test.selection = Selection::Generic; });
    append("subnormal scale", [](Case &test) { test.fog[3] = std::numeric_limits<float>::denorm_min(); test.selection = Selection::Generic; });
    append("different upper limit", [](Case &test) { test.fog[1] = 254; test.selection = Selection::Generic; });
    append("negative-zero lower limit", [](Case &test) { test.fog[2] = -0.0f; test.selection = Selection::Generic; });
    for (float first_w : {-1.0f, 0.0f, .0001f}) {
        append("eye vertex W=" + std::to_string(first_w), [first_w](Case &test) {
            test.vertices[0].position[2] = first_w;
            test.vertices[1].position[2] = 2;
            test.vertices[2].position[2] = 4;
            test.mvp.fill(0);
            test.mvp[0] = test.mvp[5] = test.mvp[11] = 1;
            test.mvp[14] = .5f;
        });
    }
    const std::array grid_positions{
        std::array<uint32_t, 3>{0xc20b9454u, 0x00000000u, 0xc20b9454u},
        std::array<uint32_t, 3>{0xc20b9454u, 0x00000000u, 0x420b9454u},
        std::array<uint32_t, 3>{0x420b9454u, 0x00000000u, 0x420b9454u},
        std::array<uint32_t, 3>{0xc20b9454u, 0x00000000u, 0xc20b9454u},
        std::array<uint32_t, 3>{0x420b9454u, 0x00000000u, 0x420b9454u},
        std::array<uint32_t, 3>{0x420b9454u, 0x00000000u, 0xc20b9454u}
    };
    const std::array cylinder_positions{
        std::array<uint32_t, 3>{0x41e17f32u, 0x00000000u, 0xc1169913u},
        std::array<uint32_t, 3>{0x4114c06fu, 0x40536552u, 0xc1bcd65au},
        std::array<uint32_t, 3>{0x41d3eee8u, 0x40536552u, 0xc10d8a0eu},
        std::array<uint32_t, 3>{0xc1e17f32u, 0x00000000u, 0x41169913u},
        std::array<uint32_t, 3>{0xc114c075u, 0x40536552u, 0x41bcd663u},
        std::array<uint32_t, 3>{0xc1d3eee8u, 0x40536552u, 0x410d8a0eu},
        std::array<uint32_t, 3>{0x41efc3bfu, 0x00000000u, 0x413cde45u},
        std::array<uint32_t, 3>{0x41e17f32u, 0x00000000u, 0xc1169913u},
        std::array<uint32_t, 3>{0x41d3eee8u, 0x40536552u, 0xc10d8a0eu},
        std::array<uint32_t, 3>{0x41efc3bfu, 0x00000000u, 0x413cde45u},
        std::array<uint32_t, 3>{0x41d3eee8u, 0x40536552u, 0xc10d8a0eu},
        std::array<uint32_t, 3>{0x41e157c1u, 0x40536552u, 0x41318205u},
        std::array<uint32_t, 3>{0xc16329dfu, 0x00000000u, 0xc1d0d956u},
        std::array<uint32_t, 3>{0xc1efc3bfu, 0x00000000u, 0xc13cde45u},
        std::array<uint32_t, 3>{0xc1e157c1u, 0x40536552u, 0xc1318205u},
        std::array<uint32_t, 3>{0x416329dfu, 0x00000000u, 0x41d0d957u},
        std::array<uint32_t, 3>{0x41efc3bfu, 0x00000000u, 0x413cde45u},
        std::array<uint32_t, 3>{0x41e157c1u, 0x40536552u, 0x41318205u},
        std::array<uint32_t, 3>{0x41e17f32u, 0x00000000u, 0xc1169913u},
        std::array<uint32_t, 3>{0x411e4592u, 0x00000000u, 0xc1c8ec47u},
        std::array<uint32_t, 3>{0x4114c06fu, 0x40536552u, 0xc1bcd65au},
        std::array<uint32_t, 3>{0xc1557ff7u, 0x40536552u, 0xc1c44973u},
        std::array<uint32_t, 3>{0x411e4592u, 0x00000000u, 0xc1c8ec47u},
        std::array<uint32_t, 3>{0xc16329dfu, 0x00000000u, 0xc1d0d956u},
        std::array<uint32_t, 3>{0xc1557ff7u, 0x40536552u, 0xc1c44973u},
        std::array<uint32_t, 3>{0xc1e157c1u, 0x40536552u, 0xc1318205u},
        std::array<uint32_t, 3>{0xc1e17f32u, 0x00000000u, 0x41169913u},
        std::array<uint32_t, 3>{0xc11e4594u, 0x00000000u, 0x41c8ec4du},
        std::array<uint32_t, 3>{0xc114c075u, 0x40536552u, 0x41bcd663u},
        std::array<uint32_t, 3>{0x41557ffbu, 0x40536552u, 0x41c44974u},
        std::array<uint32_t, 3>{0xc11e4594u, 0x00000000u, 0x41c8ec4du},
        std::array<uint32_t, 3>{0x416329dfu, 0x00000000u, 0x41d0d957u},
        std::array<uint32_t, 3>{0x41557ffbu, 0x40536552u, 0x41c44974u},
        std::array<uint32_t, 3>{0x41e157c1u, 0x40536552u, 0x41318205u}
    };
    /**
     * Recreates exact rigid positions with varied linear-filtered texture coordinates.
     */
    const auto vertices_from_bits = [](const auto &positions) {
        std::vector<gfx::Vertex3D> vertices;
        for (size_t i = 0; i < positions.size(); ++i) {
            const auto &bits = positions[i];
            auto        vertex = Vert({std::bit_cast<float>(bits[0]), std::bit_cast<float>(bits[1]), std::bit_cast<float>(bits[2])}, {0, 1, 0}, {173, 113, 197, 128});
            vertex.uv[0] = static_cast<float>(i % 3) * .7f;
            vertex.uv[1] = static_cast<float>(i % 2) * .8f;
            vertices.push_back(vertex);
        }
        return vertices;
    };
    const std::array<uint32_t, 16> grid_clip_bits = {0x40999f4bu, 0x3f828813u, 0x3963ac52u, 0xc00af25eu, 0x313be252u, 0xc1341996u, 0x387905f7u, 0xbf17fa00u, 0xc0584aeau, 0x3fb96ba7u, 0x39a1b48bu, 0xc0455fd6u, 0xbea1ef9au, 0x41e22fabu, 0x409fbfc3u, 0x42a6d2e4u};
    const std::array<uint32_t, 16> broad_grid_clip_bits = {0x412ba64au, 0xc0484a53u, 0xba2eac59u, 0x40d533edu, 0x3fac180eu, 0xc1df9154u, 0x397a92dfu, 0xc018ec3au, 0x41248ab1u, 0x40dd62c0u, 0x3a2e0763u, 0xc0d46a94u, 0xbea1ef9au, 0xc279d00bu, 0x409fc3aeu, 0x429d4100u};
    const std::array<uint32_t, 16> cylinder_clip_bits = {0x4026dfdau, 0xbf927aabu, 0xb97f7d31u, 0x401bec2bu, 0x31134648u, 0xc10d2c43u, 0x384332f6u, 0xbeee419cu, 0x4072b7d6u, 0x3f496a70u, 0x392fa79cu, 0xbfd6669cu, 0xbea1ef9au, 0x41741580u, 0x409fc053u, 0x42a5735eu};
    for (const auto &bits : {grid_clip_bits, broad_grid_clip_bits}) {
        append("rigid crossing quad", [&](Case &test) {
            test.vertices = vertices_from_bits(grid_positions);
            test.indices = {0, 1, 2, 3, 4, 5};
            for (size_t i = 0; i < bits.size(); ++i) {
                test.mvp[i] = std::bit_cast<float>(bits[i]);
            }
            test.fog = {-153, 255, 0, 612000};
        });
    }
    append("rigid crossing cylinder", [&](Case &test) {
        test.vertices = vertices_from_bits(cylinder_positions);
        test.indices = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 20, 19, 21, 22, 23, 24, 24, 23, 25, 26, 27, 28, 28, 27, 29, 30, 31, 32, 32, 31, 33};
        for (size_t i = 0; i < cylinder_clip_bits.size(); ++i) {
            test.mvp[i] = std::bit_cast<float>(cylinder_clip_bits[i]);
        }
        test.fog = {-153, 255, 0, 612000};
    });
    for (float alpha : {0.0f, .0001f, .37f, 1.75f}) {
        for (bool depth : {false, true}) {
            for (bool stencil : {false, true}) {
                append("overlap alpha=" + std::to_string(alpha), [=](Case &test) {
                    test.layered = true;
                    test.fog = {128.25f, 255, 0, -.5f};
                    test.alpha = alpha;
                    test.depth_write = depth;
                    test.stencil = stencil;
                });
            }
        }
    }
    append("fractional fog", [](Case &test) {
        test.vertices[0].position[2] = 2;
        test.vertices[1].position[2] = 4;
        test.vertices[2].position[2] = 6;
        test.mvp.fill(0);
        test.mvp[0] = test.mvp[5] = test.mvp[11] = 1;
        test.mvp[10] = .5f;
        test.mvp[14] = 1.5f;
        test.fog = {180.25f, 255, 0, -60.5f};
    });
    const auto colored_cases = std::move(cases);
    for (const auto &original : colored_cases) {
        cases.push_back(original);
        auto black = original;
        black.name += " black fog";
        black.fog_color = {0, 0, 0};
        if (original.name == "negative fresh" || original.name == "subnormal scale") {
            black.rgb_expected = false;
        }
        cases.push_back(std::move(black));
        if (original.layered) {
            auto near = original;
            near.name += " near-safe";
            near.mvp[2] = 0;
            near.selection = Selection::NearSafe;
            near.rgb_expected = !(near.depth_write && near.alpha <= .0001f);
            cases.push_back(near);
            near.name += " black fog";
            near.fog_color = {0, 0, 0};
            cases.push_back(std::move(near));
        }
        if (original.name == "near-safe retains priority" || original.name == "fresh at255" || original.name == "rigid crossing quad" || original.name == "fractional fog") {
            for (const std::array<uint8_t, 3> color : {
                     std::array<uint8_t, 3>{1, 0, 0},
                     {0, 1, 0},
                     {0, 0, 1}
            }) {
                auto near_black = original;
                near_black.name += " one-byte fog";
                near_black.fog_color = color;
                cases.push_back(std::move(near_black));
            }
        }
    }
    for (bool near_safe : {false, true}) {
        Case black_texel = basic;
        black_texel.name = "black texel retains alpha";
        black_texel.fog_color = {0, 0, 0};
        black_texel.black_texture = true;
        black_texel.alpha = .37f;
        if (near_safe) {
            black_texel.mvp[2] = 0;
            black_texel.selection = Selection::NearSafe;
        }
        cases.push_back(std::move(black_texel));
    }
    std::vector<std::vector<uint8_t>> reference;
    std::vector<uint32_t>             reference_draws;
    const std::array<uint8_t, 4>      clear = {17, 29, 43, 71};
    for (bool optimized : {false, true}) {
        ASSERT_EQ(SetEnv("KZ_PROG0_FOG_NEAR_SAFE", optimized ? "1" : "0", 1), 0);
        GfxFixture fixture(kWidth, kHeight, 1.0f, Prog0Options());
        ASSERT_EQ(gfx::detail::g.prog0_mode1_fog_pipeline != VK_NULL_HANDLE, optimized);
        const auto       texture = gfx::CreateTexture({2, 2, gfx::TextureFormat::Rgba8, 1, true});
        const std::array texels = {Rgba(231, 73, 119, 0), Rgba(61, 193, 107, 1), Rgba(83, 127, 223, 127), Rgba(199, 167, 47, 255)};
        ASSERT_TRUE(gfx::UpdateTexture(texture, 0, 0, 0, 2, 2, texels.data()));
        auto binding = NearestBinding(texture);
        binding.filter = gfx::Filter::Linear;
        const auto     black_texture = gfx::CreateTexture({1, 1, gfx::TextureFormat::Rgba8, 1, true});
        const uint32_t black_texel = Rgba(0, 0, 0, 255);
        ASSERT_TRUE(gfx::UpdateTexture(black_texture, 0, 0, 0, 1, 1, &black_texel));
        auto black_binding = NearestBinding(black_texture);
        black_binding.filter = gfx::Filter::Linear;
        for (size_t i = 0; i < cases.size(); ++i) {
            const auto &test = cases[i];
            SCOPED_TRACE(test.name);
            SCOPED_TRACE(optimized);
            const auto vertices = test.skinned ? Skinned(test.vertices, {1, 0, 0, 0}, {0, 0, 0, 0}) : test.vertices;
            const auto mesh = gfx::CreateMesh(vertices, test.indices);
            ASSERT_NE(mesh, gfx::kNullMesh);
            if (test.updated) {
                ASSERT_TRUE(gfx::UpdateMeshVertices(mesh, 0, vertices));
            }
            auto draw = Unlit(gfx::kProg0Unlit | (test.skinned ? gfx::kProg0Skinned : 0));
            std::copy(test.mvp.begin(), test.mvp.end(), draw.mesh.mvp);
            std::copy(test.fog.begin(), test.fog.end(), draw.prog0.fog);
            draw.prog0.diffuse[3] = test.alpha;
            if (test.skinned) {
                for (uint32_t bone = 0; bone < gfx::kProg0Bones; ++bone) {
                    SetBone(draw.palette, bone, kIdentityBone);
                }
            }
            fixture.Frame(clear, [&] {
                gfx::ClearStencil(0);
                if (test.layered) {
                    gfx::DrawState occluder;
                    occluder.depth_write = true;
                    occluder.color_write_mask = 0;
                    const auto cover = Quad(0, 0, gfx::LogicalWidth() / 4, gfx::LogicalHeight(), {0, 0, 0, 128}, 0, 0, 0, 0, .8f);
                    gfx::Draw2D(gfx::Primitive::Quads, cover, {}, occluder);
                }
                gfx::DrawState state;
                state.blend = true;
                state.alpha = {0, static_cast<uint8_t>(test.source_over ? 1 : 2), 0, 1, 0};
                state.fog = test.fog_enabled;
                std::copy(test.fog_color.begin(), test.fog_color.end(), state.fog_color);
                state.alpha_test = test.alpha_test;
                state.alpha_func = gfx::AlphaFunc::GEqual;
                state.alpha_ref = 1;
                state.depth_test = test.layered ? gfx::DepthTest::GEqual : gfx::DepthTest::Always;
                state.depth_write = test.depth_write;
                state.stencil_test = test.stencil;
                state.stencil_front.pass = gfx::StencilOp::Replace;
                state.stencil_front.reference = 7;
                state.stencil_back = state.stencil_front;
                const uint32_t count = test.layered ? 2 : 1;
                const auto     near_before = gfx::detail::g.counters.near_safe_prog0_fog;
                const auto     mode_before = gfx::detail::g.counters.mode1_prog0_fog;
                const auto     black_before = gfx::detail::g.counters.black_prog0_fog;
                for (uint32_t layer = 0; layer < count; ++layer) {
                    gfx::DrawMeshProg0(mesh, 0, static_cast<uint32_t>(test.indices.size()), draw.mesh, draw.prog0, test.flat ? gfx::TextureBinding{} : test.black_texture ? black_binding
                                                                                                                                                                          : binding,
                                       state, nullptr, test.skinned ? &draw.palette : nullptr);
                }
                EXPECT_EQ(gfx::detail::g.counters.near_safe_prog0_fog - near_before, optimized && test.selection == Selection::NearSafe ? count : 0u);
                EXPECT_EQ(gfx::detail::g.counters.mode1_prog0_fog - mode_before, optimized && test.selection == Selection::Mode1 ? count : 0u);
                const bool black = (test.fog_color[0] | test.fog_color[1] | test.fog_color[2]) == 0;
                EXPECT_EQ(gfx::detail::g.counters.black_prog0_fog - black_before, optimized && black && test.selection != Selection::Generic ? count : 0u);
                if (optimized && test.selection == Selection::Mode1) {
                    EXPECT_EQ(gfx::detail::g.bound.pipeline, black ? gfx::detail::g.prog0_mode1_black_fog_pipeline : gfx::detail::g.prog0_mode1_fog_pipeline);
                }
                if (optimized && test.selection == Selection::NearSafe) {
                    EXPECT_EQ(gfx::detail::g.bound.pipeline, black ? gfx::detail::g.prog0_near_safe_black_fog_pipeline : gfx::detail::g.prog0_near_safe_fog_pipelines[2]);
                }
                if (test.layered) {
                    gfx::DrawState consumer;
                    consumer.depth_test = gfx::DepthTest::GEqual;
                    consumer.stencil_test = test.stencil;
                    consumer.stencil_front.compare = gfx::CompareOp::Equal;
                    consumer.stencil_front.reference = 7;
                    consumer.stencil_back = consumer.stencil_front;
                    const auto blue = Quad(0, 0, gfx::LogicalWidth(), gfx::LogicalHeight(), {0, 0, 128, 128}, 0, 0, 0, 0, .25f);
                    gfx::Draw2D(gfx::Primitive::Quads, blue, {}, consumer);
                }
            });
            if (!optimized) {
                reference.push_back(fixture.pixels);
                reference_draws.push_back(gfx::detail::g.counters.mesh_draws);
            } else {
                EXPECT_EQ(fixture.pixels, reference[i]);
                EXPECT_EQ(gfx::detail::g.counters.mesh_draws, reference_draws[i]);
            }
            size_t changed = 0;
            for (size_t pixel = 0; pixel + 3 < fixture.pixels.size(); pixel += 4) {
                changed += fixture.pixels[pixel] != clear[0] || fixture.pixels[pixel + 1] != clear[1] || fixture.pixels[pixel + 2] != clear[2] ? 1u : 0u;
            }
            if (test.black_texture || !test.rgb_expected) {
                EXPECT_EQ(changed, 0u);
                size_t changed_alpha = 0;
                for (size_t pixel = 3; pixel < fixture.pixels.size(); pixel += 4) {
                    changed_alpha += fixture.pixels[pixel] != clear[3] ? 1u : 0u;
                }
                EXPECT_GT(changed_alpha, 0u);
            } else {
                EXPECT_GT(changed, 0u);
            }
            if (test.layered && !test.depth_write) {
                EXPECT_EQ(fixture.Pixel(kWidth / 2, kHeight / 2)[2], 128);
            }
            gfx::DestroyMesh(mesh);
        }
        gfx::DestroyTexture(black_texture);
        gfx::DestroyTexture(texture);
    }
}

/**
 * Follows changing black, near-black, colored and alpha state through actual interpolated replay.
 */
TEST(GfxGpuMeshDc2, Prog0BlackFogInputFollowsRecordedStateAndInterpolation) {
    /**
     * Restores both process policies after the independent generic and optimized lifetimes.
     */
    struct SettingRestore {
        EnvSnapshot parent = SaveEnv("KZ_FOG_NEAR_SAFE");      /**< Caller parent policy. */
        EnvSnapshot prog0 = SaveEnv("KZ_PROG0_FOG_NEAR_SAFE"); /**< Caller Prog0 override. */

        /**
         * Restores policies after either renderer lifetime completes or fails.
         */
        ~SettingRestore() {
            RestoreEnv(prog0);
            RestoreEnv(parent);
        }
    } restore;

    ASSERT_EQ(SetEnv("KZ_FOG_NEAR_SAFE", "1", 1), 0);
    const std::array<std::array<uint8_t, 3>, 4> colors{
        {{0, 0, 0}, {0, 1, 0}, {23, 47, 79}, {0, 0, 0}}
    };
    const std::array                  alphas{0.0f, .0001f, .37f, 1.75f};
    const std::array<uint8_t, 4>      clear = {17, 29, 43, 71};
    std::vector<std::vector<uint8_t>> reference;
    std::vector<uint32_t>             reference_draws;
    for (bool optimized : {false, true}) {
        ASSERT_EQ(SetEnv("KZ_PROG0_FOG_NEAR_SAFE", optimized ? "1" : "0", 1), 0);
        GfxFixture fixture(kWidth, kHeight, 1.0f, Prog0Options());
        auto       vertices = ScreenVertices({0, 0, 1}, {173, 113, 197, 128});
        vertices[0].position[2] = 2;
        vertices[1].position[2] = 4;
        vertices[2].position[2] = 6;
        vertices[1].uv[0] = vertices[2].uv[1] = 3.7f;
        const auto mesh = gfx::CreateMesh(vertices, kTriangle);
        ASSERT_NE(mesh, gfx::kNullMesh);
        if (!optimized) {
            ASSERT_TRUE(gfx::UpdateMeshVertices(mesh, 0, vertices));
        }
        const auto       texture = gfx::CreateTexture({2, 2, gfx::TextureFormat::Rgba8, 1, true});
        const std::array texels{Rgba(0, 0, 0, 255), Rgba(197, 37, 113, 1), Rgba(59, 211, 131, 127), Rgba(239, 149, 67, 255)};
        ASSERT_TRUE(gfx::UpdateTexture(texture, 0, 0, 0, 2, 2, texels.data()));
        auto binding = NearestBinding(texture);
        binding.filter = gfx::Filter::Linear;
        size_t result = 0;
        for (bool current_safe : {false, true}) {
            SCOPED_TRACE(current_safe);
            /**
             * Records four independently keyed surfaces whose push colors and material alpha vary per draw.
             */
            const auto record = [&](bool current) {
                auto transform = gfx::IdentityMeshTransform();
                transform.model[14] = current == current_safe ? 4.0f : 0.0f;
                std::fill_n(transform.projection, 16, 0);
                transform.projection[0] = transform.projection[5] = transform.projection[11] = 1;
                transform.projection[10] = .5f;
                transform.projection[14] = 1.5f;
                gfx::detail::Mat4 projection;
                gfx::detail::Mat4 model;
                std::memcpy(projection.data(), transform.projection, sizeof projection);
                std::memcpy(model.data(), transform.model, sizeof model);
                const auto mvp = gfx::detail::Multiply(projection, model);
                gfx::BeginRecording();
                gfx::Clear(true, clear.data(), true, 0);
                for (size_t layer = 0; layer < colors.size(); ++layer) {
                    auto draw = Unlit(gfx::kProg0Unlit);
                    Set(draw.prog0.fog, {180.25f, 255, 0, -60.5f});
                    std::memcpy(draw.mesh.mvp, mvp.data(), sizeof draw.mesh.mvp);
                    draw.prog0.diffuse[3] = current ? alphas[layer] : alphas[alphas.size() - 1 - layer];
                    gfx::DrawState state;
                    state.blend = state.fog = true;
                    state.alpha = {0, 2, 0, 1, 0};
                    std::copy(colors[layer].begin(), colors[layer].end(), state.fog_color);
                    gfx::SetInterpKey(900 + layer);
                    gfx::DrawMeshProg0(mesh, 0, 3, draw.mesh, draw.prog0, binding, state, &transform);
                }
                return gfx::EndRecording();
            };
            const auto previous = record(false);
            const auto current = record(true);
            ASSERT_NE(previous, nullptr);
            ASSERT_NE(current, nullptr);
            for (float alpha : {.1f, .5f, .9f}) {
                SCOPED_TRACE(alpha);
                ASSERT_TRUE(gfx::RenderList(*current, 1, {.canonical = true}));
                const auto constant_before = gfx::detail::g.prog0_zero_source_alpha_selected;
                const auto guarded_before = gfx::detail::g.prog0_guarded_zero_alpha_selected;
                ASSERT_TRUE(gfx::RenderList(*current, alpha, {.previous = previous.get(), .present = true}));
                ASSERT_TRUE(gfx::ReadbackFrame(fixture.pixels, fixture.width, fixture.height));
                const float translation = current_safe ? 4 * alpha : 4 * (1 - alpha);
                const bool  near_safe = translation > 1;
                EXPECT_EQ(gfx::detail::g.counters.near_safe_prog0_fog, optimized && near_safe ? 2u : 0u);
                EXPECT_EQ(gfx::detail::g.counters.mode1_prog0_fog, optimized && !near_safe ? 4u : 0u);
                EXPECT_EQ(gfx::detail::g.counters.black_prog0_fog, optimized ? near_safe ? 1u : 2u : 0u);
                EXPECT_EQ(gfx::detail::g.prog0_zero_source_alpha_selected - constant_before, optimized && near_safe ? 2u : 0u);
                EXPECT_EQ(gfx::detail::g.prog0_guarded_zero_alpha_selected - guarded_before, optimized && !near_safe ? 1u : 0u);
                if (optimized) {
                    EXPECT_EQ(gfx::detail::g.bound.pipeline, near_safe ? gfx::detail::g.prog0_near_safe_black_fog_pipeline : gfx::detail::g.prog0_mode1_black_fog_pipeline);
                    EXPECT_EQ(fixture.pixels, reference[result]);
                    EXPECT_EQ(gfx::detail::g.counters.mesh_draws, reference_draws[result]);
                } else {
                    reference.push_back(fixture.pixels);
                    reference_draws.push_back(gfx::detail::g.counters.mesh_draws);
                }
                size_t changed = 0;
                for (size_t pixel = 0; pixel + 3 < fixture.pixels.size(); pixel += 4) {
                    changed += fixture.pixels[pixel] != clear[0] || fixture.pixels[pixel + 1] != clear[1] || fixture.pixels[pixel + 2] != clear[2] ? 1u : 0u;
                }
                EXPECT_GT(changed, 0u);
                ++result;
            }
        }
        gfx::DestroyTexture(texture);
        gfx::DestroyMesh(mesh);
    }
}

/**
 * Preserves bounded result totals, peak snapshots and explicit missing-data accounting.
 */
TEST(GfxMeshDc2, FragmentInvocationSamplesPreserveCountsAcrossBoundsAndErrors) {
    gfx::detail::FragmentInvocationSamples samples;
    samples.capacity = 1;
    gfx::detail::FragmentInvocationObservation first;
    first.scope = gfx::detail::kProg0NearSafeFogAdditiveBlackScope;
    first.mesh = 17;
    first.generation = 2;
    first.available = 1;
    first.invocations = first.peak_invocations = 13;
    first.frame = 3;
    samples.Add(first);
    auto later = first;
    later.frame = 8;
    later.mvp[12] = 5.0f;
    later.invocations = later.peak_invocations = 29;
    samples.Add(later);
    ASSERT_EQ(samples.records.size(), 1u);
    EXPECT_EQ(samples.records[0].frame, 8u);
    EXPECT_EQ(samples.records[0].mvp[12], 5.0f);
    EXPECT_EQ(samples.records[0].draws, 2u);
    EXPECT_EQ(samples.records[0].invocations, 42u);
    auto recycled = first;
    ++recycled.generation;
    samples.Add(recycled);
    auto error = first;
    error.scope = gfx::detail::kProg0Mode1FogBlackScope;
    error.available = error.invocations = error.peak_invocations = 0;
    error.unavailable = error.errors = 1;
    samples.Add(error);
    auto unavailable = error;
    unavailable.errors = 0;
    samples.Add(unavailable);
    samples.Skip({2, 3});
    EXPECT_EQ(samples.draws, (std::array<uint64_t, 2>{5, 5}));
    EXPECT_EQ(samples.available, (std::array<uint64_t, 2>{3, 0}));
    EXPECT_EQ(samples.invocations, (std::array<uint64_t, 2>{55, 0}));
    EXPECT_EQ(samples.unavailable[1], 2u);
    EXPECT_EQ(samples.errors[1], 1u);
    EXPECT_EQ(samples.skipped, (std::array<uint64_t, 2>{2, 3}));
    EXPECT_EQ(samples.truncated_draws, 3u);
    const auto capacity = samples.records.capacity();
    samples.Clear();
    EXPECT_EQ(samples.records.capacity(), capacity);
    EXPECT_TRUE(samples.records.empty());
    EXPECT_EQ(samples.draws, (std::array<uint64_t, 2>{0, 0}));
    first.scope = gfx::detail::kProg0Mode1FogScope;
    samples.Add(first);
    EXPECT_TRUE(samples.records.empty());
    EXPECT_EQ(gfx::detail::FragmentInvocationScopeIndex(gfx::detail::kProg0NearSafeFogAdditiveScope), -1);
    EXPECT_EQ(gfx::detail::FragmentInvocationScopeIndex(UINT32_MAX), -1);
}

/**
 * Preserves ordered ownership and atomic same-submission pair evidence across bounded aggregation.
 */
TEST(GfxMeshDc2, FragmentDrawPairsKeepOrderedStatesAndAtomicSubmissions) {
    gfx::detail::FragmentDrawPairSlot          slot;
    gfx::detail::FragmentInvocationObservation first;
    first.mesh = 17;
    first.generation = 1;
    first.index_count = 6;
    first.scope = gfx::detail::kProg0NearSafeFogAdditiveBlackScope;
    first.snapshot = std::make_shared<gfx::detail::FragmentDrawSnapshot>();
    first.snapshot->origin = {.present = true, .list_serial = 11, .key = 101, .occurrence = 0};
    first.snapshot->mesh_valid = first.snapshot->prog0_valid = true;
    first.snapshot->mesh.mvp[0] = 1;
    first.snapshot->mesh.mvp[3] = -0.0f;
    auto second = first;
    second.snapshot = std::make_shared<gfx::detail::FragmentDrawSnapshot>(*first.snapshot);
    second.scope = gfx::detail::kProg0Mode1FogBlackScope;
    second.texture = 77;
    second.target = 2;
    second.snapshot->origin.key = 202;
    second.snapshot->origin.occurrence = 1;
    second.snapshot->mesh.mvp[12] = 5;
    second.snapshot->prog0.diffuse[0] = .5f;
    second.snapshot->push.mesh_index = 9;
    second.snapshot->state.depth_write = true;
    second.snapshot->state.stencil_front.reference = 7;
    second.snapshot->bound.scissor.extent = {4, 5};
    ASSERT_TRUE(slot.NeedsSnapshot(first));
    slot.Add(first, 0);
    ASSERT_TRUE(slot.NeedsSnapshot(second));
    slot.Add(second, 1);
    EXPECT_FALSE(slot.NeedsSnapshot(first));
    slot.Add(first, UINT32_MAX);
    ASSERT_EQ(slot.records.size(), 1u);
    auto pair = slot.records[0];
    EXPECT_EQ(pair.occurrences, 3u);
    EXPECT_EQ(pair.queried_occurrences, 2u);
    EXPECT_EQ(pair.members[0].snapshot->origin.key, 101u);
    EXPECT_EQ(pair.members[1].snapshot->origin.key, 202u);
    EXPECT_EQ(pair.members[0].scope, gfx::detail::kProg0NearSafeFogAdditiveBlackScope);
    EXPECT_EQ(pair.members[1].scope, gfx::detail::kProg0Mode1FogBlackScope);
    EXPECT_EQ(pair.members[1].texture, 77u);
    EXPECT_EQ(pair.members[1].target, 2u);
    EXPECT_EQ(pair.members[1].snapshot->push.mesh_index, 9u);
    EXPECT_EQ(std::bit_cast<uint32_t>(pair.members[0].snapshot->mesh.mvp[3]), 0x80000000u);
    EXPECT_EQ(pair.members[0].snapshot->mesh.mvp[12], 0);
    EXPECT_EQ(pair.members[1].snapshot->mesh.mvp[12], 5);
    EXPECT_TRUE(pair.members[1].snapshot->state.depth_write);
    EXPECT_EQ(pair.members[1].snapshot->state.stencil_front.reference, 7u);
    EXPECT_EQ(pair.members[1].snapshot->bound.scissor.extent.width, 4u);
    gfx::detail::FragmentDrawPairSamples samples;
    auto                                 singleton = pair;
    singleton.occurrences = singleton.queried_occurrences = 1;
    singleton.members[0].invocations = 10000;
    singleton.members[0].submission = 30;
    samples.Add(singleton, 1);
    auto actual_pair = pair;
    actual_pair.occurrences = actual_pair.queried_occurrences = 2;
    actual_pair.members[0].invocations = 1;
    actual_pair.members[1].invocations = 20;
    actual_pair.members[0].submission = actual_pair.members[1].submission = 40;
    samples.Add(actual_pair, 2);
    ASSERT_EQ(samples.records.size(), 1u);
    EXPECT_EQ(samples.records[0].representative.members[0].submission, 40u);
    EXPECT_EQ(samples.records[0].representative.members[1].submission, 40u);
    EXPECT_EQ(samples.records[0].representative.members[0].invocations, 1u);
    auto next_pair = actual_pair;
    next_pair.members[0].invocations = 50;
    next_pair.members[1].invocations = 1;
    next_pair.members[0].submission = next_pair.members[1].submission = 50;
    samples.Add(next_pair, 2);
    EXPECT_EQ(samples.records[0].representative.members[0].submission, 50u);
    EXPECT_EQ(samples.records[0].representative.members[1].submission, 50u);
    EXPECT_EQ(samples.records[0].representative.members[1].invocations, 1u);
    pair.complete = false;
    samples.Add(pair, 2);
    EXPECT_EQ(samples.records[0].submissions_with_one, 1u);
    EXPECT_EQ(samples.records[0].submissions_with_two, 2u);
    EXPECT_EQ(samples.records[0].submissions_with_more, 1u);
    EXPECT_EQ(samples.records[0].incomplete_submissions, 1u);
    EXPECT_EQ(samples.records[0].max_occurrences, 3u);
    samples.capacity = 1;
    auto other_geometry = actual_pair;
    ++other_geometry.members[0].mesh;
    samples.Add(other_geometry, 2);
    EXPECT_EQ(samples.dropped_groups, 1u);
    EXPECT_EQ(samples.dropped_occurrences, 2u);
    const auto capacity = slot.records.capacity();
    slot.Clear();
    EXPECT_EQ(slot.records.capacity(), capacity);
    for (uint32_t mesh = 0; mesh < 16; ++mesh) {
        first.mesh = mesh;
        slot.Add(first, mesh);
    }
    first.mesh = 100;
    EXPECT_FALSE(slot.NeedsSnapshot(first));
    slot.Add(first, 16);
    slot.Add(first, UINT32_MAX);
    EXPECT_EQ(slot.records.size(), 16u);
    EXPECT_EQ(slot.dropped_draws, 2u);
    first.snapshot = std::make_shared<gfx::detail::FragmentDrawSnapshot>();
    EXPECT_FALSE(first.snapshot->origin.present);
    EXPECT_EQ(first.snapshot->origin.key, 0u);
}

/**
 * Preserves rendering while counter results follow bounded queries and independent measurement lifetimes.
 */
TEST(GfxGpuMeshDc2, FragmentInvocationQueriesPreservePixelsAndWindowOwnership) {
    /**
     * Restores process settings after independent diagnostic renderer lifetimes.
     */
    struct SettingsRestore {
        EnvSnapshot diagnostic = SaveEnv("KZ_PERF_FRAGMENT_INVOCATIONS"); /**< Caller diagnostic policy. */
        EnvSnapshot fog = SaveEnv("KZ_FOG_NEAR_SAFE");                    /**< Caller parent fog policy. */
        EnvSnapshot prog0 = SaveEnv("KZ_PROG0_FOG_NEAR_SAFE");            /**< Caller Prog0 policy. */
        EnvSnapshot split = SaveEnv("KZ_PERF_SPLIT_REPLAY_SUBMIT");       /**< Caller split submission policy. */
        bool        timing = gfx::FrameTimingEnabled();                   /**< Caller frame timing policy. */

        /**
         * Restores all surrounding process settings on every exit path.
         */
        ~SettingsRestore() {
            RestoreEnv(diagnostic);
            RestoreEnv(fog);
            RestoreEnv(prog0);
            RestoreEnv(split);
            gfx::SetFrameTimingEnabled(timing);
        }
    } restore;

    ASSERT_EQ(SetEnv("KZ_FOG_NEAR_SAFE", "1", 1), 0);
    ASSERT_EQ(SetEnv("KZ_PROG0_FOG_NEAR_SAFE", "1", 1), 0);
    ASSERT_EQ(SetEnv("KZ_PERF_SPLIT_REPLAY_SUBMIT", "1", 1), 0);
    gfx::SetFrameTimingEnabled(true);
    std::array<std::vector<uint8_t>, 2> baseline;
    for (int lifetime = 0; lifetime < 4; ++lifetime) {
        const bool requested = lifetime == 1 || lifetime == 3;
        SCOPED_TRACE(lifetime);
        ASSERT_EQ(lifetime == 0 ? UnsetEnv("KZ_PERF_FRAGMENT_INVOCATIONS") : SetEnv("KZ_PERF_FRAGMENT_INVOCATIONS", requested ? "1" : "0", 1), 0);
        gfx::SetFrameTimingEnabled(lifetime != 3);
        auto options = Prog0Options();
        options.offscreen = lifetime == 3;
        GfxFixture fixture(kWidth, kHeight, 1, options);
        auto      &context = gfx::detail::g;
        ASSERT_EQ(context.fragment_invocations_requested, requested);
        if (requested && context.fragment_invocations_supported && !context.fragment_invocations_enabled) {
            GTEST_SKIP() << "Requires KZ_PERF_FRAME_PHASES=1 before process startup";
        }
        ASSERT_EQ(SetEnv("KZ_PERF_FRAGMENT_INVOCATIONS", requested ? "0" : "1", 1), 0);
        EXPECT_EQ(context.fragment_invocations_requested, requested);
        EXPECT_EQ(context.fragment_invocations_enabled, requested && context.fragment_invocations_supported);
        const auto     vertices = ScreenVertices({0, 0, 1}, {173, 113, 197, 128});
        const auto     mesh = gfx::CreateMesh(vertices, kTriangle);
        const auto     texture = gfx::CreateTexture({1, 1, gfx::TextureFormat::Rgba8, 1, true});
        const uint32_t texel = Rgba(117, 71, 193, 127);
        ASSERT_TRUE(gfx::UpdateTexture(texture, 0, 0, 0, 1, 1, &texel));
        const auto target = gfx::CreateRenderTarget(160, 120, true);
        ASSERT_NE(target, gfx::kNullTexture);
        const auto *target_info = gfx::detail::LookupTexture(target);
        ASSERT_NE(target_info, nullptr);
        const std::string target_identity = "target=" + std::to_string(target) + " extent=" + std::to_string(target_info->image.width) + '/' + std::to_string(target_info->image.height);
        auto              draw = Unlit(gfx::kProg0Unlit);
        SetWMvp(draw.mesh.mvp, 1);
        Set(draw.prog0.fog, {180, 255, 0, -60});
        auto crossing = draw;
        crossing.mesh.mvp[2] = .7f;
        crossing.mesh.mvp[14] = .4f;
        gfx::DrawState state;
        state.blend = state.fog = true;
        state.alpha = {0, 2, 0, 1, 0};
        const auto                   binding = NearestBinding(texture);
        const std::array<uint8_t, 4> clear = {17, 29, 43, 71};
        const auto                   record_scene = [&](uint32_t extra, bool distinct_keys) {
            gfx::BeginRecording();
            gfx::SetRenderTarget(target);
            gfx::Clear(true, clear.data(), true, 0);
            gfx::SetInterpKey(101);
            gfx::DrawMeshProg0(mesh, 0, 3, draw.mesh, draw.prog0, binding, state);
            gfx::SetRenderTarget(gfx::kMainTarget);
            gfx::Clear(true, clear.data(), true, 0);
            gfx::SetInterpKey(distinct_keys ? 202 : 101);
            auto crossing_state = state;
            crossing_state.depth_write = true;
            crossing_state.stencil_test = true;
            crossing_state.scissor = true;
            crossing_state.scissor_rect = {0, 0, 640, 480};
            crossing.mesh.normal_matrix[3] = -0.0f;
            crossing.prog0.diffuse[0] = .5f;
            gfx::DrawMeshProg0(mesh, 0, 3, crossing.mesh, crossing.prog0, binding, crossing_state);
            gfx::SetInterpKey(0);
            gfx::DrawMeshProg0(mesh, 0, 3, draw.mesh, draw.prog0, binding, state);
            auto colored = state;
            colored.fog_color[0] = 1;
            gfx::DrawMeshProg0(mesh, 0, 3, draw.mesh, draw.prog0, binding, colored);
            for (uint32_t i = 0; i < extra; ++i) {
                gfx::DrawMeshProg0(mesh, 0, 3, draw.mesh, draw.prog0, binding, state);
            }
            return gfx::EndRecording();
        };
        auto list = record_scene(0, false);
        auto distinct = record_scene(0, true);
        auto overflow = record_scene(gfx::detail::kFragmentInvocationQueries + 1, true);
        ASSERT_NE(list, nullptr);
        const auto pair = [&](const gfx::DisplayList *scene = nullptr) {
            const auto &selected = scene == nullptr ? *list : *scene;
            EXPECT_TRUE(gfx::RenderList(selected, 1, {.canonical = true}));
            EXPECT_TRUE(gfx::RenderList(selected, .5f, {.present = true}));
        };
        pair();
        gfx::BeginPresentationHistory(0);
        pair();
        // Old in-flight query references must never enter the replacement measurement.
        gfx::BeginPresentationHistory(0);
        for (int window = 0; window < 2; ++window) {
            if (window != 0) {
                gfx::BeginPresentationHistory(0);
            }
            for (int frame = 0; frame < 8; ++frame) {
                pair(window == 1 ? (frame == 7 ? overflow.get() : distinct.get()) : nullptr);
            }
            if (window == 1) {
                list.reset();
                overflow.reset();
                distinct.reset();
                gfx::DestroyMesh(mesh);
                gfx::DestroyTexture(texture);
                gfx::DestroyTexture(target);
            }
            gfx::EndPresentationHistory(0);
            ::testing::internal::CaptureStderr();
            gfx::LogPresentationHistory();
            const auto output = ::testing::internal::GetCapturedStderr();
            ASSERT_TRUE(gfx::ReadbackFrame(fixture.pixels, fixture.width, fixture.height));
            EXPECT_EQ(context.counters.mesh_draws, window == 0 ? 4u : 4u + gfx::detail::kFragmentInvocationQueries + 1);
            if (lifetime == 3) {
                EXPECT_EQ(context.timestamp_pool, VK_NULL_HANDLE);
            }
            for (const auto &slot : context.frames) {
                EXPECT_LE(slot.fragment_draws.size(), gfx::detail::kFragmentInvocationQueries);
            }
            if (baseline[static_cast<size_t>(window)].empty()) {
                baseline[static_cast<size_t>(window)] = fixture.pixels;
            } else {
                EXPECT_EQ(fixture.pixels, baseline[static_cast<size_t>(window)]);
            }
            if (!requested) {
                EXPECT_EQ(context.fragment_invocation_pool, VK_NULL_HANDLE);
                EXPECT_EQ(output.find("perf fragment invocations begin:"), std::string::npos);
                for (const auto &frame : context.frames) {
                    EXPECT_EQ(frame.fragment_draws.capacity(), 0u);
                    EXPECT_EQ(frame.fragment_values.capacity(), 0u);
                    EXPECT_EQ(frame.fragment_pairs.records.capacity(), 0u);
                }
            } else if (!context.fragment_invocations_supported) {
                EXPECT_NE(output.find("supported=false enabled=false"), std::string::npos);
                EXPECT_NE(output.find("complete_counts=false"), std::string::npos);
            } else {
                EXPECT_NE(context.fragment_invocation_pool, VK_NULL_HANDLE);
                EXPECT_NE(output.find("schema=2"), std::string::npos);
                EXPECT_NE(output.find(target_identity), std::string::npos) << output;
                EXPECT_NE(output.find("extra_application_waits=0"), std::string::npos);
                for (const char *role : {"canonical-render", "display-replay"}) {
                    EXPECT_NE(output.find(std::string("role=") + role + (window == 0 ? " expected=16/8 observed=16/8" : " expected=529/8 observed=529/8")), std::string::npos) << output;
                }
                uint64_t available = 0;
                uint64_t unavailable = 0;
                uint64_t skipped = 0;
                uint64_t invocations = 0;
                size_t   cursor = 0;
                while ((cursor = output.find("perf fragment invocation bucket:", cursor)) != std::string::npos) {
                    const auto end = output.find('\n', cursor);
                    const auto line = output.substr(cursor, end - cursor);
                    const auto number = [&](const char *key) {
                        const auto position = line.find(key);
                        EXPECT_NE(position, std::string::npos);
                        return position == std::string::npos ? uint64_t{0} : std::stoull(line.substr(position + std::strlen(key)));
                    };
                    available += number(" available=");
                    unavailable += number(" unavailable=");
                    skipped += number(" skipped=");
                    invocations += number(" invocations=");
                    EXPECT_EQ(number(" errors="), 0u);

                    if (end == std::string::npos) {
                        break;
                    }
                    cursor = end + 1;
                }
                EXPECT_GT(available, 0u);
                EXPECT_GT(invocations, 0u);
                EXPECT_EQ(skipped, window == 0 ? 0u : 8u);
                EXPECT_NE(output.find(unavailable != 0 || skipped != 0 ? "complete_counts=false" : "complete_counts=true"), std::string::npos);
                EXPECT_NE(output.find("bounds_known=true bounds_normal=true"), std::string::npos);
                EXPECT_NE(output.find("perf fragment pair member:"), std::string::npos);
                EXPECT_NE(output.find("section=origin fields="), std::string::npos);
                EXPECT_NE(output.find("\"key\":101"), std::string::npos);
                EXPECT_NE(output.find("\"key\":" + std::to_string(window == 0 ? 101 : 202)), std::string::npos);
                EXPECT_NE(output.find("section=push fields="), std::string::npos);
                EXPECT_NE(output.find("section=resolved fields="), std::string::npos);
                EXPECT_NE(output.find("80000000"), std::string::npos);
                if (unavailable != 0 || skipped != 0) {
                    EXPECT_NE(output.find("complete_pairs=false"), std::string::npos);
                    EXPECT_EQ(output.find("complete_pairs=true"), std::string::npos);
                }
                EXPECT_FALSE(gfx::detail::tls.fragment_origin.present);
                std::string matrix = "mvp_f32=";
                for (float value : draw.mesh.mvp) {
                    if (matrix.back() != '=') {
                        matrix += '/';
                    }
                    matrix += std::format("{:08x}", std::bit_cast<uint32_t>(value));
                }
                EXPECT_NE(output.find(matrix), std::string::npos);
                if (window == 0) {
                    const auto stale = output.find("stale_excluded_draws=");
                    ASSERT_NE(stale, std::string::npos);
                    EXPECT_GT(std::stoull(output.substr(stale + std::strlen("stale_excluded_draws="))), 0u);
                }
                // A second log and later slot recycling must not consume the same queries twice.
                ::testing::internal::CaptureStderr();
                gfx::LogPresentationHistory();
                EXPECT_EQ(::testing::internal::GetCapturedStderr().find("perf fragment invocations begin:"), std::string::npos);
            }
        }
        if (lifetime == 1) {
            EXPECT_GT(context.split_replay_selected, 0u);
            EXPECT_EQ(context.split_replay_first_submits, context.split_replay_final_submits);
        }
        if (options.offscreen) {
            EXPECT_EQ(context.split_replay_selected, 0u);
        }
        EXPECT_TRUE(std::any_of(baseline[0].begin(), baseline[0].end(), [](uint8_t value) { return value > 100; }));
        EXPECT_TRUE(std::any_of(baseline[1].begin(), baseline[1].end(), [](uint8_t value) { return value > 100; }));
    }
}

TEST(GfxGpuMeshDc2, Prog0FogRejectionDiagnosticPreservesPixelsAndRendererOwnership) {
    /**
     * Restores the caller's diagnostic setting after all renderer lifetimes.
     */
    struct SettingRestore {
        EnvSnapshot original = SaveEnv("KZ_PERF_PROG0_FOG_REJECTIONS"); /**< Original process override. */

        /**
         * Restores the process setting even when a fixture assertion returns early.
         */
        ~SettingRestore() { RestoreEnv(original); }
    } restore;

    std::vector<uint8_t> baseline;
    uint32_t             baseline_draws = 0;
    for (const char *setting : {static_cast<const char *>(nullptr), "1", "0", "1"}) {
        const bool enabled = setting != nullptr && std::strcmp(setting, "1") == 0;
        SCOPED_TRACE(setting == nullptr ? "default" : setting);
        ASSERT_EQ(setting == nullptr ? UnsetEnv("KZ_PERF_PROG0_FOG_REJECTIONS") : SetEnv("KZ_PERF_PROG0_FOG_REJECTIONS", setting, 1), 0);
        GfxFixture fixture(kWidth, kHeight, 1.0f, Prog0Options());
        if (enabled && !gfx::detail::g.prog0_fog_rejections) {
            GTEST_SKIP() << "Requires KZ_PERF_FRAME_PHASES=1 before process startup";
        }
        if (gfx::detail::g.prog0_mode1_fog_pipeline == VK_NULL_HANDLE) {
            GTEST_SKIP() << "Requires enabled Prog0 fog optimization for pipeline-selection observations";
        }
        ASSERT_EQ(gfx::detail::g.prog0_fog_rejections, enabled);
        ASSERT_EQ(SetEnv("KZ_PERF_PROG0_FOG_REJECTIONS", enabled ? "0" : "1", 1), 0);
        EXPECT_EQ(gfx::detail::g.prog0_fog_rejections, enabled);
        auto       vertices = ScreenVertices({0, 0, 1}, {173, 113, 197, 128});
        const auto mesh = gfx::CreateMesh(vertices, kTriangle);
        const auto updated = gfx::CreateMesh(vertices, kTriangle);
        ASSERT_NE(mesh, gfx::kNullMesh);
        ASSERT_NE(updated, gfx::kNullMesh);
        ASSERT_TRUE(gfx::UpdateMeshVertices(updated, 0, vertices));
        auto skin_draw = Unlit(gfx::kProg0Unlit | gfx::kProg0Skinned);
        SetWMvp(skin_draw.mesh.mvp, 1);
        Set(skin_draw.prog0.fog, {180, 255, 0, -60});
        for (uint32_t bone = 0; bone < gfx::kProg0Bones; ++bone) {
            SetBone(skin_draw.palette, bone, kIdentityBone);
        }
        const auto skin_vertices = Skinned(vertices, {1, 0, 0, 0}, {0, 0, 0, 0});
        const auto skin_mesh = gfx::CreateMesh(skin_vertices, kTriangle);
        ASSERT_NE(skin_mesh, gfx::kNullMesh);
        const auto     texture = gfx::CreateTexture({1, 1, gfx::TextureFormat::Rgba8, 1, true});
        const uint32_t texel = Rgba(117, 71, 193, 127);
        ASSERT_TRUE(gfx::UpdateTexture(texture, 0, 0, 0, 1, 1, &texel));
        const auto                   binding = NearestBinding(texture);
        const std::array<uint8_t, 4> clear = {17, 29, 43, 71};
        auto                         draw = Unlit(gfx::kProg0Unlit);
        SetWMvp(draw.mesh.mvp, 1);
        Set(draw.prog0.fog, {180, 255, 0, -60});
        gfx::DrawState state;
        state.blend = state.fog = true;
        state.alpha = {0, 2, 0, 1, 0};
        gfx::BeginRecording();
        gfx::Clear(true, clear.data(), true, 0);
        gfx::DrawMeshProg0(mesh, 0, 3, draw.mesh, draw.prog0, binding, state);
        auto near = draw;
        near.mesh.mvp[2] = .7f;
        near.mesh.mvp[14] = .4f;
        gfx::DrawMeshProg0(mesh, 0, 3, near.mesh, near.prog0, binding, state);
        gfx::DrawMeshProg0(updated, 0, 3, draw.mesh, draw.prog0, binding, state);
        auto other = state;
        other.alpha.b = 1;
        gfx::DrawMeshProg0(mesh, 0, 3, draw.mesh, draw.prog0, binding, other);
        other = state;
        other.alpha_test = true;
        other.alpha_func = gfx::AlphaFunc::GEqual;
        other.alpha_ref = 1;
        gfx::DrawMeshProg0(mesh, 0, 3, draw.mesh, draw.prog0, binding, other);
        other = state;
        other.fog = false;
        gfx::DrawMeshProg0(mesh, 0, 3, draw.mesh, draw.prog0, binding, other);
        gfx::DrawMeshProg0(mesh, 0, 3, draw.mesh, draw.prog0, {}, state);
        other = state;
        other.color_write_mask = 0;
        gfx::DrawMeshProg0(mesh, 0, 3, draw.mesh, draw.prog0, binding, other);
        gfx::DrawMeshProg0(skin_mesh, 0, 3, skin_draw.mesh, skin_draw.prog0, binding, state, nullptr, &skin_draw.palette);
        auto list = gfx::EndRecording();
        ASSERT_NE(list, nullptr);
        EXPECT_TRUE(gfx::RenderList(*list, 1, {.canonical = true}));
        EXPECT_TRUE(gfx::RenderList(*list, .5f, {.present = true}));
        for (int window = 0; window < 2; ++window) {
            SCOPED_TRACE(window);
            ::testing::internal::CaptureStderr();
            gfx::BeginPresentationHistory(0);
            EXPECT_TRUE(gfx::RenderList(*list, 1, {.canonical = true}));
            EXPECT_TRUE(gfx::RenderList(*list, .5f, {.present = true}));
            gfx::EndPresentationHistory(0);
            gfx::LogPresentationHistory();
            const auto output = ::testing::internal::GetCapturedStderr();
            EXPECT_EQ(gfx::detail::g.counters.mode1_prog0_fog, 1u);
            EXPECT_EQ(gfx::detail::g.counters.black_prog0_fog, 2u);
            EXPECT_TRUE(gfx::ReadbackFrame(fixture.pixels, fixture.width, fixture.height));
            if (baseline.empty()) {
                baseline = fixture.pixels;
                baseline_draws = gfx::detail::g.counters.mesh_draws;
            } else {
                EXPECT_EQ(fixture.pixels, baseline);
                EXPECT_EQ(gfx::detail::g.counters.mesh_draws, baseline_draws);
            }
            if (enabled) {
                EXPECT_NE(output.find("perf prog0 fog rejections begin: schema=2 "), std::string::npos);
                for (const char *role : {"canonical-render", "display-replay"}) {
                    for (const char *reason : {"safe", "near-margin", "unknown-bounds"}) {
                        const std::string prefix = std::string("perf prog0 fog rejection count: role=") + role + " reason=" + reason + " draws=";
                        const auto        position = output.find(prefix);
                        EXPECT_NE(position, std::string::npos) << output;
                        if (position != std::string::npos) {
                            EXPECT_EQ(std::stoull(output.substr(position + prefix.size())), 1u);
                        }
                        const std::string identity = std::string(" role=") + role + " reason=" + reason + ' ';
                        std::string       representative;
                        size_t            cursor = 0;
                        while ((cursor = output.find("perf prog0 fog rejection representative:", cursor)) != std::string::npos) {
                            const auto end = output.find('\n', cursor);
                            const auto line = output.substr(cursor, end == std::string::npos ? end : end - cursor);
                            if (line.find(identity) != std::string::npos) {
                                representative = line;
                                break;
                            }
                            if (end == std::string::npos) {
                                break;
                            }
                            cursor = end + 1;
                        }
                        EXPECT_FALSE(representative.empty());
                        const bool     near_safe = std::strcmp(reason, "safe") == 0;
                        const bool     mode1 = std::strcmp(reason, "near-margin") == 0;
                        const uint32_t scope = near_safe ? gfx::detail::kProg0NearSafeFogAdditiveBlackScope : mode1 ? gfx::detail::kProg0Mode1FogBlackScope
                                                                                                                    : gfx::detail::g.prog0_mode1_fog_base;
                        const auto     selections = std::format("near_safe_selected={} mode1_selected={} actual_scope={:#x}", near_safe, mode1, scope);
                        EXPECT_NE(representative.find(selections), std::string::npos) << representative;
                    }
                }
                EXPECT_NE(output.find("mvp_bits=3f800000/00000000/00000000/00000000"), std::string::npos);
                EXPECT_NE(output.find("representative_submission=1 "), std::string::npos);
                EXPECT_NE(output.find("clip_f64_bits=unavailable"), std::string::npos);
                EXPECT_NE(output.find("unrepresented_draws=0"), std::string::npos);
                EXPECT_NE(output.find("extra_gpu_queries=0 extra_gpu_waits=0"), std::string::npos);
            } else {
                EXPECT_EQ(output.find("perf prog0 fog rejection"), std::string::npos);
            }
        }
        size_t changed = 0;
        for (size_t pixel = 0; pixel + 3 < baseline.size(); pixel += 4) {
            changed += baseline[pixel] != clear[0] || baseline[pixel + 1] != clear[1] || baseline[pixel + 2] != clear[2] ? 1u : 0u;
        }
        EXPECT_GT(changed, 0u);
        EXPECT_GT(baseline_draws, 0u);
        list.reset();
        gfx::DestroyTexture(texture);
        gfx::DestroyMesh(skin_mesh);
        gfx::DestroyMesh(updated);
        gfx::DestroyMesh(mesh);
    }
}

TEST(GfxGpuMeshDc2, Prog0NearSafeFogPreservesFullRgbaAgainstUpdatedMeshReference) {
    GfxFixture fixture(kWidth, kHeight, 1.0f, Prog0Options());
    if (!gfx::detail::NearSafeFogEnabled()) {
        GTEST_SKIP() << "Requires KZ_FOG_NEAR_SAFE=1 before process startup";
    }
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
    /**
     * Selects a generic blend equation whose fog specialization must preserve every channel.
     */
    enum class BlendMode {
        Overwrite,
        SourceOver,
        Additive
    };
    for (bool lit : {false, true}) {
        for (BlendMode blend : {BlendMode::Overwrite, BlendMode::SourceOver, BlendMode::Additive}) {
            for (bool alpha_test : {false, true}) {
                for (const auto &test : cases) {
                    SCOPED_TRACE(test.name);
                    SCOPED_TRACE(lit ? "lit" : "unlit");
                    SCOPED_TRACE(blend == BlendMode::Overwrite ? "b0 overwrite" : blend == BlendMode::SourceOver ? "b4 source-over"
                                                                                                                 : "b12 additive");
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
                    state.blend = blend != BlendMode::Overwrite;
                    state.alpha = {0, static_cast<uint8_t>(blend == BlendMode::Additive ? 2 : 1), 0, 1, 0};
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
                    const bool supported_pipeline = (blend == BlendMode::Overwrite && alpha_test) || (blend != BlendMode::Overwrite && !alpha_test);
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

/**
 * Creates the additive fog variant before drawing and retires it across cached renderer lifetimes.
 */
TEST(GfxGpuMeshDc2, Prog0AdditiveFogPipelineIsReadyAcrossCachedRendererLifetimes) {
    auto options = Prog0Options();
    options.pipeline_cache = std::filesystem::temp_directory_path() / "dc_gfx_test" / "pipeline_cache_prog0_additive.bin";
    for (int lifetime = 0; lifetime < 2; ++lifetime) {
        SCOPED_TRACE(lifetime);
        {
            GfxFixture fixture(kWidth, kHeight, 1.0f, options);
            if (!gfx::detail::NearSafeFogEnabled()) {
                GTEST_SKIP() << "Requires KZ_FOG_NEAR_SAFE=1 before process startup";
            }
            ASSERT_NE(gfx::detail::g.prog0_near_safe_fog_pipelines[2], VK_NULL_HANDLE);
            ASSERT_NE(gfx::detail::g.prog0_mode1_fog_pipeline, VK_NULL_HANDLE);
            ASSERT_NE(gfx::detail::g.prog0_near_safe_black_fog_pipeline, VK_NULL_HANDLE);
            ASSERT_NE(gfx::detail::g.prog0_mode1_black_fog_pipeline, VK_NULL_HANDLE);
            EXPECT_EQ(gfx::detail::g.prog0_mode1_fog_base, gfx::detail::g.prog0_near_safe_fog_bases[2]);
            EXPECT_EQ(gfx::detail::DescribePipeline(gfx::detail::kProg0Mode1FogScope), "prog0-fog-mode1-fresh");
            EXPECT_EQ(gfx::detail::g.prog0_near_safe_fog_bases[2],
                      gfx::detail::PipelineIndex(gfx::detail::kFamilyMeshProg0, 12, gfx::detail::kTextureRgba,
                                                 gfx::detail::kAlphaVariantNone, gfx::detail::kColorFeatureCount + 1));
            EXPECT_EQ(gfx::detail::DescribePipeline(gfx::detail::kProg0NearSafeFogAdditiveScope),
                      "prog0-fog-near-safe-additive");
        }
        EXPECT_EQ(gfx::detail::g.prog0_near_safe_fog_pipelines[2], VK_NULL_HANDLE);
        EXPECT_EQ(gfx::detail::g.prog0_mode1_fog_pipeline, VK_NULL_HANDLE);
        EXPECT_EQ(gfx::detail::g.prog0_near_safe_black_fog_pipeline, VK_NULL_HANDLE);
        EXPECT_EQ(gfx::detail::g.prog0_mode1_black_fog_pipeline, VK_NULL_HANDLE);
        EXPECT_EQ(gfx::detail::g.prog0_mode1_fog_base, UINT32_MAX);
        EXPECT_EQ(gfx::detail::g.prog0_near_safe_fog_bases[2], UINT32_MAX);
        ASSERT_TRUE(std::filesystem::exists(options.pipeline_cache));
        EXPECT_GT(std::filesystem::file_size(options.pipeline_cache), 0u);
    }
}

/**
 * Preserves overlapping additive pixels and later depth/stencil consumers, including invisible alpha.
 */
TEST(GfxGpuMeshDc2, Prog0AdditiveFogPreservesOverlapAndDepthStencilSideEffects) {
    GfxFixture fixture(kWidth, kHeight, 1.0f, Prog0Options());
    if (!gfx::detail::NearSafeFogEnabled()) {
        GTEST_SKIP() << "Requires KZ_FOG_NEAR_SAFE=1 before process startup";
    }
    const std::array<uint8_t, 4> clear = {17, 29, 43, 71};
    auto                         vertices = ScreenVertices({0, 0, 1}, {173, 113, 197, 128});
    vertices[0].uv[0] = vertices[0].uv[1] = .13f;
    vertices[1].uv[0] = vertices[2].uv[1] = 3.67f;
    const auto mesh = gfx::CreateMesh(vertices, kTriangle);
    const auto reference_mesh = gfx::CreateMesh(vertices, kTriangle);
    ASSERT_NE(mesh, gfx::kNullMesh);
    ASSERT_NE(reference_mesh, gfx::kNullMesh);
    ASSERT_TRUE(gfx::UpdateMeshVertices(reference_mesh, 0, vertices));
    ASSERT_FALSE(gfx::detail::LookupMesh(reference_mesh)->bounds_known);
    const auto texture = gfx::CreateTexture({2, 2, gfx::TextureFormat::Rgba8, 1, true});
    ASSERT_NE(texture, gfx::kNullTexture);
    const std::array texels = {Rgba(231, 73, 119, 0), Rgba(61, 193, 107, 1),
                               Rgba(83, 127, 223, 127), Rgba(199, 167, 47, 255)};
    ASSERT_TRUE(gfx::UpdateTexture(texture, 0, 0, 0, 2, 2, texels.data()));
    auto binding = NearestBinding(texture);
    binding.filter = gfx::Filter::Linear;
    for (float alpha : {0.0f, .0001f, .37f, 1.0f, 1.75f}) {
        SCOPED_TRACE(alpha);
        for (bool depth_write : {false, true}) {
            SCOPED_TRACE(depth_write);
            for (bool stencil : {false, true}) {
                SCOPED_TRACE(stencil);
                /**
                 * Renders the same geometry through immutable or independently generic fog pipelines.
                 */
                const auto render = [&](gfx::MeshHandle handle, bool specialized, bool probe) {
                    fixture.Frame(clear, [&] {
                        gfx::ClearStencil(0);
                        gfx::DrawState occluder;
                        occluder.depth_write = true;
                        occluder.color_write_mask = 0;
                        const auto cover = Quad(0, 0, gfx::LogicalWidth() / 4, gfx::LogicalHeight(), {0, 0, 0, 128}, 0, 0, 0, 0, .8f);
                        gfx::Draw2D(gfx::Primitive::Quads, cover, {}, occluder);
                        auto draw = Unlit(gfx::kProg0Unlit);
                        SetWMvp(draw.mesh.mvp, 2);
                        Set(draw.prog0.fog, {180, 255, 0, -60});
                        draw.prog0.diffuse[3] = alpha;
                        gfx::DrawState state;
                        state.blend = true;
                        state.alpha = {0, 2, 0, 1, 0};
                        state.fog = true;
                        state.fog_color[0] = 23;
                        state.fog_color[1] = 47;
                        state.fog_color[2] = 79;
                        state.depth_test = gfx::DepthTest::GEqual;
                        state.depth_write = depth_write;
                        state.stencil_test = stencil;
                        state.stencil_front.pass = gfx::StencilOp::Replace;
                        state.stencil_front.reference = 7;
                        state.stencil_back = state.stencil_front;
                        const auto before = gfx::detail::g.counters.near_safe_prog0_fog;
                        gfx::DrawMeshProg0(handle, 0, 3, draw.mesh, draw.prog0, binding, state);
                        draw.prog0.diffuse[0] = .61f;
                        draw.prog0.diffuse[1] = .83f;
                        gfx::DrawMeshProg0(handle, 0, 3, draw.mesh, draw.prog0, binding, state);
                        EXPECT_EQ(gfx::detail::g.counters.near_safe_prog0_fog - before, specialized ? 2u : 0u);
                        if (probe) {
                            gfx::DrawState consumer;
                            consumer.depth_test = gfx::DepthTest::GEqual;
                            consumer.stencil_test = stencil;
                            consumer.stencil_front.compare = gfx::CompareOp::Equal;
                            consumer.stencil_front.reference = 7;
                            consumer.stencil_back = consumer.stencil_front;
                            const auto blue = Quad(0, 0, gfx::LogicalWidth(), gfx::LogicalHeight(), {0, 0, 128, 128}, 0, 0, 0, 0, .25f);
                            gfx::Draw2D(gfx::Primitive::Quads, blue, {}, consumer);
                        }
                    });
                    return fixture.pixels;
                };
                const auto expected = render(reference_mesh, false, false);
                EXPECT_EQ(render(mesh, true, false), expected);
                const auto expected_probe = render(reference_mesh, false, true);
                EXPECT_EQ(render(mesh, true, true), expected_probe);
                if (depth_write) {
                    EXPECT_EQ(expected_probe, expected);
                } else {
                    EXPECT_NE(expected_probe, expected);
                    EXPECT_EQ(fixture.Pixel(kWidth / 2, kHeight / 2)[2], 128);
                }
            }
        }
    }
    gfx::DestroyTexture(texture);
    gfx::DestroyMesh(reference_mesh);
    gfx::DestroyMesh(mesh);
}

TEST(GfxGpuMeshDc2, Prog0NearSafeFogUsesActualInterpolatedMvp) {
    GfxFixture fixture(kWidth, kHeight, 1.0f, Prog0Options());
    if (!gfx::detail::NearSafeFogEnabled()) {
        GTEST_SKIP() << "Requires KZ_FOG_NEAR_SAFE=1 before process startup";
    }
    auto vertices = ScreenVertices({0, 0, 1});
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
    for (bool additive : {false, true}) {
        SCOPED_TRACE(additive);
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
                state.alpha = {0, static_cast<uint8_t>(additive ? 2 : 1), 0, 1, 0};
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
                EXPECT_EQ(gfx::detail::g.counters.mode1_prog0_fog, additive && translation <= 1 ? 1u : 0u);
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

namespace {
/**
 * Restores renderer policies used by independent smooth-input comparisons.
 */
struct SharedRgbPolicies {
    EnvSnapshot unlit_vertex = SaveEnv("KZ_PROG0_UNLIT_VERTEX"); /**< Caller vertex specialization policy. */
    EnvSnapshot shared_rgb = SaveEnv("KZ_PROG0_SHARED_RGB"); /**< Caller shared-input policy. */
    EnvSnapshot fog = SaveEnv("KZ_FOG_NEAR_SAFE");           /**< Caller parent fog policy. */
    EnvSnapshot prog0 = SaveEnv("KZ_PROG0_FOG_NEAR_SAFE");   /**< Caller Prog0 policy. */
    EnvSnapshot prewarm = SaveEnv("KZ_PIPELINE_PREWARM");    /**< Caller pipeline creation policy. */

    /**
     * Enables the established black families without changing their shader inputs.
     */
    SharedRgbPolicies() {
        SetEnv("KZ_PROG0_UNLIT_VERTEX", "0", 1);
        SetEnv("KZ_FOG_NEAR_SAFE", "1", 1);
        SetEnv("KZ_PROG0_FOG_NEAR_SAFE", "1", 1);
        SetEnv("KZ_PIPELINE_PREWARM", "0", 1);
    }

    /**
     * Restores every process override after assertions or normal completion.
     */
    ~SharedRgbPolicies() {
        RestoreEnv(unlit_vertex);
        RestoreEnv(shared_rgb);
        RestoreEnv(fog);
        RestoreEnv(prog0);
        RestoreEnv(prewarm);
    }
};

/**
 * Builds a visible additive material selecting the safe or near-crossing black family.
 */
Prog0Draw SharedRgbBlackDraw(bool crossing) {
    auto draw = Unlit(gfx::kProg0Unlit);
    Set(draw.prog0.object_color, {128, 128, 128, 128});
    Set(draw.prog0.diffuse, {1, 1, 1, 1});
    SetWMvp(draw.mesh.mvp, 4);
    if (crossing) {
        draw.mesh.mvp[2] = 1;
        draw.mesh.mvp[14] = 2.5f;
    }
    Set(draw.prog0.fog, {-153, 255, 0, 612000});
    return draw;
}

/**
 * Routes only a test draw through the guarded fallback and restores both owned handles.
 */
struct SharedRgbGuardFallback {
    VkPipeline ordinary; /**< Original ordinary mode1 handle. */
    VkPipeline shared;   /**< Original shared mode1 handle. */
    VkPipeline unlit; /**< Original unlit-vertex mode1 handle. */

    /**
     * Uses the guarded fragment recipe without changing the positive material.
     */
    explicit SharedRgbGuardFallback(bool enabled) : ordinary(gfx::detail::g.prog0_mode1_black_fog_pipeline), shared(gfx::detail::g.prog0_shared_rgb_pipelines[1]), unlit(gfx::detail::g.prog0_unlit_vertex_pipelines[1]) {
        if (enabled) {
            gfx::detail::g.prog0_mode1_black_fog_pipeline = gfx::detail::g.prog0_guarded_zero_alpha_pipeline;
            gfx::detail::g.prog0_shared_rgb_pipelines[1] = gfx::detail::g.prog0_shared_rgb_pipelines[2];
            gfx::detail::g.prog0_unlit_vertex_pipelines[1] = gfx::detail::g.prog0_unlit_vertex_pipelines[2];
        }
    }

    /**
     * Restores original ownership before pipeline destruction.
     */
    ~SharedRgbGuardFallback() {
        gfx::detail::g.prog0_mode1_black_fog_pipeline = ordinary;
        gfx::detail::g.prog0_shared_rgb_pipelines[1] = shared;
        gfx::detail::g.prog0_unlit_vertex_pipelines[1] = unlit;
    }
};
} // namespace

/**
 * Compares visible multi-mip sampling and partial depth rejection against unchanged shader lifetimes.
 */
namespace {
void CheckSharedRgbPixels(bool unlit_vertex) {
    SharedRgbPolicies                 policies;
    std::vector<std::vector<uint8_t>> reference;
    std::array<uint8_t, 4>            magnified_center{};
    for (bool enabled : {false, true}) {
        ASSERT_EQ(SetEnv("KZ_PROG0_SHARED_RGB", unlit_vertex || enabled ? "1" : "0", 1), 0);
        ASSERT_EQ(SetEnv("KZ_PROG0_UNLIT_VERTEX", unlit_vertex && enabled ? "1" : "0", 1), 0);
        GfxFixture fixture(kWidth, kHeight, 1, Prog0Options());
        auto &selected = unlit_vertex ? gfx::detail::g.prog0_unlit_vertex_selected : gfx::detail::g.prog0_shared_rgb_selected;
        const auto &pipelines = unlit_vertex ? gfx::detail::g.prog0_unlit_vertex_pipelines : gfx::detail::g.prog0_shared_rgb_pipelines;
        EXPECT_EQ(gfx::detail::g.prog0_unlit_vertex, unlit_vertex && enabled);
        ASSERT_EQ(gfx::detail::g.prog0_shared_rgb, unlit_vertex || enabled);
        for (auto pipeline : pipelines) {
            EXPECT_EQ(pipeline != VK_NULL_HANDLE, enabled);
        }
        const auto                                  texture = gfx::CreateTexture({16, 16, gfx::TextureFormat::Rgba8, 5, true});
        const std::array<std::array<uint8_t, 3>, 5> colors = {
            {{233, 17, 31}, {19, 211, 23}, {13, 29, 211}, {179, 23, 193}, {37, 181, 173}}
        };
        for (uint32_t mip = 0; mip < 5; ++mip) {
            const uint32_t        size = 16u >> mip;
            std::vector<uint32_t> pixels(size * size);
            for (uint32_t y = 0; y < size; ++y) {
                for (uint32_t x = 0; x < size; ++x) {
                    const auto &color = colors[mip];
                    pixels[y * size + x] = Rgba(color[0], color[1], color[2], ((x + y) & 1) ? 224 : 160);
                }
            }
            ASSERT_TRUE(gfx::UpdateTexture(texture, mip, 0, 0, size, size, pixels.data()));
        }
        const auto            single_texture = gfx::CreateTexture({16, 16, gfx::TextureFormat::Rgba8, 1, true});
        std::vector<uint32_t> single_pixels(16 * 16);
        for (uint32_t y = 0; y < 16; ++y) {
            for (uint32_t x = 0; x < 16; ++x) {
                single_pixels[y * 16 + x] = Rgba(static_cast<uint8_t>(17 + x * 13), static_cast<uint8_t>(31 + y * 11), static_cast<uint8_t>(211 - x * 7), static_cast<uint8_t>(((x + y) & 3) * 85));
            }
        }
        ASSERT_TRUE(gfx::UpdateTexture(single_texture, 0, 0, 0, 16, 16, single_pixels.data()));
        size_t index = 0;
        for (bool forced_guard : {false, true}) {
            for (bool single_level : {false, true}) {
                for (bool crossing : {false, true}) {
                    if (forced_guard && (!single_level || !crossing)) {
                        continue;
                    }
                    for (auto filter : {gfx::Filter::Nearest, gfx::Filter::Linear}) {
                        for (auto wrap : {gfx::Wrap::Clamp, gfx::Wrap::Repeat}) {
                            for (int occlusion : {0, 1, 2}) {
                                for (float uv_scale : {.25f, 16.0f}) {
                                    SCOPED_TRACE(std::format("enabled{} crossing{} filter{} wrap{} occlusion{} uv{}", enabled, crossing, static_cast<int>(filter), static_cast<int>(wrap), occlusion, uv_scale));
                                    auto vertices = ScreenVertices({0, 0, 1});
                                    for (auto &vertex : vertices) {
                                        vertex.uv[0] = (vertex.position[0] + 1) * uv_scale / 2 + .5f / 16 - .125f;
                                        vertex.uv[1] = (vertex.position[1] + 1) * uv_scale / 2 + .5f / 16 - .125f;
                                    }
                                    const auto             mesh = gfx::CreateMesh(vertices, kTriangle);
                                    SharedRgbGuardFallback fallback(forced_guard);
                                    auto                   draw = SharedRgbBlackDraw(crossing);
                                    gfx::DrawState         state;
                                    state.blend = state.fog = true;
                                    state.alpha = {0, 2, 0, 1, 0};
                                    state.depth_test = gfx::DepthTest::GEqual;
                                    state.alpha_test = true;
                                    state.alpha_func = gfx::AlphaFunc::GEqual;
                                    state.alpha_ref = 0;
                                    auto binding = NearestBinding(single_level ? single_texture : texture);
                                    binding.filter = filter;
                                    binding.wrap_u = binding.wrap_v = wrap;
                                    const auto                   before = selected;
                                    const std::array<uint8_t, 4> clear = {17, 31, 47, 91};
                                    fixture.Frame(clear, [&] {
                                        if (occlusion) {
                                            gfx::DrawState cover;
                                            cover.depth_write = true;
                                            const auto quad = Quad(0, 0, gfx::LogicalWidth() / (occlusion == 2 ? 2 : 1), gfx::LogicalHeight(), {71, 23, 41, 128}, 0, 0, 0, 0, 1);
                                            gfx::Draw2D(gfx::Primitive::Quads, quad, {}, cover);
                                        }
                                        gfx::DrawMeshProg0(mesh, 0, 3, draw.mesh, draw.prog0, binding, state);
                                        draw.mesh.mvp[12] = .1f;
                                        draw.prog0.diffuse[3] = .75f;
                                        gfx::DrawMeshProg0(mesh, 0, 3, draw.mesh, draw.prog0, binding, state);
                                    });
                                    const size_t family = crossing ? 1 : 0;
                                    EXPECT_EQ(selected[family] - before[family], enabled ? 2u : 0u);
                                    EXPECT_EQ(gfx::detail::g.counters.mesh_draws, 2u);
                                    if (!enabled) {
                                        reference.push_back(fixture.pixels);
                                    } else {
                                        EXPECT_EQ(fixture.pixels, reference[index]);
                                    }
                                    if (occlusion) {
                                        EXPECT_EQ(fixture.Pixel(kWidth / 4, kHeight / 2)[0], 71u);
                                    }
                                    if (single_level && occlusion != 1) {
                                        size_t visible = 0;
                                        for (int y = 0; y < fixture.height; ++y) {
                                            for (int x = occlusion == 2 ? fixture.width / 2 : 0; x < fixture.width; ++x) {
                                                const auto pixel = fixture.Pixel(x, y);
                                                visible += pixel[0] != clear[0] || pixel[1] != clear[1] || pixel[2] != clear[2];
                                            }
                                        }
                                        EXPECT_GT(visible, 0u);
                                    }
                                    if (!single_level && !crossing && filter == gfx::Filter::Nearest && wrap == gfx::Wrap::Clamp && occlusion == 0) {
                                        const auto pixel = fixture.Pixel(kWidth / 2, kHeight / 2);
                                        if (uv_scale < 1) {
                                            magnified_center = pixel;
                                        } else {
                                            EXPECT_GT(magnified_center[0], pixel[0]);
                                            EXPECT_GT(pixel[2], magnified_center[2]);
                                        }
                                    }
                                    ++index;
                                    gfx::DestroyMesh(mesh);
                                }
                            }
                        }
                    }
                }
            }
        }
        for (unsigned stress = 0; stress < 4; ++stress) {
            auto vertices = ScreenVertices({0, 0, 1});
            for (auto &vertex : vertices) {
                vertex.uv[0] = (vertex.position[0] + 1) * .37f;
                vertex.uv[1] = (vertex.position[1] + 1) * .29f;
            }
            if (stress == 2) {
                vertices[0].position[1] = -.03125f;
                vertices[1].position[1] = vertices[2].position[1] = .0625f;
            }
            const auto mesh = gfx::CreateMesh(vertices, kTriangle);
            auto       draw = SharedRgbBlackDraw(false);
            draw.mesh.mvp[3] = 1;
            if (stress == 1) {
                draw.mesh.mvp[0] = draw.mesh.mvp[5] = 1;
                draw.mesh.mvp[3] = 2;
                draw.mesh.mvp[15] = .5f;
                draw.mesh.mvp[14] = 1.25f;
            }
            if (stress == 3) {
                draw.mesh.mvp[2] = 2;
                draw.mesh.mvp[14] = 4;
            }
            const auto *uploaded = gfx::detail::LookupMesh(mesh);
            ASSERT_NE(uploaded, nullptr);
            const bool safe = gfx::detail::Prog0FogIsNearSafe(*uploaded, draw.mesh, draw.prog0);
            ASSERT_FALSE(stress == 3 && safe);
            const size_t           family = safe ? 0 : 1;
            SharedRgbGuardFallback fallback(stress == 3);
            gfx::DrawState         state;
            state.blend = state.fog = true;
            state.alpha = {0, 2, 0, 1, 0};
            const auto before = selected;
            fixture.Frame(kClear, [&] { gfx::DrawMeshProg0(mesh, 0, 3, draw.mesh, draw.prog0, NearestBinding(single_texture), state); });
            EXPECT_EQ(selected[family] - before[family], enabled ? 1u : 0u);
            size_t visible = 0;
            for (size_t p = 0; p < fixture.pixels.size(); p += 4) {
                visible += fixture.pixels[p] != kClear[0] || fixture.pixels[p + 1] != kClear[1] || fixture.pixels[p + 2] != kClear[2];
            }
            EXPECT_GT(visible, 0u) << stress;
            if (!enabled) {
                reference.push_back(fixture.pixels);
            } else {
                EXPECT_EQ(fixture.pixels, reference[index]);
            }
            ++index;
            gfx::DestroyMesh(mesh);
        }
        gfx::DestroyTexture(single_texture);
        gfx::DestroyTexture(texture);
    }
}
} // namespace

TEST(GfxGpuMeshDc2, Prog0SharedRgbPreservesPixelsAndGuardedFallback) { CheckSharedRgbPixels(false); }

/**
 * Compares the rigid unlit vertex sibling against the original shared-input shader.
 */
TEST(GfxGpuMeshDc2, Prog0UnlitVertexPreservesPixelsAndGuardedFallback) { CheckSharedRgbPixels(true); }

/**
 * Retains final state exclusions, six clipping planes and zero-path priority across independent lifetimes.
 */
namespace {
void CheckSharedRgbState(bool unlit_vertex) {
    EXPECT_EQ(gfx::detail::DescribePipeline(gfx::detail::kProg0NearSafeUnlitVertexScope), "prog0-fog-near-safe-additive-black-shared-rgb-unlit-vertex");
    EXPECT_EQ(gfx::detail::DescribePipeline(gfx::detail::kProg0Mode1UnlitVertexScope), "prog0-fog-mode1-fresh-black-shared-rgb-unlit-vertex");
    EXPECT_EQ(gfx::detail::DescribePipeline(gfx::detail::kProg0GuardedUnlitVertexScope), "prog0-guarded-zero-source-alpha-shared-rgb-unlit-vertex");
    EXPECT_EQ(gfx::detail::DescribePipeline(gfx::detail::kProg0NearSafeFogSharedRgbScope), "prog0-fog-near-safe-additive-black-shared-rgb");
    EXPECT_EQ(gfx::detail::DescribePipeline(gfx::detail::kProg0Mode1FogSharedRgbScope), "prog0-fog-mode1-fresh-black-shared-rgb");
    EXPECT_EQ(gfx::detail::DescribePipeline(gfx::detail::kProg0GuardedZeroSharedRgbScope), "prog0-guarded-zero-source-alpha-shared-rgb");
    SharedRgbPolicies                 policies;
    std::vector<std::vector<uint8_t>> reference;
    for (bool enabled : {false, true}) {
        ASSERT_EQ(SetEnv("KZ_PROG0_SHARED_RGB", unlit_vertex || enabled ? "1" : "0", 1), 0);
        ASSERT_EQ(SetEnv("KZ_PROG0_UNLIT_VERTEX", unlit_vertex && enabled ? "1" : "0", 1), 0);
        GfxFixture                    fixture(kWidth, kHeight, 1, Prog0Options());
        auto &selected = unlit_vertex ? gfx::detail::g.prog0_unlit_vertex_selected : gfx::detail::g.prog0_shared_rgb_selected;
        const auto &pipelines = unlit_vertex ? gfx::detail::g.prog0_unlit_vertex_pipelines : gfx::detail::g.prog0_shared_rgb_pipelines;
        EXPECT_EQ(gfx::detail::g.prog0_unlit_vertex, unlit_vertex && enabled);
        const auto                    texture = gfx::CreateTexture({2, 2, gfx::TextureFormat::Rgba8, 1, true});
        const std::array<uint32_t, 4> pixels = {Rgba(211, 41, 83, 255), Rgba(31, 193, 59, 191), Rgba(43, 71, 229, 127), Rgba(197, 109, 47, 223)};
        ASSERT_TRUE(gfx::UpdateTexture(texture, 0, 0, 0, 2, 2, pixels.data()));
        const auto mesh = gfx::CreateMesh(ScreenVertices({0, 0, 1}), kTriangle);
        size_t     index = 0;
        for (bool crossing : {false, true}) {
            for (unsigned variant = 0; variant < 29; ++variant) {
                SCOPED_TRACE(std::format("enabled{} crossing{} variant{}", enabled, crossing, variant));
                auto           draw = SharedRgbBlackDraw(crossing);
                gfx::DrawState state;
                state.blend = state.fog = true;
                state.alpha = {0, 2, 0, 1, 0};
                state.depth_test = gfx::DepthTest::GEqual;
                state.alpha_test = true;
                state.alpha_func = gfx::AlphaFunc::GEqual;
                state.alpha_ref = 0;
                bool expected = variant <= 4 || variant == 10 || (variant >= 11 && variant < 17) || variant >= 27;
                if (variant == 1) {
                    state.depth_write = true;
                }
                if (variant == 2) {
                    state.stencil_test = true;
                    state.stencil_front.pass = state.stencil_back.pass = gfx::StencilOp::Replace;
                    state.stencil_front.reference = state.stencil_back.reference = 7;
                }
                if (variant == 3) {
                    state.color_write_mask = gfx::kWriteRed | gfx::kWriteGreen | gfx::kWriteBlue;
                }
                if (variant == 4) {
                    state.color_write_mask = gfx::kWriteAlpha;
                }
                if (variant == 5) {
                    state.color_write_mask = 0;
                }
                if (variant == 6) {
                    state.fog_color[0] = 1;
                }
                if (variant == 7) {
                    state.alpha = {0, 1, 0, 1, 0};
                }
                if (variant == 8) {
                    state.alpha_ref = 1;
                }
                if (variant == 9) {
                    draw = SharedRgbBlackDraw(false);
                    draw.prog0.diffuse[3] = .005984187126159668f;
                }
                if (variant == 10) {
                    draw = SharedRgbBlackDraw(true);
                    draw.prog0.diffuse[3] = .005984187126159668f;
                }
                if (variant >= 11 && variant < 17) {
                    const auto  plane = variant - 11, axis = plane / 2;
                    const float sign = (plane & 1) ? -1.f : 1.f;
                    draw.prog0.flags |= gfx::kProg0Clip | gfx::kProg0Scissor;
                    std::fill_n(draw.prog0.clip_or_view, 16, 0.f);
                    draw.prog0.clip_or_view[axis] = sign;
                    draw.prog0.clip_or_view[12 + axis] = 2 * sign;
                    draw.prog0.clip_or_view[15] = 2;
                }
                auto binding = NearestBinding(texture);
                if (variant >= 17 && variant < 23) {
                    float &operand = variant < 20 ? draw.prog0.object_color[variant - 17] : draw.prog0.diffuse[variant - 20];
                    operand = std::bit_cast<float>(std::bit_cast<uint32_t>(operand) + 1);
                }
                if (variant == 23) {
                    draw.prog0.flags |= gfx::kProg0PointLights;
                }
                if (variant == 24) {
                    draw.prog0.flags = 0;
                }
                if (variant == 25) {
                    state.prog0_face = gfx::kProg0FaceNoNormal;
                }
                if (variant == 26) {
                    draw.prog0.flags |= gfx::kProg0Skinned;
                }
                auto alternate_mesh = variant == 26 ? gfx::CreateMesh(Skinned(ScreenVertices({0, 0, 1}), {1, 0, 0, 0}, {0, 0, 0, 0}), kTriangle) : gfx::kNullMesh;
                if (variant >= 27) {
                    auto thin = ScreenVertices({0, 0, 1});
                    thin[0].position[1] = variant == 27 ? -.125f : 0;
                    thin[1].position[1] = thin[2].position[1] = variant == 27 ? .125f : .03125f;
                    alternate_mesh = gfx::CreateMesh(thin, kTriangle);
                }
                for (size_t bone = 0; bone < gfx::kProg0Bones; ++bone) {
                    draw.palette.bones[bone][0][0] = draw.palette.bones[bone][1][1] = draw.palette.bones[bone][2][2] = 1;
                }
                const auto                   before = selected;
                const auto                   zero_before = gfx::detail::g.prog0_zero_source_alpha_selected;
                const auto                   guard_before = gfx::detail::g.prog0_guarded_zero_alpha_selected;
                const std::array<uint8_t, 4> clear = {17, 31, 47, 91};
                fixture.Frame(clear, [&] {
                    gfx::ClearStencil(0);
                    gfx::DrawMeshProg0(alternate_mesh != gfx::kNullMesh ? alternate_mesh : mesh, 0, 3, draw.mesh, draw.prog0, binding, state, nullptr, variant == 26 ? &draw.palette : nullptr);
                    if (variant == 0) {
                        gfx::DrawState consumer;
                        consumer.depth_test = gfx::DepthTest::GEqual;
                        const auto depth_probe = Quad(0, 0, gfx::LogicalWidth(), gfx::LogicalHeight(), {37, 83, 191, 128}, 0, 0, 0, 0, .25f);
                        gfx::Draw2D(gfx::Primitive::Quads, depth_probe, {}, consumer);
                        consumer.stencil_test = true;
                        consumer.stencil_front.compare = consumer.stencil_back.compare = gfx::CompareOp::Equal;
                        const auto unchanged_stencil = Quad(0, 0, gfx::LogicalWidth(), gfx::LogicalHeight(), {89, 127, 59, 128}, 0, 0, 0, 0, .25f);
                        gfx::Draw2D(gfx::Primitive::Quads, unchanged_stencil, {}, consumer);
                        consumer.stencil_front.compare = consumer.stencil_back.compare = gfx::CompareOp::NotEqual;
                        const auto rejected_stencil = Quad(0, 0, gfx::LogicalWidth(), gfx::LogicalHeight(), {255, 0, 0, 128}, 0, 0, 0, 0, .25f);
                        gfx::Draw2D(gfx::Primitive::Quads, rejected_stencil, {}, consumer);
                    }
                    if (variant == 1 || variant == 2) {
                        gfx::DrawState consumer;
                        consumer.depth_test = gfx::DepthTest::GEqual;
                        if (variant == 2) {
                            consumer.stencil_test = true;
                            consumer.stencil_front.compare = consumer.stencil_back.compare = gfx::CompareOp::Equal;
                            consumer.stencil_front.reference = consumer.stencil_back.reference = 7;
                        }
                        const auto quad = Quad(0, 0, gfx::LogicalWidth(), gfx::LogicalHeight(), {37, 83, 191, 128}, 0, 0, 0, 0, variant == 1 ? .25f : .95f);
                        gfx::Draw2D(gfx::Primitive::Quads, quad, {}, consumer);
                    }
                });
                const size_t selected_family = variant == 10 ? 2 : crossing ? 1
                                                                            : 0;
                EXPECT_EQ(selected[selected_family] - before[selected_family], enabled && expected ? 1u : 0u);
                EXPECT_EQ(selected[0] - before[0] + selected[1] - before[1] + selected[2] - before[2], enabled && expected ? 1u : 0u);
                EXPECT_EQ(gfx::detail::g.prog0_zero_source_alpha_selected - zero_before, variant == 9 ? 1u : 0u);
                EXPECT_EQ(gfx::detail::g.prog0_guarded_zero_alpha_selected - guard_before, variant == 10 ? 1u : 0u);
                if (!enabled) {
                    reference.push_back(fixture.pixels);
                } else {
                    EXPECT_EQ(fixture.pixels, reference[index]);
                }
                if (variant == 0) {
                    EXPECT_EQ(fixture.Pixel(kWidth / 2, kHeight / 2)[0], 89u);
                    EXPECT_EQ(fixture.Pixel(kWidth / 2, kHeight / 2)[1], 127u);
                    EXPECT_EQ(fixture.Pixel(kWidth / 2, kHeight / 2)[2], 59u);
                }
                if (variant == 1) {
                    EXPECT_NE(fixture.Pixel(kWidth / 2, kHeight / 2)[2], 191u);
                    EXPECT_NE(fixture.Pixel(kWidth / 2, kHeight / 2)[0], clear[0]);
                }
                if (variant == 2) {
                    EXPECT_EQ(fixture.Pixel(kWidth / 2, kHeight / 2)[2], 191u);
                }
                if (variant >= 11 && variant < 17) {
                    EXPECT_NE(fixture.Pixel(kWidth / 4, kHeight / 2)[0], clear[0]);
                    EXPECT_EQ(fixture.Pixel(3 * kWidth / 4, kHeight / 2)[0], clear[0]);
                }
                if (variant == 10) {
                    const auto pixel = fixture.Pixel(kWidth / 2, kHeight / 2);
                    EXPECT_EQ(pixel[0], clear[0]);
                    EXPECT_EQ(pixel[1], clear[1]);
                    EXPECT_EQ(pixel[2], clear[2]);
                    EXPECT_EQ(pixel[3], 0u);
                }
                if (variant >= 27) {
                    size_t covered = 0;
                    for (size_t p = 0; p < fixture.pixels.size(); p += 4) {
                        covered += fixture.pixels[p] != clear[0] || fixture.pixels[p + 1] != clear[1] || fixture.pixels[p + 2] != clear[2];
                    }
                    EXPECT_GT(covered, 0u);
                }
                if (alternate_mesh != gfx::kNullMesh) {
                    gfx::DestroyMesh(alternate_mesh);
                }
                ++index;
            }
        }
        gfx::DestroyMesh(mesh);
        gfx::DestroyTexture(texture);
    }
}
} // namespace

TEST(GfxGpuMeshDc2, Prog0SharedRgbRequiresExactMaterialAndPreservesState) { CheckSharedRgbState(false); }

/**
 * Compares the rigid unlit vertex sibling against the original shared-input shader.
 */
TEST(GfxGpuMeshDc2, Prog0UnlitVertexRequiresExactMaterialAndPreservesState) { CheckSharedRgbState(true); }

/**
 * Preserves actual canonical and interpolated replay matrices through eager, lazy and disabled lifetimes.
 */
namespace {
void CheckSharedRgbLifetimes(bool unlit_vertex) {
    SharedRgbPolicies                 policies;
    std::vector<std::vector<uint8_t>> reference;
    for (unsigned lifetime = 0; lifetime < 4; ++lifetime) {
        const bool enabled = lifetime == 1 || lifetime == 2;
        ASSERT_EQ(SetEnv("KZ_PROG0_SHARED_RGB", unlit_vertex || enabled ? "1" : "0", 1), 0);
        ASSERT_EQ(SetEnv("KZ_PROG0_UNLIT_VERTEX", unlit_vertex && enabled ? "1" : "0", 1), 0);
        ASSERT_EQ(SetEnv("KZ_PIPELINE_PREWARM", lifetime == 2 ? "1" : "0", 1), 0);
        GfxFixture fixture(kWidth, kHeight, 1, Prog0Options());
        auto &selected = unlit_vertex ? gfx::detail::g.prog0_unlit_vertex_selected : gfx::detail::g.prog0_shared_rgb_selected;
        const auto &pipelines = unlit_vertex ? gfx::detail::g.prog0_unlit_vertex_pipelines : gfx::detail::g.prog0_shared_rgb_pipelines;
        EXPECT_EQ(gfx::detail::g.prog0_unlit_vertex, unlit_vertex && enabled);
        for (auto pipeline : pipelines) {
            EXPECT_EQ(pipeline != VK_NULL_HANDLE, enabled);
        }
        ASSERT_EQ(SetEnv("KZ_PROG0_SHARED_RGB", enabled ? "0" : "1", 1), 0);
        ASSERT_EQ(SetEnv("KZ_PROG0_UNLIT_VERTEX", unlit_vertex && enabled ? "0" : "1", 1), 0);
        EXPECT_EQ(gfx::detail::g.prog0_unlit_vertex, unlit_vertex && enabled);
        EXPECT_EQ(gfx::detail::g.prog0_shared_rgb, unlit_vertex || enabled);
        const auto                    mesh = gfx::CreateMesh(ScreenVertices({0, 0, 1}), kTriangle);
        const auto                    texture = gfx::CreateTexture({2, 2, gfx::TextureFormat::Rgba8, 1, true});
        const std::array<uint32_t, 4> pixels = {Rgba(211, 41, 83, 255), Rgba(31, 193, 59, 191), Rgba(43, 71, 229, 127), Rgba(197, 109, 47, 223)};
        ASSERT_TRUE(gfx::UpdateTexture(texture, 0, 0, 0, 2, 2, pixels.data()));
        size_t index = 0;
        for (bool guarded : {false, true}) {
            for (bool crossing : {false, true, false}) {
                if (guarded && !crossing) {
                    continue;
                }
                const auto record = [&](bool current) {
                    auto transform = gfx::IdentityMeshTransform();
                    transform.projection[0] = transform.projection[5] = transform.projection[15] = 4;
                    transform.projection[14] = crossing ? 2.5f : 2.f;
                    transform.projection[2] = crossing ? 1.f : 0.f;
                    transform.model[12] = current ? .2f : -.2f;
                    gfx::detail::Mat4 projection, model;
                    std::copy_n(transform.projection, 16, projection.begin());
                    std::copy_n(transform.model, 16, model.begin());
                    const auto mvp = gfx::detail::Multiply(projection, model);
                    auto       draw = SharedRgbBlackDraw(crossing);
                    if (guarded) {
                        draw.prog0.diffuse[3] = .005984187126159668f;
                    }
                    std::copy(mvp.begin(), mvp.end(), draw.mesh.mvp);
                    gfx::DrawState state;
                    state.blend = state.fog = true;
                    state.alpha = {0, 2, 0, 1, 0};
                    state.depth_test = gfx::DepthTest::GEqual;
                    gfx::BeginRecording();
                    gfx::Clear(true, kClear.data(), true, 0);
                    gfx::SetInterpKey(9271);
                    gfx::DrawMeshProg0(mesh, 0, 3, draw.mesh, draw.prog0, NearestBinding(texture), state, &transform);
                    return gfx::EndRecording();
                };
                const auto previous = record(false), current = record(true);
                for (float alpha : {1.f, .5f}) {
                    const auto before = selected;
                    ASSERT_TRUE(gfx::RenderList(*current, alpha, {.previous = alpha < 1 ? previous.get() : nullptr, .canonical = alpha == 1, .present = unlit_vertex && alpha < 1}));
                    SCOPED_TRACE(std::format("lifetime{} crossing{} alpha{} near_delta{} mode_delta{}", lifetime, crossing, alpha,
                                             selected[0] - before[0], selected[1] - before[1]));
                    const size_t family = guarded ? 2 : crossing ? 1
                                                                 : 0;
                    EXPECT_EQ(selected[family] - before[family], enabled ? 1u : 0u);
                    ASSERT_TRUE(gfx::ReadbackFrame(fixture.pixels, fixture.width, fixture.height));
                    if (guarded) {
                        EXPECT_EQ(fixture.Pixel(kWidth / 4, kHeight / 2)[3], 0u);
                    } else {
                        EXPECT_GT(fixture.Pixel(kWidth / 4, kHeight / 2)[0], kClear[0]);
                    }
                    if (lifetime == 0) {
                        reference.push_back(fixture.pixels);
                    } else {
                        EXPECT_EQ(fixture.pixels, reference[index]);
                    }
                    ++index;
                }
            }
        }
        gfx::DestroyMesh(mesh);
        gfx::DestroyTexture(texture);
    }
}
} // namespace

TEST(GfxGpuMeshDc2, Prog0SharedRgbFollowsRecordedMatricesAndPipelineLifetimes) { CheckSharedRgbLifetimes(false); }

/**
 * Compares the rigid unlit vertex sibling against the original shared-input shader.
 */
TEST(GfxGpuMeshDc2, Prog0UnlitVertexFollowsRecordedMatricesAndPipelineLifetimes) { CheckSharedRgbLifetimes(true); }

namespace {
/**
 * Restores the caller's identity-bound diagnostic mode.
 */
struct RgbCostPolicy {
    EnvSnapshot mode = SaveEnv("KZ_PROG0_RGB_COST_DIAGNOSTIC"); /**< Original diagnostic request. */
    EnvSnapshot shared = SaveEnv("KZ_PROG0_SHARED_RGB");        /**< Original shared-input request. */

    /**
     * Enables retained equal-input pipelines for diagnostic comparisons.
     */
    RgbCostPolicy() { SetEnv("KZ_PROG0_SHARED_RGB", "1", 1); }

    /**
     * Restores both requests after all renderer lifetimes end.
     */
    ~RgbCostPolicy() {
        RestoreEnv(mode);
        RestoreEnv(shared);
    }
};

/**
 * Builds the ordinary additive state without changing alpha or depth policy.
 */
gfx::DrawState RgbCostState() {
    gfx::DrawState state;
    state.blend = state.fog = true;
    state.alpha = {0, 2, 0, 1, 0};
    state.depth_test = gfx::DepthTest::GEqual;
    return state;
}

/**
 * Verifies that identity-only execution never arms trace observers.
 */
void ExpectIdentityOnly() {
    const auto stats = gfx::detail::InspectAttachmentAlphaTrace();
    EXPECT_FALSE(stats.enabled);
    EXPECT_EQ(stats.epoch, 0u);
    EXPECT_EQ(stats.retained, 0u);
    EXPECT_EQ(stats.observer_capacity, 0u);
    EXPECT_EQ(stats.census_capacity, 0u);
    EXPECT_FALSE(stats.prepared_candidate);
    EXPECT_FALSE(gfx::detail::AttachmentAlphaTraceRecording());
}
} // namespace

/**
 * Compares live sampled alpha and masked full pixels across identical catalog-backed setup.
 */
TEST(GfxGpuMeshDc2, Prog0RgbCostModesPreserveAlphaAndMaskedPixels) {
    SharedRgbPolicies parent;
    RgbCostPolicy     policies;
    for (float uv_scale : {.25f, 17.25f}) {
        std::vector<std::vector<uint8_t>> control, masked;
        for (unsigned mode = 0; mode < 4; ++mode) {
            AlphaTraceTestInput input(false, true, uv_scale);
            ASSERT_EQ(SetEnv("KZ_PROG0_RGB_COST_DIAGNOSTIC", std::to_string(mode).c_str(), 1), 0);
            GfxFixture fixture(kWidth, kHeight, 1, Prog0Options());
            EXPECT_EQ(static_cast<unsigned>(gfx::detail::g.prog0_rgb_cost_mode), mode);
            for (auto pipeline : gfx::detail::g.prog0_rgb_cost_pipelines) {
                EXPECT_EQ(pipeline != VK_NULL_HANDLE, mode != 0);
            }
            EXPECT_EQ(gfx::detail::AttachmentAlphaIdentityReady(), mode != 0);
            if (mode == 0) {
                EXPECT_EQ(gfx::detail::g.alpha_trace, nullptr);
            }
            const auto mesh = gfx::CreateMesh(input.vertices, kTriangle);
            const auto texture = gfx::CreateTexture({2, 2, gfx::TextureFormat::Rgba8, 1, true});
            ASSERT_TRUE(gfx::UpdateTexture(texture, 0, 0, 0, 2, 2, input.pixels.data()));
            fixture.Frame(kClear, [] {});
            const auto clear_alpha = fixture.pixels[3];
            gfx::BeginAttachmentAlphaTrace(AlphaTestNow());
            size_t index = 0;
            for (bool crossing : {false, true}) {
                for (auto filter : {gfx::Filter::Nearest, gfx::Filter::Linear}) {
                    for (auto wrap : {gfx::Wrap::Clamp, gfx::Wrap::Repeat}) {
                        for (unsigned scenario = 0; scenario < 10; ++scenario) {
                            SCOPED_TRACE(std::format("mode{} uv{} crossing{} filter{} wrap{} scenario{}", mode, uv_scale, crossing, static_cast<int>(filter), static_cast<int>(wrap), scenario));
                            auto draw = SharedRgbBlackDraw(crossing);
                            draw.prog0.object_color[3] = 64;
                            auto state = RgbCostState();
                            if (scenario == 3) {
                                state.stencil_test = true;
                                state.stencil_front.compare = state.stencil_back.compare = gfx::CompareOp::Equal;
                                state.stencil_front.reference = state.stencil_back.reference = 7;
                            }
                            if (scenario >= 4) {
                                const auto  plane = scenario - 4, axis = plane / 2;
                                const float sign = (plane & 1) ? -1.f : 1.f;
                                draw.prog0.flags |= gfx::kProg0Clip | gfx::kProg0Scissor;
                                std::fill_n(draw.prog0.clip_or_view, 16, 0.f);
                                draw.prog0.clip_or_view[axis] = sign;
                                draw.prog0.clip_or_view[12 + axis] = 2 * sign;
                                draw.prog0.clip_or_view[15] = 2;
                            }
                            auto binding = NearestBinding(texture);
                            binding.filter = filter;
                            binding.wrap_u = binding.wrap_v = wrap;
                            const auto before = gfx::detail::g.prog0_rgb_cost_selected;
                            fixture.Frame(kClear, [&] {
                                gfx::ClearStencil(0);
                                if (scenario == 1 || scenario == 2) {
                                    gfx::DrawState cover;
                                    cover.depth_write = true;
                                    const auto quad = Quad(0, 0, gfx::LogicalWidth() / (scenario == 2 ? 2 : 1), gfx::LogicalHeight(), {71, 23, 41, 128}, 0, 0, 0, 0, 1);
                                    gfx::Draw2D(gfx::Primitive::Quads, quad, {}, cover);
                                }
                                gfx::DrawMeshProg0(mesh, 0, 3, draw.mesh, draw.prog0, binding, state);
                                draw.prog0.object_color[3] = 96;
                                gfx::DrawMeshProg0(mesh, 0, 3, draw.mesh, draw.prog0, binding, state);
                            });
                            const size_t family = crossing ? 1 : 0;
                            EXPECT_EQ(gfx::detail::g.prog0_rgb_cost_selected[family] - before[family], mode ? 2u : 0u);
                            EXPECT_EQ(gfx::detail::g.counters.mesh_draws, 2u);
                            if (mode == 0) {
                                control.push_back(fixture.pixels);
                            } else {
                                ASSERT_EQ(fixture.pixels.size(), control[index].size());
                                for (size_t a = 3; a < fixture.pixels.size(); a += 4) {
                                    EXPECT_EQ(fixture.pixels[a], control[index][a]);
                                }
                                if (mode == 1) {
                                    EXPECT_EQ(fixture.pixels, control[index]);
                                }
                            }
                            if (mode == 2) {
                                masked.push_back(fixture.pixels);
                            }
                            if (mode == 3) {
                                EXPECT_EQ(fixture.pixels, masked[index]);
                            }
                            if (scenario == 0) {
                                size_t                live_alpha = 0;
                                std::array<bool, 256> levels{};
                                bool                  intermediate = false;
                                for (size_t a = 3; a < fixture.pixels.size(); a += 4) {
                                    const auto value = fixture.pixels[a];
                                    live_alpha += value != clear_alpha;
                                    if (value != clear_alpha) {
                                        levels[value] = true;
                                    }
                                    intermediate |= value != clear_alpha && value > 0 && value < 255;
                                }
                                EXPECT_GT(live_alpha, 0u);
                                if (uv_scale > 1 && wrap == gfx::Wrap::Repeat) {
                                    EXPECT_GE(std::count(levels.begin(), levels.end(), true), 2);
                                    EXPECT_TRUE(intermediate);
                                }
                            }
                            ++index;
                        }
                    }
                }
            }
            for (bool mixed_w : {false, true}) {
                auto draw = SharedRgbBlackDraw(false);
                draw.prog0.object_color[3] = 64;
                draw.mesh.mvp[3] = 1;
                if (mixed_w) {
                    draw.mesh.mvp[0] = draw.mesh.mvp[5] = 1;
                    draw.mesh.mvp[3] = 2;
                    draw.mesh.mvp[15] = .5f;
                    draw.mesh.mvp[14] = 1.25f;
                }
                const auto *uploaded = gfx::detail::LookupMesh(mesh);
                ASSERT_NE(uploaded, nullptr);
                const size_t family = gfx::detail::Prog0FogIsNearSafe(*uploaded, draw.mesh, draw.prog0) ? 0 : 1;
                const auto   before = gfx::detail::g.prog0_rgb_cost_selected;
                fixture.Frame(kClear, [&] { gfx::DrawMeshProg0(mesh, 0, 3, draw.mesh, draw.prog0, NearestBinding(texture), RgbCostState()); });
                EXPECT_EQ(gfx::detail::g.prog0_rgb_cost_selected[family] - before[family], mode ? 1u : 0u);
                if (mode == 0) {
                    control.push_back(fixture.pixels);
                } else {
                    for (size_t a = 3; a < fixture.pixels.size(); a += 4) {
                        EXPECT_EQ(fixture.pixels[a], control[index][a]);
                    }
                    if (mode == 1) {
                        EXPECT_EQ(fixture.pixels, control[index]);
                    }
                }
                if (mode == 2) {
                    masked.push_back(fixture.pixels);
                }
                if (mode == 3) {
                    EXPECT_EQ(fixture.pixels, masked[index]);
                }
                size_t covered = 0;
                for (size_t a = 3; a < fixture.pixels.size(); a += 4) {
                    covered += fixture.pixels[a] != clear_alpha;
                }
                EXPECT_GT(covered, 0u);
                ++index;
            }
            gfx::EndAttachmentAlphaTrace(AlphaTestNow());
            ExpectIdentityOnly();
            gfx::DestroyMesh(mesh);
            gfx::DestroyTexture(texture);
        }
    }
}

/**
 * Preserves depth and stencil consumers and restores full writes after every unmatched draw.
 */
TEST(GfxGpuMeshDc2, Prog0RgbCostPreservesConsumersAndRestoresMask) {
    SharedRgbPolicies                 parent;
    RgbCostPolicy                     policies;
    std::vector<std::vector<uint8_t>> reference;
    std::vector<std::vector<uint8_t>> masked;
    for (unsigned mode = 1; mode < 4; ++mode) {
        AlphaTraceTestInput input(false, true);
        ASSERT_EQ(SetEnv("KZ_PROG0_RGB_COST_DIAGNOSTIC", std::to_string(mode).c_str(), 1), 0);
        GfxFixture fixture(kWidth, kHeight, 1, Prog0Options());
        const auto mesh = gfx::CreateMesh(input.vertices, kTriangle);
        auto       other_vertices = input.vertices;
        other_vertices[0].uv[0] = .03125f;
        const auto other = gfx::CreateMesh(other_vertices, kTriangle);
        const auto texture = gfx::CreateTexture({2, 2, gfx::TextureFormat::Rgba8, 1, true});
        ASSERT_TRUE(gfx::UpdateTexture(texture, 0, 0, 0, 2, 2, input.pixels.data()));
        size_t index = 0;
        for (bool crossing : {false, true}) {
            for (unsigned scenario = 0; scenario < 4; ++scenario) {
                auto draw = SharedRgbBlackDraw(crossing);
                auto state = RgbCostState();
                if (scenario == 1) {
                    state.depth_write = true;
                }
                if (scenario == 2) {
                    state.stencil_test = true;
                    state.stencil_front.pass = state.stencil_back.pass = gfx::StencilOp::Replace;
                    state.stencil_front.reference = state.stencil_back.reference = 7;
                }
                const auto before = gfx::detail::g.prog0_rgb_cost_selected;
                fixture.Frame(kClear, [&] {
                    gfx::ClearStencil(0);
                    gfx::DrawMeshProg0(mesh, 0, 3, draw.mesh, draw.prog0, NearestBinding(texture), state);
                    if (scenario == 3) {
                        auto ordinary = RgbCostState();
                        ordinary.scissor = true;
                        ordinary.scissor_rect = {static_cast<int32_t>(gfx::LogicalWidth() / 2), 0, static_cast<int32_t>(gfx::LogicalWidth() / 2), static_cast<int32_t>(gfx::LogicalHeight())};
                        gfx::DrawMeshProg0(other, 0, 3, draw.mesh, draw.prog0, NearestBinding(texture), ordinary);
                    } else {
                        gfx::DrawState consumer;
                        consumer.depth_test = gfx::DepthTest::GEqual;
                        consumer.stencil_test = true;
                        consumer.stencil_front.compare = consumer.stencil_back.compare = gfx::CompareOp::Equal;
                        consumer.stencil_front.reference = consumer.stencil_back.reference = scenario == 2 ? 7 : 0;
                        const auto quad = Quad(0, 0, gfx::LogicalWidth(), gfx::LogicalHeight(), {89, 127, 59, 128}, 0, 0, 0, 0, scenario == 1 ? .25f : .95f);
                        gfx::Draw2D(gfx::Primitive::Quads, quad, {}, consumer);
                        if (scenario == 0) {
                            consumer.stencil_front.compare = consumer.stencil_back.compare = gfx::CompareOp::NotEqual;
                            const auto rejected = Quad(0, 0, gfx::LogicalWidth(), gfx::LogicalHeight(), {255, 0, 0, 128}, 0, 0, 0, 0, .95f);
                            gfx::Draw2D(gfx::Primitive::Quads, rejected, {}, consumer);
                        }
                    }
                });
                EXPECT_EQ(gfx::detail::g.prog0_rgb_cost_selected[crossing ? 1 : 0] - before[crossing ? 1 : 0], 1u);
                if (mode == 1) {
                    reference.push_back(fixture.pixels);
                } else if (scenario != 1 && scenario != 3) {
                    EXPECT_EQ(fixture.pixels, reference[index]);
                }
                if (mode == 2) {
                    masked.push_back(fixture.pixels);
                }
                if (mode == 3) {
                    EXPECT_EQ(fixture.pixels, masked[index]);
                }
                if (scenario == 0 || scenario == 2) {
                    EXPECT_EQ(fixture.Pixel(kWidth / 4, kHeight / 2)[0], 89u);
                    EXPECT_EQ(fixture.Pixel(kWidth / 4, kHeight / 2)[1], 127u);
                }
                if (scenario == 1) {
                    EXPECT_NE(fixture.Pixel(kWidth / 4, kHeight / 2)[0], 89u);
                }
                if (scenario == 3) {
                    EXPECT_NE(fixture.Pixel(3 * kWidth / 4, kHeight / 2)[0], kClear[0]);
                    if (mode > 1) {
                        EXPECT_EQ(fixture.Pixel(kWidth / 4, kHeight / 2)[0], kClear[0]);
                    }
                }
                ++index;
            }
        }
        ExpectIdentityOnly();
        gfx::DestroyMesh(other);
        gfx::DestroyMesh(mesh);
        gfx::DestroyTexture(texture);
    }
}

/**
 * Rejects stale or unrelated identities and preserves actual recorded execution matching.
 */
TEST(GfxGpuMeshDc2, Prog0RgbCostIdentityAndRecordedLifetimes) {
    SharedRgbPolicies                 parent;
    RgbCostPolicy                     policies;
    std::vector<std::vector<uint8_t>> recorded_control, recorded_mask;
    for (unsigned mode : {0u, 1u, 2u, 3u, 0u}) {
        AlphaTraceTestInput input(false, true);
        SetEnv("KZ_PROG0_RGB_COST_DIAGNOSTIC", std::to_string(mode).c_str(), 1);
        GfxFixture fixture(kWidth, kHeight, 1, Prog0Options());
        const auto texture = gfx::CreateTexture({2, 2, gfx::TextureFormat::Rgba8, 1, true});
        ASSERT_TRUE(gfx::UpdateTexture(texture, 0, 0, 0, 2, 2, input.pixels.data()));
        fixture.Frame(kClear, [] {});
        const auto clear_alpha = fixture.pixels[3];
        auto       mesh = gfx::CreateMesh(input.vertices, kTriangle);
        const auto draw = [&](gfx::MeshHandle handle, uint32_t first = 0, uint32_t count = 3) {
            auto       data = SharedRgbBlackDraw(false);
            auto       state = RgbCostState();
            const auto before = gfx::detail::g.prog0_rgb_cost_selected[0];
            fixture.Frame(kClear, [&] { gfx::DrawMeshProg0(handle, first, count, data.mesh, data.prog0, NearestBinding(texture), state); });
            return gfx::detail::g.prog0_rgb_cost_selected[0] - before;
        };
        EXPECT_EQ(draw(mesh), mode ? 1u : 0u);
        EXPECT_EQ(draw(mesh, 0, 0), 0u);
        ASSERT_TRUE(gfx::UpdateTexture(texture, 0, 0, 0, 1, 1, input.pixels.data()));
        EXPECT_EQ(draw(mesh), 0u);
        ASSERT_TRUE(gfx::UpdateTexture(texture, 0, 0, 0, 2, 2, input.pixels.data()));
        EXPECT_EQ(draw(mesh), mode ? 1u : 0u);
        auto changed_pixels = input.pixels;
        changed_pixels[0] ^= 1;
        ASSERT_TRUE(gfx::UpdateTexture(texture, 0, 0, 0, 2, 2, changed_pixels.data()));
        EXPECT_EQ(draw(mesh), 0u);
        ASSERT_TRUE(gfx::UpdateTexture(texture, 0, 0, 0, 2, 2, input.pixels.data()));
        EXPECT_EQ(draw(mesh), mode ? 1u : 0u);
        const auto source = gfx::CreateTexture({2, 2, gfx::TextureFormat::Rgba8, 1, true});
        ASSERT_TRUE(gfx::UpdateTexture(source, 0, 0, 0, 2, 2, input.pixels.data()));
        ASSERT_TRUE(gfx::CopyTexture(source, {0, 0, 2, 2}, texture, 0, 0));
        EXPECT_EQ(draw(mesh), 0u);
        ASSERT_TRUE(gfx::UpdateTexture(texture, 0, 0, 0, 2, 2, input.pixels.data()));
        EXPECT_EQ(draw(mesh), mode ? 1u : 0u);
        gfx::DestroyTexture(source);
        auto changed = input.vertices;
        changed[0].position[0] += .03125f;
        ASSERT_TRUE(gfx::UpdateMeshVertices(mesh, 0, changed));
        EXPECT_EQ(draw(mesh), 0u);
        gfx::DestroyMesh(mesh);
        mesh = gfx::CreateMesh(input.vertices, kTriangle);
        EXPECT_EQ(draw(mesh), mode ? 1u : 0u);
        size_t recorded_index = 0;
        for (bool crossing : {false, true}) {
            const auto record = [&](float x) {
                auto data = SharedRgbBlackDraw(crossing);
                auto transform = gfx::IdentityMeshTransform();
                transform.model[12] = x;
                transform.projection[0] = transform.projection[5] = transform.projection[15] = 4;
                transform.projection[14] = crossing ? 2.5f : 2;
                transform.projection[2] = crossing ? 1 : 0;
                gfx::detail::Mat4 projection, model;
                std::copy_n(transform.projection, 16, projection.begin());
                std::copy_n(transform.model, 16, model.begin());
                const auto mvp = gfx::detail::Multiply(projection, model);
                std::copy(mvp.begin(), mvp.end(), data.mesh.mvp);
                gfx::BeginRecording();
                gfx::Clear(true, kClear.data(), true, 0);
                gfx::SetInterpKey(9272);
                gfx::DrawMeshProg0(mesh, 0, 3, data.mesh, data.prog0, NearestBinding(texture), RgbCostState(), &transform);
                return gfx::EndRecording();
            };
            const auto previous = record(-.2f), current = record(.2f);
            for (float alpha : {1.f, .5f}) {
                const auto before = gfx::detail::g.prog0_rgb_cost_selected[crossing ? 1 : 0];
                ASSERT_TRUE(gfx::RenderList(*current, alpha, {.canonical = alpha == 1, .previous = alpha < 1 ? previous.get() : nullptr}));
                EXPECT_EQ(gfx::detail::g.prog0_rgb_cost_selected[crossing ? 1 : 0] - before, mode ? 1u : 0u);
                ASSERT_TRUE(gfx::ReadbackFrame(fixture.pixels, fixture.width, fixture.height));
                if (mode == 0 && recorded_control.size() < 4) {
                    recorded_control.push_back(fixture.pixels);
                } else {
                    for (size_t a = 3; a < fixture.pixels.size(); a += 4) {
                        EXPECT_EQ(fixture.pixels[a], recorded_control[recorded_index][a]);
                    }
                    if (mode <= 1) {
                        EXPECT_EQ(fixture.pixels, recorded_control[recorded_index]);
                    }
                }
                if (mode == 2) {
                    recorded_mask.push_back(fixture.pixels);
                }
                if (mode == 3) {
                    EXPECT_EQ(fixture.pixels, recorded_mask[recorded_index]);
                }
                size_t covered = 0;
                for (size_t a = 3; a < fixture.pixels.size(); a += 4) {
                    covered += fixture.pixels[a] != clear_alpha;
                }
                EXPECT_GT(covered, 0u);
                ++recorded_index;
            }
        }
        gfx::EndAttachmentAlphaTrace(AlphaTestNow());
        ExpectIdentityOnly();
        gfx::DestroyMesh(mesh);
        gfx::DestroyTexture(texture);
    }
    {
        AlphaTraceTestInput input(false, true, 1, true, true);
        SetEnv("KZ_PROG0_RGB_COST_DIAGNOSTIC", "2", 1);
        GfxFixture fixture(kWidth, kHeight, 1, Prog0Options());
        const auto mesh = gfx::CreateMesh(input.vertices, input.indices);
        const auto texture = gfx::CreateTexture({2, 2, gfx::TextureFormat::Rgba8, 1, true});
        ASSERT_TRUE(gfx::UpdateTexture(texture, 0, 0, 0, 2, 2, input.pixels.data()));
        auto data = SharedRgbBlackDraw(false);
        for (uint32_t count : {6u, 3u}) {
            const auto before = gfx::detail::g.prog0_rgb_cost_selected[0];
            fixture.Frame(kClear, [&] { gfx::DrawMeshProg0(mesh, 0, count, data.mesh, data.prog0, NearestBinding(texture), RgbCostState()); });
            EXPECT_EQ(gfx::detail::g.prog0_rgb_cost_selected[0] - before, count == 6 ? 1u : 0u);
        }
        for (bool crossing : {false, true}) {
            auto zero = SharedRgbBlackDraw(crossing);
            zero.prog0.diffuse[3] = .005984187126159668f;
            const auto before = gfx::detail::g.prog0_rgb_cost_selected;
            const auto constant = gfx::detail::g.prog0_zero_source_alpha_selected;
            const auto guarded = gfx::detail::g.prog0_guarded_zero_alpha_selected;
            fixture.Frame(kClear, [&] { gfx::DrawMeshProg0(mesh, 0, 6, zero.mesh, zero.prog0, NearestBinding(texture), RgbCostState()); });
            EXPECT_EQ(gfx::detail::g.prog0_rgb_cost_selected, before);
            EXPECT_EQ(gfx::detail::g.prog0_zero_source_alpha_selected - constant, crossing ? 0u : 1u);
            EXPECT_EQ(gfx::detail::g.prog0_guarded_zero_alpha_selected - guarded, crossing ? 1u : 0u);
        }
        std::vector<gfx::MeshHandle> crowded;
        for (unsigned i = 0; i < 1025; ++i) {
            crowded.push_back(gfx::CreateMesh(input.vertices, input.indices));
        }
        EXPECT_FALSE(gfx::detail::AttachmentAlphaIdentityReady());
        EXPECT_GT(gfx::detail::InspectAttachmentAlphaTrace().identity_overflow, 0u);
        const auto before = gfx::detail::g.prog0_rgb_cost_selected;
        fixture.Frame(kClear, [&] { gfx::DrawMeshProg0(mesh, 0, 6, data.mesh, data.prog0, NearestBinding(texture), RgbCostState()); });
        EXPECT_EQ(gfx::detail::g.prog0_rgb_cost_selected, before);
        for (auto handle : crowded) {
            gfx::DestroyMesh(handle);
        }
        ExpectIdentityOnly();
        gfx::DestroyMesh(mesh);
        gfx::DestroyTexture(texture);
    }
    for (unsigned invalid = 0; invalid < 3; ++invalid) {
        AlphaTraceTestInput input(false, true, 1, invalid != 0);
        SetEnv("KZ_PROG0_RGB_COST_DIAGNOSTIC", invalid == 2 ? "invalid" : "2", 1);
        if (invalid == 1) {
            std::ofstream broken(input.path, std::ios::binary | std::ios::trunc);
            broken << "invalid";
        }
        GfxFixture fixture(kWidth, kHeight, 1, Prog0Options());
        const auto mesh = gfx::CreateMesh(input.vertices, kTriangle);
        const auto texture = gfx::CreateTexture({2, 2, gfx::TextureFormat::Rgba8, 1, true});
        ASSERT_TRUE(gfx::UpdateTexture(texture, 0, 0, 0, 2, 2, input.pixels.data()));
        auto data = SharedRgbBlackDraw(false);
        fixture.Frame(kClear, [&] { gfx::DrawMeshProg0(mesh, 0, 3, data.mesh, data.prog0, NearestBinding(texture), RgbCostState()); });
        EXPECT_EQ(gfx::detail::g.prog0_rgb_cost_selected[0], 0u);
        if (invalid) {
            EXPECT_FALSE(gfx::detail::AttachmentAlphaIdentityReady());
        }
        ExpectIdentityOnly();
        gfx::DestroyMesh(mesh);
        gfx::DestroyTexture(texture);
    }
}

namespace {
/**
 * Restores opt-in query, identity and measured-phase policies.
 */
struct GuardedSamplePolicies {
    EnvSnapshot samples = SaveEnv("KZ_PERF_GUARDED_ZERO_SAMPLES"); /**< Original precise probe request. */
    EnvSnapshot phases = SaveEnv("KZ_PERF_FRAME_PHASES");          /**< Original measured frame reporting. */
    EnvSnapshot rgb = SaveEnv("KZ_PROG0_RGB_COST_DIAGNOSTIC");     /**< Original RGB diagnostic policy. */
    EnvSnapshot shared = SaveEnv("KZ_PROG0_SHARED_RGB");           /**< Original smooth-input policy. */

    /**
     * Enables only the prerequisites shared by exact OFF and ON comparisons.
     */
    GuardedSamplePolicies() {
        SetEnv("KZ_PERF_FRAME_PHASES", "1", 1);
        SetEnv("KZ_PROG0_RGB_COST_DIAGNOSTIC", "0", 1);
        SetEnv("KZ_PROG0_SHARED_RGB", "1", 1);
    }

    /**
     * Restores all requests after the last renderer lifetime.
     */
    ~GuardedSamplePolicies() {
        RestoreEnv(samples);
        RestoreEnv(phases);
        RestoreEnv(rgb);
        RestoreEnv(shared);
    }
};

/**
 * Routes a production-valid zero material through its guarded family for count witnesses.
 */
struct GuardedSampleFamily {
    VkPipeline constant = gfx::detail::g.prog0_zero_source_alpha_pipeline; /**< Earlier unconditional zero priority. */
    VkPipeline near = gfx::detail::g.prog0_near_safe_fog_pipelines[2];     /**< Earlier near-safe additive priority. */

    /**
     * Keeps the actual guarded predicate and all its eligibility checks intact.
     */
    GuardedSampleFamily() {
        gfx::detail::g.prog0_zero_source_alpha_pipeline = VK_NULL_HANDLE;
        gfx::detail::g.prog0_near_safe_fog_pipelines[2] = VK_NULL_HANDLE;
    }

    /**
     * Restores owned handles before normal pipeline teardown.
     */
    ~GuardedSampleFamily() {
        gfx::detail::g.prog0_zero_source_alpha_pipeline = constant;
        gfx::detail::g.prog0_near_safe_fog_pipelines[2] = near;
    }
};
} // namespace

/**
 * Counts exact all-fast, all-fallback and half-covered predicate samples without image changes.
 */
TEST(GfxGpuMeshDc2, GuardedZeroSamplesCountActualArmsWithoutImageChanges) {
    SharedRgbPolicies     parent;
    GuardedSamplePolicies policies;
    for (unsigned shape = 0; shape < 3; ++shape) {
        std::vector<std::vector<uint8_t>> reference;
        for (bool enabled : {false, true}) {
            AlphaTraceTestInput input(false, true, shape == 2 ? 2048.f : shape == 1 ? 0.f
                                                                                    : 1.f,
                                      true, false, true, shape == 1 ? 1500.f : 0.f);
            SetEnv("KZ_PERF_GUARDED_ZERO_SAMPLES", enabled ? "1" : "0", 1);
            GfxFixture          fixture(kWidth, kHeight, 1, Prog0Options());
            GuardedSampleFamily family;
            if (enabled && !gfx::detail::g.guarded_samples_supported) {
                GTEST_SKIP() << "Precise occlusion unavailable; count witness not executed";
            }
            const auto mesh = gfx::CreateMesh(input.vertices, input.indices);
            const auto texture = gfx::CreateTexture({2, 2, gfx::TextureFormat::Rgba8, 1, true});
            ASSERT_TRUE(gfx::UpdateTexture(texture, 0, 0, 0, 2, 2, input.pixels.data()));
            auto draw = SharedRgbBlackDraw(false);
            draw.prog0.diffuse[3] = .005984187126159668f;
            const auto *uploaded = gfx::detail::LookupMesh(mesh);
            ASSERT_NE(uploaded, nullptr);
            ASSERT_TRUE(uploaded->bounded_uv_inputs);
            ASSERT_TRUE(gfx::detail::Prog0CanGuardZeroSourceAlpha(*uploaded, draw.mesh, draw.prog0));
            gfx::BeginRecording();
            gfx::Clear(true, kClear.data(), true, 0);
            gfx::ClearStencil(0);
            gfx::DrawMeshProg0(mesh, 0, static_cast<uint32_t>(input.indices.size()), draw.mesh, draw.prog0, NearestBinding(texture), RgbCostState());
            const auto list = gfx::EndRecording();
            gfx::BeginPresentationHistory(0);
            size_t image = 0;
            for (bool canonical : {true, false}) {
                const auto before = gfx::detail::g.prog0_guarded_zero_alpha_selected;
                ASSERT_TRUE(gfx::RenderList(*list, 1, {.canonical = canonical, .present = !canonical}));
                EXPECT_EQ(gfx::detail::g.prog0_guarded_zero_alpha_selected - before, 1u);
                ASSERT_TRUE(gfx::ReadbackFrame(fixture.pixels, fixture.width, fixture.height));
                if (!enabled) {
                    reference.push_back(fixture.pixels);
                } else {
                    EXPECT_EQ(fixture.pixels, reference[image]);
                }
                ++image;
            }
            gfx::EndPresentationHistory(0);
            for (unsigned i = 0; i < 5; ++i) {
                fixture.Frame(kClear, [] {});
            }
            const auto result = gfx::detail::InspectGuardedZeroSamples();
            EXPECT_EQ(result.enabled, enabled && result.supported);
            if (!enabled || !result.supported) {
                EXPECT_FALSE(result.owner);
                EXPECT_FALSE(result.pool);
            } else {
                EXPECT_TRUE(result.pool);
                EXPECT_EQ(result.errors, 0u);
                for (size_t index : {0u, 2u}) {
                    const auto &execution = result.executions[index];
                    ASSERT_TRUE(execution.attempted) << shape << ":" << index;
                    ASSERT_TRUE(execution.complete) << shape << ":" << index;
                    EXPECT_EQ(execution.pairs, 1u);
                    EXPECT_EQ(execution.sidecars, 1u);
                    EXPECT_EQ(execution.total, static_cast<uint64_t>(kWidth * kHeight));
                    EXPECT_EQ(execution.fast, shape == 0 ? execution.total : shape == 1 ? 0u
                                                                                        : execution.total / 2);
                }
                EXPECT_FALSE(result.executions[1].attempted);
                EXPECT_FALSE(result.executions[3].attempted);
            }
            ExpectIdentityOnly();
            gfx::DestroyMesh(mesh);
            gfx::DestroyTexture(texture);
        }
    }
}

/**
 * Preserves clipping, post-depth samples and immediate original pipeline and mask effects.
 */
TEST(GfxGpuMeshDc2, GuardedZeroSamplesPreserveStateCoverageAndRestoration) {
    SharedRgbPolicies                 parent;
    GuardedSamplePolicies             policies;
    std::vector<std::vector<uint8_t>> reference;
    std::vector<uint64_t>             coverage;
    for (bool enabled : {false, true}) {
        AlphaTraceTestInput input(false, true, 1, true, false, true);
        SetEnv("KZ_PERF_GUARDED_ZERO_SAMPLES", enabled ? "1" : "0", 1);
        GfxFixture          fixture(kWidth, kHeight, 1, Prog0Options());
        GuardedSampleFamily family;
        if (enabled && !gfx::detail::g.guarded_samples_supported) {
            GTEST_SKIP() << "Precise occlusion unavailable; no count claim";
        }
        const auto mesh = gfx::CreateMesh(input.vertices, input.indices);
        auto       fallback_vertices = input.vertices;
        for (auto &vertex : fallback_vertices) {
            vertex.uv[0] = 1500;
            vertex.uv[1] = 0;
        }
        const auto unmatched = gfx::CreateMesh(fallback_vertices, input.indices);
        const auto texture = gfx::CreateTexture({2, 2, gfx::TextureFormat::Rgba8, 1, true});
        ASSERT_TRUE(gfx::UpdateTexture(texture, 0, 0, 0, 2, 2, input.pixels.data()));
        auto draw = SharedRgbBlackDraw(false);
        draw.prog0.diffuse[3] = .005984187126159668f;
        size_t image = 0;
        for (unsigned scenario = 0; scenario < 13; ++scenario) {
            auto current = draw;
            auto state = RgbCostState();
            if (scenario >= 3 && scenario < 9) {
                const auto  plane = scenario - 3, axis = plane / 2;
                const float sign = (plane & 1) ? -1.f : 1.f;
                current.prog0.flags |= gfx::kProg0Clip | gfx::kProg0Scissor;
                std::fill_n(current.prog0.clip_or_view, 16, 0.f);
                current.prog0.clip_or_view[axis] = sign;
                current.prog0.clip_or_view[12 + axis] = 2 * sign;
                current.prog0.clip_or_view[15] = 2;
            }
            if (scenario == 9) {
                state.depth_write = true;
            }
            if (scenario == 10) {
                state.stencil_test = true;
            }
            const auto record = [&] {
                gfx::BeginRecording();
                gfx::Clear(true, kClear.data(), true, 0);
                gfx::ClearStencil(0);
                if (scenario == 1 || scenario == 2) {
                    gfx::DrawState cover;
                    cover.depth_write = true;
                    const auto quad = Quad(0, 0, gfx::LogicalWidth() / (scenario == 2 ? 2 : 1), gfx::LogicalHeight(), {71, 23, 41, 128}, 0, 0, 0, 0, 1);
                    gfx::Draw2D(gfx::Primitive::Quads, quad, {}, cover);
                }
                gfx::DrawMeshProg0(mesh, 0, 6, current.mesh, current.prog0, NearestBinding(texture), state);
                if (scenario >= 11) {
                    const std::array<uint8_t, 4> reset = {79, 83, 89, 128};
                    gfx::Clear(true, reset.data(), false, 0);
                    gfx::DrawMeshProg0(unmatched, 0, 6, draw.mesh, draw.prog0, NearestBinding(texture), RgbCostState());
                    if (scenario == 12) {
                        gfx::DrawState consumer;
                        consumer.depth_test = gfx::DepthTest::GEqual;
                        consumer.stencil_test = true;
                        consumer.stencil_front.compare = consumer.stencil_back.compare = gfx::CompareOp::Equal;
                        const auto quad = Quad(0, 0, gfx::LogicalWidth(), gfx::LogicalHeight(), {89, 127, 59, 128}, 0, 0, 0, 0, .25f);
                        gfx::Draw2D(gfx::Primitive::Quads, quad, {}, consumer);
                        consumer.stencil_front.compare = consumer.stencil_back.compare = gfx::CompareOp::NotEqual;
                        const auto reject = Quad(0, 0, gfx::LogicalWidth(), gfx::LogicalHeight(), {255, 0, 0, 128}, 0, 0, 0, 0, .25f);
                        gfx::Draw2D(gfx::Primitive::Quads, reject, {}, consumer);
                    }
                }
                return gfx::EndRecording();
            };
            const auto list = record();
            gfx::BeginPresentationHistory(0);
            for (bool canonical : {true, false}) {
                ASSERT_TRUE(gfx::RenderList(*list, 1, {.canonical = canonical, .present = !canonical}));
                ASSERT_TRUE(gfx::ReadbackFrame(fixture.pixels, fixture.width, fixture.height));
                if (!enabled) {
                    reference.push_back(fixture.pixels);
                    uint64_t samples = 0;
                    for (size_t i = 3; i < fixture.pixels.size(); i += 4) {
                        samples += fixture.pixels[i] == 0;
                    }
                    if (scenario < 9) {
                        EXPECT_EQ(samples, scenario == 0 ? static_cast<uint64_t>(kWidth * kHeight) : scenario == 1 ? 0u
                                                                                                                   : static_cast<uint64_t>(kWidth * kHeight / 2));
                    }
                    coverage.push_back(samples);
                } else {
                    EXPECT_EQ(fixture.pixels, reference[image]);
                }
                if (scenario == 11) {
                    const auto pixel = fixture.Pixel(kWidth / 2, kHeight / 2);
                    EXPECT_EQ(pixel[0], 79u);
                    EXPECT_EQ(pixel[3], 0u);
                }
                if (scenario == 12) {
                    const auto pixel = fixture.Pixel(kWidth / 2, kHeight / 2);
                    EXPECT_EQ(pixel[0], 89u);
                    EXPECT_EQ(pixel[1], 127u);
                    EXPECT_EQ(pixel[2], 59u);
                }
                ++image;
            }
            gfx::EndPresentationHistory(0);
            for (unsigned i = 0; i < 5; ++i) {
                fixture.Frame(kClear, [] {});
            }
            const auto result = gfx::detail::InspectGuardedZeroSamples();
            if (enabled) {
                for (size_t role : {0u, 1u}) {
                    const auto &execution = result.executions[role * 2];
                    if (scenario == 9 || scenario == 10) {
                        EXPECT_FALSE(execution.attempted);
                        continue;
                    }
                    ASSERT_TRUE(execution.complete) << scenario << ":" << role;
                    const uint64_t expected = scenario >= 11 ? kWidth * kHeight : coverage[image - 2 + role];
                    if (scenario < 9) {
                        EXPECT_EQ(expected, scenario == 0 ? static_cast<uint64_t>(kWidth * kHeight) : scenario == 1 ? 0u
                                                                                                                    : static_cast<uint64_t>(kWidth * kHeight / 2));
                    }
                    EXPECT_EQ(execution.total, expected) << scenario;
                    EXPECT_EQ(execution.fast, expected) << scenario;
                    EXPECT_EQ(execution.pairs, 1u);
                    EXPECT_EQ(execution.sidecars, 1u);
                }
            }
        }
        ExpectIdentityOnly();
        gfx::DestroyMesh(unmatched);
        gfx::DestroyMesh(mesh);
        gfx::DestroyTexture(texture);
    }
}

/**
 * Bounds sampled executions, preserves pending results and isolates measurement generations.
 */
TEST(GfxGpuMeshDc2, GuardedZeroSamplesBoundEpochsFencesAndLifetimes) {
    SharedRgbPolicies     parent;
    GuardedSamplePolicies policies;
    for (bool eager : {false, true}) {
        AlphaTraceTestInput input(false, true, 1, true, false, true);
        SetEnv("KZ_PERF_GUARDED_ZERO_SAMPLES", "1", 1);
        SetEnv("KZ_PIPELINE_PREWARM", eager ? "1" : "0", 1);
        GfxFixture          fixture(kWidth, kHeight, 1, Prog0Options());
        GuardedSampleFamily family;
        if (!gfx::detail::g.guarded_samples_supported) {
            GTEST_SKIP() << "Precise occlusion unavailable; bounded count witness not executed";
        }
        ASSERT_NE(gfx::detail::g.guarded_classifier_pipeline, VK_NULL_HANDLE);
        EXPECT_EQ(gfx::detail::DescribePipeline(gfx::detail::kProg0GuardClassifierScope), "prog0-guarded-zero-sample-classifier");
        const auto mesh = gfx::CreateMesh(input.vertices, input.indices);
        const auto texture = gfx::CreateTexture({2, 2, gfx::TextureFormat::Rgba8, 1, true});
        ASSERT_TRUE(gfx::UpdateTexture(texture, 0, 0, 0, 2, 2, input.pixels.data()));
        const auto record = [&](float x, unsigned draws, bool excluded, gfx::MeshHandle selected = gfx::kNullMesh, uint32_t count = 6) {
            auto data = SharedRgbBlackDraw(false);
            data.prog0.diffuse[3] = .005984187126159668f;
            auto transform = gfx::IdentityMeshTransform();
            transform.model[12] = x;
            transform.projection[0] = transform.projection[5] = transform.projection[15] = 4;
            transform.projection[14] = 2;
            gfx::detail::Mat4 projection, model;
            std::copy_n(transform.projection, 16, projection.begin());
            std::copy_n(transform.model, 16, model.begin());
            const auto mvp = gfx::detail::Multiply(projection, model);
            std::copy(mvp.begin(), mvp.end(), data.mesh.mvp);
            auto state = RgbCostState();
            state.depth_write = excluded;
            gfx::BeginRecording();
            gfx::Clear(true, kClear.data(), true, 0);
            for (unsigned i = 0; i < draws; ++i) {
                gfx::SetInterpKey(9273 + i);
                gfx::DrawMeshProg0(selected == gfx::kNullMesh ? mesh : selected, 0, count, data.mesh, data.prog0, NearestBinding(texture), state, &transform);
            }
            return gfx::EndRecording();
        };
        const auto previous = record(-.2f, 1, false), current = record(.2f, 1, false), excluded = record(.2f, 1, true), overflow = record(.2f, 65, false);
        const auto render = [&](const gfx::DisplayList &list, bool canonical) { return gfx::RenderList(list, canonical ? 1.f : .5f, {.canonical = canonical, .previous = canonical ? nullptr : previous.get(), .present = !canonical}); };
        const auto drain = [&] {for(unsigned i=0;i<5;++i){fixture.Frame(kClear,[]{});
} };
        gfx::BeginPresentationHistory(0);
        ASSERT_TRUE(gfx::UpdateTexture(texture, 0, 0, 0, 1, 1, input.pixels.data()));
        ASSERT_TRUE(render(*current, true));
        ASSERT_TRUE(render(*current, false));
        auto state = gfx::detail::InspectGuardedZeroSamples();
        EXPECT_FALSE(state.executions[0].attempted);
        EXPECT_FALSE(state.executions[2].attempted);
        ASSERT_TRUE(gfx::UpdateTexture(texture, 0, 0, 0, 2, 2, input.pixels.data()));
        const auto partial = record(.2f, 1, false, gfx::kNullMesh, 3);
        ASSERT_TRUE(render(*partial, true));
        ASSERT_TRUE(render(*partial, false));
        state = gfx::detail::InspectGuardedZeroSamples();
        EXPECT_FALSE(state.executions[0].attempted);
        EXPECT_FALSE(state.executions[2].attempted);
        ASSERT_TRUE(render(*excluded, true));
        ASSERT_TRUE(render(*excluded, false));
        state = gfx::detail::InspectGuardedZeroSamples();
        EXPECT_FALSE(state.executions[0].attempted);
        EXPECT_FALSE(state.executions[2].attempted);
        std::array<uint64_t, 2> visible{};
        for (bool canonical : {true, false}) {
            ASSERT_TRUE(render(*current, canonical));
            ASSERT_TRUE(gfx::ReadbackFrame(fixture.pixels, fixture.width, fixture.height));
            for (size_t a = 3; a < fixture.pixels.size(); a += 4) {
                visible[canonical ? 0 : 1] += fixture.pixels[a] == 0;
            }
        }
        EXPECT_EQ(visible[0], 2784u);
        EXPECT_EQ(visible[1], 3072u);
        ASSERT_TRUE(render(*current, true));
        ASSERT_TRUE(render(*current, false));
        std::this_thread::sleep_for(std::chrono::milliseconds(510));
        ASSERT_TRUE(render(*current, true));
        ASSERT_TRUE(render(*current, false));
        ASSERT_TRUE(render(*current, true));
        ASSERT_TRUE(render(*current, false));
        gfx::EndPresentationHistory(0);
        drain();
        state = gfx::detail::InspectGuardedZeroSamples();
        for (size_t role = 0; role < 2; ++role) {
            for (size_t attempt = 0; attempt < 2; ++attempt) {
                const auto &execution = state.executions[role * 2 + attempt];
                ASSERT_TRUE(execution.complete);
                EXPECT_EQ(execution.pairs, 1u);
                EXPECT_EQ(execution.sidecars, 1u);
                EXPECT_EQ(execution.total, visible[role]);
                EXPECT_EQ(execution.fast, execution.total);
                if (attempt) {
                    EXPECT_GE(execution.begin - state.executions[role * 2].begin, 500'000'000u);
                }
            }
        }
        gfx::LogPresentationHistory();
        if (eager) {
            gfx::BeginPresentationHistory(0);
            ASSERT_TRUE(render(*current, true));
            ExpectIdentityOnly();
            gfx::DestroyMesh(mesh);
            gfx::DestroyTexture(texture);
            continue;
        }
        auto temporary = gfx::CreateMesh(input.vertices, input.indices);
        gfx::BeginRecording();
        ASSERT_TRUE(gfx::UpdateMeshVertices(temporary, 0, input.vertices));
        const auto update = gfx::EndRecording();
        EXPECT_NE(gfx::detail::LookupMesh(temporary)->alpha_trace_aliases, 0u);
        gfx::BeginPresentationHistory(0);
        ASSERT_TRUE(render(*update, true));
        EXPECT_EQ(gfx::detail::LookupMesh(temporary)->alpha_trace_aliases, 0u);
        const auto invalidated = record(.2f, 1, false, temporary);
        ASSERT_TRUE(render(*invalidated, true));
        EXPECT_FALSE(gfx::detail::InspectGuardedZeroSamples().executions[0].attempted);
        gfx::EndPresentationHistory(0);
        drain();
        gfx::LogPresentationHistory();
        const auto old_handle = temporary;
        gfx::DestroyMesh(temporary);
        temporary = gfx::CreateMesh(input.vertices, input.indices);
        EXPECT_NE(temporary, old_handle);
        const auto reused = record(.2f, 1, false, temporary);
        gfx::BeginPresentationHistory(0);
        ASSERT_TRUE(render(*reused, true));
        gfx::EndPresentationHistory(0);
        gfx::DestroyMesh(temporary);
        drain();
        ASSERT_TRUE(gfx::detail::InspectGuardedZeroSamples().executions[0].complete);
        gfx::LogPresentationHistory();
        gfx::BeginPresentationHistory(0);
        gfx::EndPresentationHistory(0);
        state = gfx::detail::InspectGuardedZeroSamples();
        for (const auto &execution : state.executions) {
            EXPECT_FALSE(execution.attempted);
        }
        gfx::LogPresentationHistory();
        gfx::BeginPresentationHistory(0);
        ASSERT_TRUE(render(*overflow, true));
        ASSERT_TRUE(render(*overflow, false));
        gfx::EndPresentationHistory(0);
        drain();
        state = gfx::detail::InspectGuardedZeroSamples();
        for (size_t index : {0u, 2u}) {
            EXPECT_TRUE(state.executions[index].overflow);
            EXPECT_FALSE(state.executions[index].complete);
            EXPECT_EQ(state.executions[index].pairs, 64u);
            EXPECT_EQ(state.executions[index].sidecars, 64u);
        }
        gfx::LogPresentationHistory();
        const auto boundary = AlphaTestNow();
        gfx::BeginPresentationHistory(boundary);
        ASSERT_TRUE(render(*current, true));
        gfx::EndPresentationHistory(boundary);
        drain();
        state = gfx::detail::InspectGuardedZeroSamples();
        EXPECT_TRUE(state.executions[0].cancelled);
        EXPECT_FALSE(state.executions[0].complete);
        gfx::LogPresentationHistory();
        gfx::BeginPresentationHistory(0);
        ASSERT_TRUE(render(*current, true));
        const auto old_epoch = gfx::detail::InspectGuardedZeroSamples().epoch;
        gfx::BeginPresentationHistory(0);
        ASSERT_TRUE(render(*current, true));
        gfx::EndPresentationHistory(0);
        drain();
        state = gfx::detail::InspectGuardedZeroSamples();
        EXPECT_GT(state.epoch, old_epoch);
        ASSERT_TRUE(state.executions[0].complete);
        EXPECT_EQ(state.executions[0].pairs, 1u);
        EXPECT_FALSE(state.executions[2].attempted);
        gfx::LogPresentationHistory();
        gfx::BeginPresentationHistory(0);
        ASSERT_TRUE(render(*current, true));
        gfx::EndPresentationHistory(0);
        gfx::detail::RunOnRenderThread([&] {
            auto found = std::find_if(gfx::detail::g.frames.begin(), gfx::detail::g.frames.end(), [](const auto &frame) { return !frame.guarded_samples.empty() && !frame.guarded_merged; });
            ASSERT_NE(found, gfx::detail::g.frames.end());
            VkFenceCreateInfo info{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
            VkFence           fake = VK_NULL_HANDLE;
            ASSERT_EQ(vkCreateFence(gfx::detail::g.device, &info, nullptr, &fake), VK_SUCCESS);
            const auto real = found->fence;
            const auto pending = found->pending;
            found->fence = fake;
            found->pending = true;
            const auto pending_state = gfx::detail::InspectGuardedZeroSamples();
            EXPECT_FALSE(pending_state.executions[0].complete);
            EXPECT_FALSE(found->guarded_merged);
            found->fence = real;
            found->pending = pending;
            vkDestroyFence(gfx::detail::g.device, fake, nullptr);
        });
        drain();
        state = gfx::detail::InspectGuardedZeroSamples();
        ASSERT_TRUE(state.executions[0].complete);
        EXPECT_EQ(state.errors, 0u);
        gfx::LogPresentationHistory();
        std::vector<gfx::TextureHandle> identities;
        for (size_t i = 0; i < 1024; ++i) {
            identities.push_back(gfx::CreateTexture({2, 2, gfx::TextureFormat::Rgba8, 1, true}));
        }
        auto overflow_draw = SharedRgbBlackDraw(false);
        overflow_draw.prog0.diffuse[3] = .005984187126159668f;
        gfx::BeginRecording();
        gfx::Clear(true, kClear.data(), true, 0);
        gfx::DrawMeshProg0(mesh, 0, 6, overflow_draw.mesh, overflow_draw.prog0, NearestBinding(texture), RgbCostState());
        for (const auto identity : identities) {
            ASSERT_TRUE(gfx::UpdateTexture(identity, 0, 0, 0, 2, 2, input.pixels.data()));
        }
        const auto lost_identity = gfx::EndRecording();
        gfx::BeginPresentationHistory(0);
        ASSERT_TRUE(render(*lost_identity, true));
        gfx::EndPresentationHistory(0);
        drain();
        state = gfx::detail::InspectGuardedZeroSamples();
        ASSERT_TRUE(state.executions[0].attempted);
        EXPECT_TRUE(state.executions[0].cancelled);
        EXPECT_FALSE(state.executions[0].complete);
        EXPECT_FALSE(gfx::detail::AttachmentAlphaIdentityReady());
        EXPECT_GT(gfx::detail::InspectAttachmentAlphaTrace().identity_overflow, 0u);
        gfx::LogPresentationHistory();
        for (const auto identity : identities) {
            gfx::DestroyTexture(identity);
        }
        ExpectIdentityOnly();
        gfx::DestroyMesh(mesh);
        gfx::DestroyTexture(texture);
    }
}
