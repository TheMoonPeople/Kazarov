#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <format>
#include <limits>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "gfx/context.hpp"
#include "gfx/displaylist.hpp"
#include "platform/overlay.hpp"
#include "tests/gfx_fixture.hpp"

using namespace dc::test;

namespace {

/**
 * Evaluates matching with a direct ordered scan independent of candidate buckets.
 */
std::vector<int32_t> ReferenceMeshMatches(const gfx::DisplayList &list, const gfx::DisplayList &previous,
                                          std::unordered_set<gfx::InterpKey> &instanced) {
    using Records = std::unordered_map<gfx::InterpKey, std::vector<int32_t>>;
    const auto groups = [&](const gfx::DisplayList &source) {
        Records result;
        for (size_t index = 0; index < source.records.size(); ++index) {
            const auto &record = source.records[index];
            if (!record.key || !record.has_transform) {
                continue;
            }
            auto &family = result[record.key];
            if (!family.empty() && source.records[family.front()].model != record.model) {
                instanced.insert(record.key);
            }
            family.push_back(static_cast<int32_t>(index));
        }
        return result;
    };
    const auto before = groups(previous);
    const auto now = groups(list);
    const auto mirrored = [](const gfx::detail::Mat4 &m) {
        const float cross_x = m[5] * m[10] - m[6] * m[9];
        const float cross_y = m[6] * m[8] - m[4] * m[10];
        const float cross_z = m[4] * m[9] - m[5] * m[8];
        return m[0] * cross_x + m[1] * cross_y + m[2] * cross_z < 0;
    };
    const auto distance = [](const auto &a, const auto &b) {
        const float x = b.model[12] - a.model[12];
        const float y = b.model[13] - a.model[13];
        const float z = b.model[14] - a.model[14];
        return std::sqrt(x * x + y * y + z * z);
    };
    const auto compatible = [&](int32_t index, int32_t candidate) {
        const auto &record = list.records[index];
        const auto &earlier = previous.records[candidate];
        const auto &a = std::get<gfx::detail::MeshEntry>(list.entries[record.entry]);
        const auto &b = std::get<gfx::detail::MeshEntry>(previous.entries[earlier.entry]);
        if (mirrored(record.model) != mirrored(earlier.model) || a.mesh != b.mesh ||
            a.binding != b.binding || a.first_index != b.first_index || a.index_count != b.index_count ||
            (a.prog0 >= 0) != (b.prog0 >= 0)) {
            return false;
        }
        if (a.mesh != gfx::kNullMesh) {
            return true;
        }
        if (a.indices != b.indices || a.vertices.size() != b.vertices.size()) {
            return false;
        }
        bool identical = true;
        for (size_t vertex = 0; vertex < a.vertices.size(); ++vertex) {
            identical &= std::memcmp(&a.vertices[vertex], &b.vertices[vertex], sizeof(gfx::Vertex3D)) == 0;
        }
        if (identical) {
            return true;
        }
        if (!record.blend_vertices || !earlier.blend_vertices || record.occurrence != earlier.occurrence) {
            return false;
        }
        for (size_t vertex = 0; vertex < a.vertices.size(); ++vertex) {
            if (std::memcmp(a.vertices[vertex].uv, b.vertices[vertex].uv, sizeof(a.vertices[vertex].uv)) ||
                std::memcmp(a.vertices[vertex].color, b.vertices[vertex].color, sizeof(a.vertices[vertex].color))) {
                return false;
            }
        }
        return true;
    };
    std::vector<int32_t> result(list.records.size(), -1);
    for (const auto &[key, records] : now) {
        const auto found = before.find(key);
        if (found == before.end()) {
            continue;
        }
        const auto       &candidates = found->second;
        std::vector<bool> claimed(candidates.size(), false);
        if (instanced.contains(key)) {
            for (int32_t index : records) {
                if (list.records[index].no_interpolation) {
                    continue;
                }
                for (size_t position = 0; position < candidates.size(); ++position) {
                    const int32_t candidate = candidates[position];
                    if (!claimed[position] && previous.records[candidate].model == list.records[index].model && compatible(index, candidate)) {
                        claimed[position] = true;
                        result[index] = candidate;
                        break;
                    }
                }
            }
            for (int32_t index : records) {
                if (list.records[index].no_interpolation || result[index] >= 0) {
                    continue;
                }
                float  nearest = std::min(25.0f, list.records[index].teleport_distance);
                size_t chosen = 0;
                for (size_t position = 0; position < candidates.size(); ++position) {
                    const float moved = distance(previous.records[candidates[position]], list.records[index]);
                    if (!claimed[position] && moved <= nearest && compatible(index, candidates[position])) {
                        nearest = moved;
                        chosen = position;
                        result[index] = candidates[position];
                    }
                }
                if (result[index] >= 0) {
                    claimed[chosen] = true;
                }
            }
        } else {
            for (int32_t index : records) {
                for (size_t position = 0; position < candidates.size(); ++position) {
                    const int32_t candidate = candidates[position];
                    if (claimed[position] || !compatible(index, candidate)) {
                        continue;
                    }
                    claimed[position] = true;
                    if (!list.records[index].no_interpolation &&
                        !(distance(previous.records[candidate], list.records[index]) > list.records[index].teleport_distance)) {
                        result[index] = candidate;
                    }
                    break;
                }
            }
        }
    }
    return result;
}

} // namespace

TEST(GfxGpuDisplayList, CandidateBucketsPreserveOrderedReferenceMatches) {
    GfxOptions options;
    options.prog0_meshes = true;
    options.pipeline_cache = std::filesystem::temp_directory_path() / "dc_gfx_test" / "pipeline_cache_match_reference.bin";
    GfxFixture                              fixture(16, 16, 1.0f, options);
    const std::vector<gfx::Vertex3D>        vertices = {{{-.2f, -.2f, .5f}}, {{.2f, -.2f, .5f}}, {{0, .2f, .5f}}};
    const std::vector<uint32_t>             indices = {0, 1, 2, 0, 2, 1};
    const std::array<gfx::MeshHandle, 2>    meshes{gfx::CreateMesh(vertices, indices), gfx::CreateMesh(vertices, indices)};
    const std::array<gfx::TextureHandle, 3> textures{gfx::kNullTexture, gfx::CreateTexture({1, 1}), gfx::CreateTexture({1, 1})};
    const uint8_t                           white[4] = {128, 128, 128, 128};
    ASSERT_NE(meshes[0], gfx::kNullMesh);
    ASSERT_NE(meshes[1], gfx::kNullMesh);
    ASSERT_TRUE(gfx::UpdateTexture(textures[1], 0, 0, 0, 1, 1, white));
    ASSERT_TRUE(gfx::UpdateTexture(textures[2], 0, 0, 0, 1, 1, white));
    const auto identity = [] {
        gfx::detail::Mat4 matrix{};
        matrix[0] = matrix[5] = matrix[10] = matrix[15] = 1;
        return matrix;
    }();
    uint64_t   serial = 100000;
    const auto make = [&](int scenario, int generation) {
        auto list = std::make_shared<gfx::DisplayList>();
        list->instance = gfx::detail::g.renderer_instance;
        list->serial = ++serial;
        list->cut_generation = 100000 + scenario;
        list->cameras.push_back(identity);
        list->prog0_blocks.emplace_back().flags = gfx::kProg0Unlit;
        gfx::detail::ClearEntry clear;
        clear.color = clear.depth = true;
        list->entries.emplace_back(clear);
        list->main_cleared = true;
        const uint32_t record_count = scenario < 12 ? 96 : 1024;
        for (uint32_t index = 0; index < record_count; ++index) {
            if (generation && (index + static_cast<uint32_t>(scenario)) % 17 == 0) {
                continue;
            }
            const uint32_t         shape = (index * 13 + static_cast<uint32_t>(scenario)) % 19;
            gfx::detail::MeshEntry mesh;
            mesh.mesh = shape % 3 ? meshes[shape % 2] : gfx::kNullMesh;
            mesh.first_index = shape % 2;
            mesh.index_count = shape % 4;
            mesh.binding.texture = textures[shape % 3];
            mesh.binding.palette = textures[shape % 2];
            mesh.binding.filter = shape % 2 ? gfx::Filter::Nearest : gfx::Filter::Linear;
            mesh.binding.wrap_u = shape % 3 ? gfx::Wrap::Clamp : gfx::Wrap::Repeat;
            mesh.binding.wrap_v = shape % 5 ? gfx::Wrap::Repeat : gfx::Wrap::Clamp;
            mesh.binding.ignore_alpha = shape % 7 == 0;
            mesh.prog0 = shape % 2 ? 0 : -1;
            mesh.vertices.resize(2);
            mesh.vertices[0].position[0] = static_cast<float>((index % 3) + (generation && scenario % 3 == 0));
            mesh.vertices[0].uv[0] = static_cast<float>(shape % 2);
            mesh.vertices[1] = mesh.vertices[0];
            std::memcpy(mesh.constants.mvp, identity.data(), sizeof mesh.constants.mvp);
            gfx::detail::MeshRecord record;
            record.key = index % 11 ? 1 + index % 3 : 0;
            if (scenario >= 12 && record.key != 0) {
                record.key = index % 4 ? 65536 + index : 4096 + index % 7;
            }
            record.has_transform = index % 23 != 0;
            record.entry = static_cast<uint32_t>(list->entries.size());
            record.occurrence = index % 5;
            record.no_interpolation = generation && index % 13 == 0;
            record.blend_vertices = index % 3 == 0;
            record.teleport_distance = index % 7 == 0 ? -1.0f : index % 5 == 0 ? 0.0f
                                                                               : 25.0f;
            record.projection = record.middle = record.local = identity;
            record.model = identity;
            record.model[12] = scenario % 2 ? static_cast<float>(static_cast<int>(index % 5) - 2 + generation) : static_cast<float>(generation);
            if (scenario % 4 == 1) {
                record.model[0] = index % 4 == 0 ? -1.0f : 1.0f;
            }
            if (scenario % 4 == 2) {
                record.model[0] = index % 4 == 0 ? 0.0f : index % 7 == 0 ? std::numeric_limits<float>::quiet_NaN()
                                                                         : 1.0f;
            }
            if (scenario % 4 == 3 && index % 7 == 0) {
                record.teleport_distance = std::numeric_limits<float>::quiet_NaN();
            }
            if (generation && scenario % 3 == 1 && index % 7 == 0) {
                mesh.binding.texture = textures[(shape % 3 + 1) % textures.size()];
            }
            mesh.record = static_cast<uint32_t>(list->records.size());
            list->records.push_back(record);
            list->entries.emplace_back(std::move(mesh));
        }
        return list;
    };

    /**
     * Retains independent cache values after temporary matching arenas have been released.
     */
    struct RetainedMatches {
        gfx::DisplayListRef  before; /**< Earlier owned list used by the cache lookup. */
        gfx::DisplayListRef  after;  /**< Later owned list containing persistent match indices. */
        std::vector<int32_t> match;  /**< Independent copy of the ordered reference result. */
        std::vector<bool>    blend;  /**< Independent copy of the cached vertex-blending decisions. */
    };

    std::vector<RetainedMatches> retained;
    for (int scenario = 0; scenario < 16; ++scenario) {
        SCOPED_TRACE(scenario);
        auto before = make(scenario, 0);
        auto after = make(scenario, 1);
        ASSERT_TRUE(gfx::RenderList(*before, 1, {.canonical = true}));
        std::unordered_set<gfx::InterpKey> instanced;
        for (int generation = 1; generation < 3; ++generation) {
            const auto expected = ReferenceMeshMatches(*after, *before, instanced);
            ASSERT_TRUE(gfx::RenderList(*after, .5f, {.previous = before.get()}));
            EXPECT_EQ(after->cache.match, expected);
            retained.push_back({before, after, expected, after->cache.blend});
            ASSERT_TRUE(gfx::RenderList(*after, .75f, {.previous = before.get()}));
            EXPECT_EQ(after->cache.match, expected);
            EXPECT_EQ(after->cache.blend, retained.back().blend);
            before = std::move(after);
            after = make(scenario, generation + 1);
        }
    }
    // Later generations and cuts have reused temporary allocation storage; cached indices
    // still belong to their lists and must remain valid without another matching pass.
    for (const auto &pair : retained) {
        EXPECT_EQ(pair.after->cache.previous_serial, pair.before->serial);
        EXPECT_EQ(pair.after->cache.match, pair.match);
        EXPECT_EQ(pair.after->cache.blend, pair.blend);
        ASSERT_TRUE(gfx::RenderList(*pair.after, .25f, {.previous = pair.before.get()}));
        EXPECT_EQ(pair.after->cache.match, pair.match);
        EXPECT_EQ(pair.after->cache.blend, pair.blend);
    }
    for (auto mesh : meshes) {
        gfx::DestroyMesh(mesh);
    }
    for (auto texture : textures) {
        if (texture != gfx::kNullTexture) {
            gfx::DestroyTexture(texture);
        }
    }
}

TEST(GfxGpuMeshDc2, InterpolatedOutlineEscapesNewestPartCrop) {
    GfxOptions options;
    options.offscreen = true;
    options.prog0_meshes = true;
    options.logical_width = 64;
    options.logical_height = 48;
    GfxFixture fixture(64, 48, 1.0f, options);
    const auto target = gfx::CreateRenderTarget(64, 48, true, true);
    ASSERT_NE(target, gfx::kNullTexture);
    const gfx::LogicalRect           full{0, 0, 64, 48};
    const uint8_t                    clear[4] = {0, 0, 0, 0};
    const std::vector<gfx::Vertex3D> triangle = {
        {{-0.2f, -0.2f, 0.5f}}, {{0.2f, -0.2f, 0.5f}}, {{0, 0.2f, 0.5f}}};
    const std::vector<uint32_t> indices = {0, 1, 2};
    auto                        record = [&](float camera, bool tagged) {
        auto transform = gfx::IdentityMeshTransform();
        transform.view[12] = camera;
        gfx::MeshConstants constants{};
        std::memcpy(constants.mvp, transform.view, sizeof constants.mvp);
        gfx::Prog0Constants block{};
        block.flags = gfx::kProg0Unlit;
        for (int i = 0; i < 4; ++i) {
            block.object_color[i] = i == 3 ? 128 : 200;
            block.diffuse[i] = 1;
        }
        const float            x = 32 + camera * 32;
        const gfx::LogicalRect crop{x - 8, 18, 16, 12};
        gfx::BeginRecording();
        gfx::Clear(true, clear, true, 0);
        gfx::SetRenderTarget(target);
        gfx::Clear(true, clear, false, 0, &crop, tagged ? &full : nullptr);
        gfx::SetInterpKey(17);
        gfx::DrawMeshImmediateProg0(triangle, indices, constants, block, {}, {}, &transform);
        gfx::SetRenderTarget(gfx::kMainTarget);
        std::vector<gfx::Vertex2D> quad(4);
        for (int i = 0; i < 4; ++i) {
            auto &v = quad[i];
            v.x = v.u = crop.x + ((i == 1 || i == 2) ? crop.w : 0);
            v.y = v.v = crop.y + (i >= 2 ? crop.h : 0);
            std::memset(v.color, 128, 4);
        }
        gfx::TextureBinding binding;
        binding.texture = target;
        gfx::DrawState state;
        auto           display_quad = quad;
        for (int i = 0; i < 4; ++i) {
            auto &v = display_quad[i];
            v.x = v.u = (i == 1 || i == 2) ? full.w : 0;
            v.y = v.v = i >= 2 ? full.h : 0;
        }
        const gfx::Presentation2D presentation{display_quad, state};
        gfx::Draw2D(gfx::Primitive::Quads, quad, binding, state,
                    tagged ? &presentation : nullptr);
        return gfx::EndRecording();
    };
    auto render = [&](const gfx::DisplayList &list, float alpha, gfx::RenderOptions options) {
        options.present = !options.canonical;
        EXPECT_TRUE(gfx::RenderList(list, alpha, options));
        EXPECT_TRUE(gfx::ReadbackFrame(fixture.pixels, fixture.width, fixture.height));
        return fixture.pixels;
    };
    auto       before = record(-0.5f, true);
    auto       after = record(0.5f, true);
    auto       untagged = record(0.5f, false);
    const auto newest = render(*untagged, 1, {.canonical = true});
    EXPECT_EQ(render(*after, 1, {.canonical = true}), newest);
    // Compare every display pixel against an independently recorded camera position.
    // This exercises the Prog0 path used by outlined character parts.
    for (int phase = 0; phase < 8; ++phase) {
        const float alpha = phase / 8.0f;
        auto        expected = record(-0.5f + alpha, false);
        auto        reference = render(*expected, 1, {.canonical = true});
        render(*after, 1, {.canonical = true});
        EXPECT_EQ(render(*after, alpha, {.previous = before.get()}), reference) << alpha;
        EXPECT_EQ(render(*after, alpha, {.canonical = true}), newest);
    }
    gfx::DestroyTexture(target);
}

namespace {

constexpr int                kWidth = 64;
constexpr int                kHeight = 48;
const std::array<uint8_t, 4> kClear = {0, 0, 0, 0x80};
const std::vector<uint32_t>  kTriangle = {0, 1, 2};

using Rows = std::array<float, 12>;
constexpr Rows kIdentityRows = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};

GfxOptions Prog0Options() {
    GfxOptions options;
    options.prog0_meshes = true;
    options.pipeline_cache = std::filesystem::temp_directory_path() / "dc_gfx_test" / "pipeline_cache_prog0.bin";
    return options;
}

void Identity(float (&m)[16]) {
    std::memset(m, 0, sizeof m);
    m[0] = m[5] = m[10] = m[15] = 1.0f;
}

void SetBone(gfx::Prog0Palette &p, uint32_t slot, const Rows &rows) {
    for (uint32_t r = 0; r < 3; r++) {
        for (uint32_t c = 0; c < 4; c++) {
            p.bones[slot][r][c] = rows[r * 4 + c];
        }
    }
}

gfx::Vertex3D Vert(std::array<float, 3> position, std::array<float, 3> normal, std::array<uint8_t, 4> color = {}) {
    return {
        {position[0], position[1], position[2]},
        {normal[0], normal[1], normal[2]},
        {0.0f, 0.0f},
        {color[0], color[1], color[2], color[3]}
    };
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

struct Coverage {
    double x = 0.0;
    int    count = 0;
};

Coverage ReadCoverage(GfxFixture &fixture) {
    EXPECT_TRUE(gfx::ReadbackFrame(fixture.pixels, fixture.width, fixture.height));
    Coverage out;
    double   sum = 0.0;
    for (uint32_t y = 0; y < fixture.height; y++) {
        for (uint32_t x = 0; x < fixture.width; x++) {
            if (fixture.Pixel(x, y)[0] > 100) {
                sum += x;
                out.count++;
            }
        }
    }
    if (out.count > 0) {
        out.x = sum / out.count;
    }
    return out;
}

} // namespace

TEST(GfxGpuDisplayList, RecordingReservesPriorSizesWithoutRetainingCommands) {
    GfxFixture          fixture(kWidth, kHeight);
    gfx::Prog0Constants constants = {};
    constants.flags = gfx::kProg0Unlit;
    gfx::BeginRecording();
    gfx::SetInterpKey(17);
    for (int i = 0; i < 129; ++i) {
        constants.diffuse[0] = static_cast<float>(i);
        gfx::detail::RecordMesh({}, nullptr, &constants, nullptr);
    }
    gfx::CutInterpolation();
    auto first = gfx::EndRecording();
    ASSERT_EQ(first->prog0_blocks.size(), 129u);

    gfx::BeginRecording();
    auto empty = gfx::EndRecording();
    EXPECT_GE(empty->entries.capacity(), first->entries.size());
    EXPECT_GE(empty->records.capacity(), first->records.size());
    EXPECT_GE(empty->prog0_blocks.capacity(), first->prog0_blocks.size());
    EXPECT_TRUE(empty->entries.empty());
    EXPECT_TRUE(empty->records.empty());
    EXPECT_TRUE(empty->prog0_blocks.empty());
    EXPECT_TRUE(empty->occurrences.empty());
    EXPECT_FALSE(empty->cut);
    EXPECT_FALSE(empty->camera_cut);
    EXPECT_EQ(gfx::CurrentInterpKey(), 0u);
    // Reservation must leave a still-live previous list and its owned constants intact.
    for (int i = 0; i < 129; ++i) {
        EXPECT_EQ(first->prog0_blocks[i].diffuse[0], static_cast<float>(i));
        EXPECT_EQ(std::get<gfx::detail::MeshEntry>(first->entries[i]).prog0, i);
    }

    // The hint follows actual size, not a permanent high-water allocation after a scene change.
    gfx::BeginRecording();
    auto after_empty = gfx::EndRecording();
    EXPECT_EQ(after_empty->entries.capacity(), 0u);
    EXPECT_EQ(after_empty->records.capacity(), 0u);
    EXPECT_EQ(after_empty->prog0_blocks.capacity(), 0u);
}

TEST(GfxGpuDisplayList, RecordingReservationToleratesSmallGrowthAndResetsWithRenderer) {
    {
        GfxFixture          fixture(kWidth, kHeight);
        gfx::Prog0Constants constants = {};
        gfx::BeginRecording();
        for (int i = 0; i < 129; ++i) {
            constants.specular_power = static_cast<uint32_t>(i);
            gfx::detail::RecordMesh({}, nullptr, &constants, nullptr);
        }
        auto first = gfx::EndRecording();
        gfx::BeginRecording();
        for (int i = 0; i < 130; ++i) {
            constants.specular_power = static_cast<uint32_t>(i);
            gfx::detail::RecordMesh({}, nullptr, &constants, nullptr);
        }
        auto second = gfx::EndRecording();
        EXPECT_EQ(second->entries.capacity(), first->entries.capacity());
        EXPECT_EQ(second->records.capacity(), first->records.capacity());
        EXPECT_EQ(second->prog0_blocks.capacity(), first->prog0_blocks.capacity());
        EXPECT_EQ(second->prog0_blocks.size(), 130u);
        EXPECT_EQ(first->prog0_blocks.size(), 129u);
    }
    {
        GfxFixture fixture(kWidth, kHeight);
        gfx::BeginRecording();
        auto empty = gfx::EndRecording();
        EXPECT_EQ(empty->entries.capacity(), 0u);
        EXPECT_EQ(empty->records.capacity(), 0u);
        EXPECT_EQ(empty->prog0_blocks.capacity(), 0u);
    }
}

TEST(GfxGpuDisplayList, ConsecutiveIdenticalProg0BlocksShareOneRecord) {
    GfxFixture          fixture(kWidth, kHeight);
    gfx::Prog0Constants first = {};
    first.flags = gfx::kProg0Unlit;
    gfx::Prog0Constants second = first;
    second.specular_power = 1;
    gfx::BeginRecording();
    gfx::detail::RecordMesh({}, nullptr, &first, nullptr);
    gfx::detail::RecordMesh({}, nullptr, &first, nullptr);
    gfx::detail::RecordMesh({}, nullptr, &second, nullptr);
    auto list = gfx::EndRecording();
    ASSERT_EQ(list->prog0_blocks.size(), 2u);
    ASSERT_EQ(list->entries.size(), 3u);
    EXPECT_EQ(std::get<gfx::detail::MeshEntry>(list->entries[0]).prog0, 0);
    EXPECT_EQ(std::get<gfx::detail::MeshEntry>(list->entries[1]).prog0, 0);
    EXPECT_EQ(std::get<gfx::detail::MeshEntry>(list->entries[2]).prog0, 1);
}

TEST(GfxGpuDisplayList, RecordingOverlayKeepsSeparateCapacityHistory) {
    GfxFixture          fixture(kWidth, kHeight, 1.0f, Prog0Options());
    gfx::Prog0Constants constants{};
    gfx::BeginRecording();
    for (int i = 0; i < 129; ++i) {
        gfx::detail::RecordMesh({}, nullptr, &constants, nullptr);
    }
    auto game = gfx::EndRecording();
    auto overlay = OverlayRecord("60");
    ASSERT_NE(overlay, nullptr);
    EXPECT_LT(overlay->entries.capacity(), game->entries.size());
    EXPECT_EQ(overlay->records.capacity(), 0u);
    EXPECT_EQ(overlay->prog0_blocks.capacity(), 0u);
    gfx::BeginRecording();
    auto next_game = gfx::EndRecording();
    EXPECT_GE(next_game->entries.capacity(), game->entries.size());
    EXPECT_GE(next_game->records.capacity(), game->records.size());
    EXPECT_GE(next_game->prog0_blocks.capacity(), game->prog0_blocks.size());
    auto next_overlay = OverlayRecord("61");
    ASSERT_NE(next_overlay, nullptr);
    EXPECT_GE(next_overlay->entries.capacity(), overlay->entries.size());
    EXPECT_LT(next_overlay->entries.capacity(), game->entries.size());
    EXPECT_EQ(next_overlay->prog0_blocks.capacity(), 0u);
}

TEST(GfxDisplayList, BonesBlendPerBoneAtHalf) {
    gfx::Prog0Palette before = {};
    gfx::Prog0Palette after = {};
    for (uint32_t slot = 0; slot < gfx::kProg0Bones; slot++) {
        SetBone(before, slot, kIdentityRows);
        SetBone(after, slot, kIdentityRows);
    }

    const float root = std::sqrt(0.5f);
    const Rows  moved_translation = {1, 0, 0, 10, 0, 1, 0, 0, 0, 0, 1, 0};
    const Rows  rotated = {0, -1, 0, 0, 1, 0, 0, 0, 0, 0, 1, 0};
    const Rows  scaled = {3, 0, 0, 0, 0, 3, 0, 0, 0, 0, 3, 0};
    const Rows  teleported = {1, 0, 0, 500, 0, 1, 0, 0, 0, 0, 1, 0};
    const Rows  still = {1, 0, 0, 7, 0, 1, 0, 0, 0, 0, 1, 0};
    SetBone(after, 0, moved_translation);
    SetBone(after, 1, rotated);
    SetBone(after, 2, scaled);
    SetBone(after, 3, teleported);
    SetBone(before, 4, still);
    SetBone(after, 4, still);

    gfx::Prog0Palette expected = after;
    SetBone(expected, 0, {1, 0, 0, 5, 0, 1, 0, 0, 0, 0, 1, 0});
    SetBone(expected, 1, {root, -root, 0, 0, root, root, 0, 0, 0, 0, 1, 0});
    SetBone(expected, 2, {2, 0, 0, 0, 0, 2, 0, 0, 0, 0, 2, 0});

    gfx::Prog0Palette out = {};
    gfx::detail::BlendProg0Bones(before, after, 0.5f, 200.0f, out);
    for (uint32_t slot = 0; slot < gfx::kProg0Bones; slot++) {
        for (uint32_t r = 0; r < 3; r++) {
            for (uint32_t c = 0; c < 4; c++) {
                EXPECT_NEAR(out.bones[slot][r][c], expected.bones[slot][r][c], 1e-4f)
                    << "slot " << slot << " row " << r << " column " << c;
            }
        }
    }
    EXPECT_EQ(std::memcmp(out.bones[3], after.bones[3], sizeof out.bones[3]), 0);
    EXPECT_EQ(std::memcmp(out.bones[4], after.bones[4], sizeof out.bones[4]), 0);
}

TEST(GfxGpuMeshDc2, SkinnedPaletteInterpolatesInDisplayFrame) {
    GfxFixture                       fixture(kWidth, kHeight, 1.0f, Prog0Options());
    const std::vector<gfx::Vertex3D> vertices =
        Skinned({Vert({-0.2f, -0.2f, 0.5f}, {0, 0, 1}), Vert({0.2f, -0.2f, 0.5f}, {0, 0, 1}),
                 Vert({0.0f, 0.2f, 0.5f}, {0, 0, 1})},
                {1, 0, 0, 0}, {0, 0, 0, 0});
    gfx::MeshConstants constants = {};
    Identity(constants.mvp);
    gfx::MeshTransform  transform = gfx::IdentityMeshTransform();
    gfx::Prog0Constants block = {};
    block.flags = gfx::kProg0Unlit | gfx::kProg0Skinned;
    for (uint32_t k = 0; k < 4; k++) {
        block.object_color[k] = k == 3 ? 128.0f : 200.0f;
        block.diffuse[k] = 1.0f;
    }

    auto record = [&](float translation_x, bool cut) {
        gfx::Prog0Palette palette = {};
        SetBone(palette, 0, {1, 0, 0, translation_x, 0, 1, 0, 0, 0, 0, 1, 0});
        gfx::BeginRecording();
        gfx::SetInterpKey(1, false, 200.0f);
        if (cut) {
            gfx::CutInterpolation();
        }
        gfx::Clear(true, kClear.data(), true, 0.0f);
        gfx::DrawMeshImmediateProg0(vertices, kTriangle, constants, block, {}, {}, &transform, &palette);
        return gfx::EndRecording();
    };
    auto canonical = [&](const gfx::DisplayList &list) {
        gfx::RenderOptions options;
        options.canonical = true;
        EXPECT_TRUE(gfx::RenderList(list, 1.0f, options));
        return ReadCoverage(fixture);
    };

    gfx::DisplayListRef list_a = record(-0.25f, false);
    gfx::DisplayListRef list_b = record(0.25f, false);
    ASSERT_TRUE(list_a != nullptr);
    ASSERT_TRUE(list_b != nullptr);
    Coverage at_a = canonical(*list_a);
    Coverage at_b = canonical(*list_b);
    ASSERT_GT(at_a.count, 0);
    ASSERT_GT(at_b.count, 0);
    ASSERT_GT(std::fabs(at_b.x - at_a.x), 4.0);

    gfx::RenderOptions display;
    display.previous = list_a.get();
    display.present = true;
    ASSERT_TRUE(gfx::RenderList(*list_b, 0.5f, display));
    Coverage halfway = ReadCoverage(fixture);
    EXPECT_GT(halfway.count, 0);
    EXPECT_NEAR(halfway.x, (at_a.x + at_b.x) / 2.0, 1.0);

    gfx::DisplayListRef cut_b = record(0.25f, true);
    ASSERT_TRUE(cut_b != nullptr);
    ASSERT_TRUE(gfx::RenderList(*cut_b, 0.5f, display));
    Coverage after_cut = ReadCoverage(fixture);
    EXPECT_NEAR(after_cut.x, at_b.x, 1.0);
}

// One mesh drawn by two models, rewritten before each draw: every display frame takes each draw's
// own rewrite, interpolated with the one its match took.
TEST(GfxGpuMeshDc2, SharedMeshRewritesInterpolatePerDraw) {
    GfxFixture fixture(kWidth, kHeight);
    auto       triangle = [](float x) {
        return std::vector<gfx::Vertex3D>{Vert({x - 0.1f, -0.1f, 0.5f}, {0, 0, 1}),
                                          Vert({x + 0.1f, -0.1f, 0.5f}, {0, 0, 1}),
                                          Vert({x, 0.1f, 0.5f}, {0, 0, 1})};
    };
    auto mesh = gfx::CreateMesh(triangle(0), kTriangle);
    auto record = [&](float left, float right) {
        gfx::BeginRecording();
        gfx::Clear(true, kClear.data(), true, 0);
        uint32_t key = 1;
        for (float x : {left, right}) {
            gfx::SetInterpKey(key++, false, 200.0f);
            EXPECT_TRUE(gfx::UpdateMeshVertices(mesh, 0, triangle(x)));
            auto               transform = gfx::IdentityMeshTransform();
            gfx::MeshConstants constants = {};
            Identity(constants.mvp);
            constants.diffuse[0] = constants.diffuse[3] = 1;
            gfx::DrawMesh(mesh, 0, 3, constants, {}, {}, &transform);
        }
        return gfx::EndRecording();
    };
    // The mean x of the lit pixels in each half of the frame.
    auto halves = [&]() {
        EXPECT_TRUE(gfx::ReadbackFrame(fixture.pixels, fixture.width, fixture.height));
        std::array<Coverage, 2> out;
        std::array<double, 2>   sum = {};
        for (uint32_t y = 0; y < fixture.height; y++) {
            for (uint32_t x = 0; x < fixture.width; x++) {
                if (fixture.Pixel(x, y)[0] > 100) {
                    size_t half = x < fixture.width / 2 ? 0 : 1;
                    sum[half] += x;
                    out[half].count++;
                }
            }
        }
        for (size_t half = 0; half < 2; half++) {
            out[half].x = out[half].count > 0 ? sum[half] / out[half].count : 0.0;
        }
        return out;
    };
    auto canonical = [&](const gfx::DisplayList &list) {
        EXPECT_TRUE(gfx::RenderList(list, 1.0f, {.canonical = true}));
        return halves();
    };

    gfx::DisplayListRef     before = record(-0.6f, 0.4f);
    gfx::DisplayListRef     after = record(-0.4f, 0.6f);
    std::array<Coverage, 2> at_before = canonical(*before);
    std::array<Coverage, 2> at_after = canonical(*after);
    ASSERT_TRUE(gfx::RenderList(*after, 0.5f, {.previous = before.get(), .present = true}));
    std::array<Coverage, 2> halfway = halves();
    for (size_t half = 0; half < 2; half++) {
        ASSERT_GT(at_before[half].count, 0);
        ASSERT_GT(at_after[half].count, 0);
        EXPECT_GT(halfway[half].count, 0) << half;
        EXPECT_NEAR(halfway[half].x, (at_before[half].x + at_after[half].x) / 2.0, 1.0) << half;
    }

    // Without a previous list each draw still takes its own rewrite.
    ASSERT_TRUE(gfx::RenderList(*after, 0.5f, {.present = true}));
    std::array<Coverage, 2> uninterpolated = halves();
    for (size_t half = 0; half < 2; half++) {
        EXPECT_NEAR(uninterpolated[half].x, at_after[half].x, 1.0) << half;
    }
    gfx::DestroyMesh(mesh);
}

// A 16:9 window on the 4:3 logical frame leaves a 16 px bar at the right, which the right anchor
// shifts the quad into.
TEST(GfxGpuMeshDc2, AnchoredScissorMovesWithTheDraw) {
    constexpr int                kAnchorWidth = 128;
    constexpr int                kAnchorHeight = 72;
    constexpr uint32_t           kFrameRight = 112;
    const std::array<uint8_t, 4> black = {0, 0, 0, 0xFF};
    const std::array<uint8_t, 4> white = {0xFF, 0xFF, 0xFF, 0xFF};

    GfxFixture fixture(kAnchorWidth, kAnchorHeight, 1.0f);
    auto       draw = [&](bool follow_scissor) {
        fixture.Frame(black, [&] {
            gfx::UiAnchorScope anchor(gfx::UiAnchor::Side(1, -1, follow_scissor));
            gfx::DrawState     state;
            state.scissor = true;
            state.scissor_rect = {560.0f, 100.0f, 40.0f, 100.0f};
            gfx::Draw2D(gfx::Primitive::Quads, Quad(560.0f, 100.0f, 40.0f, 100.0f, white), {}, state);
        });
        int total = 0;
        int in_bar = 0;
        for (uint32_t y = 0; y < fixture.height; y++) {
            for (uint32_t x = 0; x < fixture.width; x++) {
                std::array<uint8_t, 4> p = fixture.Pixel(x, y);
                if (p[0] > 200 && p[1] > 200 && p[2] > 200) {
                    total++;
                    if (x >= kFrameRight) {
                        in_bar++;
                    }
                }
            }
        }
        return std::pair{total, in_bar};
    };

    auto [followed, followed_in_bar] = draw(true);
    EXPECT_GT(followed, 0);
    EXPECT_EQ(followed, followed_in_bar);

    auto [clipped, clipped_in_bar] = draw(false);
    EXPECT_EQ(clipped, 0);
    EXPECT_EQ(clipped_in_bar, 0);
}

namespace {

// A clip-space triangle that covers the whole target, flat red (2.0 is 0xFF through kModulate).
void DrawWindowRed(GfxFixture &fixture) {
    gfx::MeshConstants constants = {};
    Identity(constants.mvp);
    constants.diffuse[0] = 2.0f;
    constants.diffuse[3] = 2.0f;
    const std::vector<gfx::Vertex3D> vertices = {
        {{-1.0f, -1.0f, 0.5f}, {0, 0, 1}, {0, 0}, {}},
        {{3.0f, -1.0f, 0.5f},  {0, 0, 1}, {0, 0}, {}},
        {{-1.0f, 3.0f, 0.5f},  {0, 0, 1}, {0, 0}, {}},
    };
    fixture.Frame({0xFF, 0xFF, 0xFF, 0xFF}, [&] { gfx::DrawMeshImmediate(vertices, kTriangle, constants, {}, {}); });
}

} // namespace

// The bars are cleared to black when the renderer makes the main target, and the frame's clear
// and mesh must leave them that way.
TEST(GfxGpuMeshDc2, LetterboxClipsMeshesAndClearsToTheFrame) {
    constexpr int kLetterboxWidth = 128;
    constexpr int kLetterboxHeight = 72;

    GfxFixture fixture(kLetterboxWidth, kLetterboxHeight, 1.0f);
    gfx::SetFrameLayout(gfx::FrameLayout{gfx::AspectMode::Letterbox, 1.0f});
    DrawWindowRed(fixture);
    EXPECT_TRUE(fixture.PixelNear(4, 36, 0, 0, 0));
    EXPECT_TRUE(fixture.PixelNear(123, 36, 0, 0, 0));
    EXPECT_TRUE(fixture.PixelNear(64, 36, 255, 0, 0));
    gfx::SetFrameLayout(gfx::FrameLayout{gfx::AspectMode::Fill, 1.0f});
}

TEST(GfxGpuDisplayList, HostOverlayRemainsVisibleInLetterboxBars) {
    GfxFixture fixture(1360, 768, 1.0f);
    gfx::SetFrameLayout({gfx::AspectMode::Letterbox, 1.0f});
    gfx::BeginRecording();
    const uint8_t clear[4] = {0, 0, 0, 255};
    gfx::Clear(true, clear, true, 0);
    auto game = gfx::EndRecording();
    ASSERT_NE(game, nullptr);
    ASSERT_TRUE(gfx::RenderList(*game, 1, {.canonical = true}));
    const auto overlay = OverlayRecord("60 FPS");
    ASSERT_NE(overlay, nullptr);
    ASSERT_TRUE(gfx::RenderList(*game, 1, {.present = true, .overlay = overlay.get()}));
    ASSERT_TRUE(gfx::ReadbackFrame(fixture.pixels, fixture.width, fixture.height));
    bool visible = false;
    // The 4:3 frame starts at x=168; the entire counter fits inside the left bar.
    for (uint32_t y = 0; y < 64; ++y) {
        for (uint32_t x = 0; x < 168; ++x) {
            const auto pixel = fixture.Pixel(x, y);
            visible = visible || pixel[0] > 128 || pixel[1] > 128 || pixel[2] > 128;
        }
    }
    EXPECT_TRUE(visible);
    EXPECT_TRUE(fixture.PixelNear(4, 384, 0, 0, 0));
    gfx::SetFrameLayout({gfx::AspectMode::Fill, 1.0f});
}

TEST(GfxGpuMeshDc2, FillKeepsClearsAcrossTheWindow) {
    constexpr int kFillWidth = 128;
    constexpr int kFillHeight = 72;

    GfxFixture fixture(kFillWidth, kFillHeight, 1.0f);
    DrawWindowRed(fixture);
    EXPECT_TRUE(fixture.PixelNear(4, 36, 255, 0, 0));
    EXPECT_TRUE(fixture.PixelNear(123, 36, 255, 0, 0));
    EXPECT_TRUE(fixture.PixelNear(64, 36, 255, 0, 0));
    gfx::SetFrameLayout(gfx::FrameLayout{gfx::AspectMode::Fill, 1.0f});
}

namespace {
gfx::detail::Draw2DEntry PresentationQuad(float x, float y, uint8_t opacity) {
    gfx::detail::Draw2DEntry draw{};
    draw.primitive = gfx::Primitive::Quads;
    draw.interpolation = {17, 3};
    draw.vertices = {
        {x,     y,     0, 0, 0, {128, 32, 16, opacity}},
        {x + 8, y,     0, 8, 0, {128, 32, 16, opacity}},
        {x + 8, y + 8, 0, 8, 8, {128, 32, 16, opacity}},
        {x,     y + 8, 0, 0, 8, {128, 32, 16, opacity}}
    };
    return draw;
}
} // namespace

TEST(GfxDisplayList, Keyed2DInterpolatesOnlyScreenPositionAndOpacity) {
    auto before = PresentationQuad(8, 12, 32);
    auto after = PresentationQuad(24, 20, 96);
    after.vertices[0].color[0] = 200;
    std::vector<gfx::Vertex2D> result;
    for (float alpha : {0.0f, 0.25f, 0.5f, 0.75f}) {
        ASSERT_TRUE(gfx::detail::Interpolate2D(before, after, alpha, result));
        for (size_t i = 0; i < result.size(); ++i) {
            EXPECT_FLOAT_EQ(result[i].x, before.vertices[i].x + 16 * alpha);
            EXPECT_FLOAT_EQ(result[i].y, before.vertices[i].y + 8 * alpha);
            EXPECT_EQ(result[i].color[3], 32 + 64 * alpha);
            EXPECT_EQ(result[i].color[0], after.vertices[i].color[0]);
            EXPECT_EQ(result[i].u, after.vertices[i].u);
            EXPECT_EQ(result[i].z, after.vertices[i].z);
        }
    }
    EXPECT_FALSE(gfx::detail::Interpolate2D(before, after, 1.0f, result));
    EXPECT_EQ(std::memcmp(result.data(), after.vertices.data(), result.size() * sizeof(result[0])), 0);
}

TEST(GfxDisplayList, Keyed2DRejectsReusedOrIncompatibleDraws) {
    const auto                            before = PresentationQuad(8, 12, 32);
    const auto                            after = PresentationQuad(24, 20, 96);
    std::vector<gfx::detail::Draw2DEntry> incompatible;
    auto                                  change = [&](auto mutate) { auto draw = after; mutate(draw); incompatible.push_back(draw); };
    change([](auto &d) { d.interpolation.key = 0; });
    change([](auto &d) { ++d.interpolation.key; });
    change([](auto &d) { ++d.interpolation.generation; });
    change([](auto &d) { ++d.occurrence; });
    change([](auto &d) { ++d.target; });
    change([](auto &d) { d.primitive = gfx::Primitive::Triangles; });
    change([](auto &d) { d.vertices.pop_back(); });
    change([](auto &d) { ++d.binding.texture; });
    change([](auto &d) { ++d.binding.palette; });
    change([](auto &d) { d.binding.filter = gfx::Filter::Nearest; });
    change([](auto &d) { d.binding.wrap_u = gfx::Wrap::Repeat; });
    change([](auto &d) { d.state.blend = true; });
    change([](auto &d) { d.state.scissor = true; });
    change([](auto &d) { d.state.scissor_rect.x = 8; });
    change([](auto &d) { d.state.alpha_ref = 7; });
    change([](auto &d) { d.state.stencil_front.reference = 2; });
    change([](auto &d) { d.side.x = 1; });
    change([](auto &d) { d.scene = 0; });
    change([](auto &d) { d.state.depth_test = gfx::DepthTest::Greater; });
    change([](auto &d) { d.state.depth_write = true; });
    change([](auto &d) { d.vertices[2].u += 1; });
    change([](auto &d) { d.vertices[2].v += 1; });
    change([](auto &d) { d.vertices[2].z += 1; });
    change([](auto &d) { d.vertices[2].fog = 0; });
    change([](auto &d) { d.vertices[2].x = INFINITY; });
    for (size_t i = 0; i < incompatible.size(); ++i) {
        std::vector<gfx::Vertex2D> result;
        EXPECT_FALSE(gfx::detail::Interpolate2D(before, incompatible[i], 0.5f, result)) << i;
        EXPECT_EQ(std::memcmp(result.data(), incompatible[i].vertices.data(), result.size() * sizeof(result[0])), 0) << i;
    }
}

TEST(GfxGpuDisplayList, Keyed2DPresentationIsOptInAndCanonicalExact) {
    GfxFixture fixture(64, 48, 1.0f, true, 64, 48);
    auto       record = [&](float x, float y, uint8_t opacity, uint64_t generation, bool reverse, int cut) {
        gfx::BeginRecording();
        gfx::Clear(true, kClear.data(), true, 0);
        if (cut == 1) {
            gfx::CutInterpolation();
        }
        if (cut == 2) {
            gfx::CutCameraInterpolation();
        }
        auto hud = PresentationQuad(40, 28, 128);
        hud.vertices[0].color[1] = 128;
        auto damage = PresentationQuad(x, y, opacity);
        auto draw_damage = [&] {
            gfx::Interp2DScope scope({generation ? 17u : 0u, generation});
            gfx::Draw2D(damage.primitive, damage.vertices, damage.binding, damage.state);
        };
        auto draw_hud = [&] { gfx::Draw2D(hud.primitive, hud.vertices, hud.binding, hud.state); };
        if (reverse) {
            draw_hud();
            draw_damage();
        } else {
            draw_damage();
            draw_hud();
        }
        return gfx::EndRecording();
    };
    auto render = [&](const gfx::DisplayList &list, float alpha, gfx::RenderOptions options) {
        EXPECT_TRUE(gfx::RenderList(list, alpha, options));
        EXPECT_TRUE(gfx::ReadbackFrame(fixture.pixels, fixture.width, fixture.height));
        return fixture.pixels;
    };
    auto before = record(8, 8, 32, 3, true, 0);
    render(*before, 1, {.canonical = true});
    auto       after = record(24, 12, 96, 3, false, 0);
    const auto newest = render(*after, 1, {.canonical = true});
    for (float alpha : {0.0f, 0.25f, 0.5f, 0.75f, 1.0f}) {
        auto actual = render(*after, alpha, {.previous = before.get()});
        auto expected = record(8 + 16 * alpha, 8 + 4 * alpha, uint8_t(32 + 64 * alpha), 0, false, 0);
        EXPECT_EQ(actual, render(*expected, 1, {})) << alpha;
        EXPECT_EQ(render(*after, 0.5f, {.canonical = true}), newest);
    }
    EXPECT_EQ(render(*after, 0, {}), newest); // new object, no predecessor
    EXPECT_EQ(render(*after, 1, {.previous = before.get()}), newest);
    for (int cut : {1, 2}) {
        auto cut_list = record(24, 12, 96, 3, false, cut);
        render(*cut_list, 1, {.canonical = true});
        EXPECT_EQ(render(*cut_list, 0, {.previous = before.get()}), newest);
    }
    auto reused = record(24, 12, 96, 4, false, 0);
    render(*reused, 1, {.canonical = true});
    EXPECT_EQ(render(*reused, 0, {.previous = before.get()}), newest);
    auto untagged = record(24, 12, 96, 0, false, 0);
    render(*untagged, 1, {.canonical = true});
    EXPECT_EQ(render(*untagged, 0, {.previous = before.get()}), newest);
}

// One shared model has several faces per placement; culling and sorting can reorder either.
TEST(GfxGpuMeshDc2, InstancedFacesKeepMeshAndTextureIdentity) {
    GfxFixture                       fixture(kWidth, kHeight);
    const std::vector<gfx::Vertex3D> vertices = {
        Vert({-0.1f, -0.1f, 0.5f}, {0, 0, 1}), Vert({0.1f, -0.1f, 0.5f}, {0, 0, 1}),
        Vert({0, 0.1f, 0.5f}, {0, 0, 1})};
    gfx::MeshHandle mesh_a = gfx::CreateMesh(vertices, kTriangle);
    gfx::MeshHandle mesh_b = gfx::CreateMesh(vertices, kTriangle);
    ASSERT_NE(mesh_a, gfx::kNullMesh);
    ASSERT_NE(mesh_b, gfx::kNullMesh);
    gfx::TextureHandle texture_a = gfx::CreateTexture({1, 1});
    gfx::TextureHandle texture_b = gfx::CreateTexture({1, 1});
    const uint8_t      white[4] = {128, 128, 128, 128};
    ASSERT_TRUE(gfx::UpdateTexture(texture_a, 0, 0, 0, 1, 1, white));
    ASSERT_TRUE(gfx::UpdateTexture(texture_b, 0, 0, 0, 1, 1, white));
    auto record = [&](bool reordered) {
        gfx::BeginRecording();
        gfx::Clear(true, kClear.data(), true, 0.0f);
        gfx::SetInterpKey(77);
        auto draw = [&](float x, gfx::MeshHandle mesh, gfx::TextureHandle texture) {
            auto transform = gfx::IdentityMeshTransform();
            transform.model[12] = x;
            gfx::MeshConstants constants = {};
            std::memcpy(constants.mvp, transform.model, sizeof constants.mvp);
            constants.diffuse[0] = constants.diffuse[3] = 1.0f;
            gfx::TextureBinding binding;
            binding.texture = texture;
            gfx::DrawMesh(mesh, 0, 3, constants, binding, {}, &transform);
        };
        if (!reordered) {
            draw(-0.4f, mesh_a, texture_a);
            draw(-0.4f, mesh_b, texture_a);
            draw(-0.4f, mesh_a, texture_b);
            draw(0.4f, mesh_a, texture_a);
            draw(0.4f, mesh_b, texture_a);
            draw(0.4f, mesh_a, texture_b);
        } else {
            // Moving right place comes first; the stationary left place lost one face to culling.
            draw(0.5f, mesh_a, texture_b);
            draw(0.5f, mesh_b, texture_a);
            draw(0.5f, mesh_a, texture_a);
            draw(-0.4f, mesh_b, texture_a);
        }
        return gfx::EndRecording();
    };
    auto before = record(false);
    auto after = record(true);
    ASSERT_TRUE(gfx::RenderList(*after, 1, {.canonical = true}));
    ASSERT_TRUE(gfx::RenderList(*after, 0.5f, {.previous = before.get(), .present = true}));
    ASSERT_EQ(after->cache.match.size(), 4u);
    EXPECT_EQ(after->cache.match[0], 5);
    EXPECT_EQ(after->cache.match[1], 4);
    EXPECT_EQ(after->cache.match[2], 3);
    EXPECT_EQ(after->cache.match[3], 1);
    gfx::DestroyMesh(mesh_a);
    gfx::DestroyMesh(mesh_b);
    gfx::DestroyTexture(texture_a);
    gfx::DestroyTexture(texture_b);
}

// A newly visible (or material-cut) draw has no pose predecessor, but still belongs to the
// interpolated camera. Its Prog0 clip planes must follow the same camera as its ordinary MVP.
TEST(GfxGpuMeshDc2, UnmatchedProg0ClipFollowsInterpolatedCamera) {
    GfxFixture                       fixture(kWidth, kHeight, 1.0f, Prog0Options());
    const std::vector<gfx::Vertex3D> vertices = {
        Vert({0.1f, -0.2f, 0.5f}, {0, 0, 1}), Vert({0.5f, -0.2f, 0.5f}, {0, 0, 1}),
        Vert({0.3f, 0.2f, 0.5f}, {0, 0, 1})};
    auto record = [&](float camera_x, gfx::InterpKey key) {
        auto transform = gfx::IdentityMeshTransform();
        transform.view[12] = camera_x;
        gfx::MeshConstants constants = {};
        std::memcpy(constants.mvp, transform.view, sizeof constants.mvp);
        gfx::Prog0Constants block = {};
        Identity(block.model_world);
        std::memcpy(block.clip_or_view, transform.view, sizeof block.clip_or_view);
        block.flags = gfx::kProg0Unlit | gfx::kProg0Scissor;
        for (int k = 0; k < 4; ++k) {
            block.object_color[k] = k == 3 ? 128.0f : 200.0f;
            block.diffuse[k] = 1;
        }
        gfx::BeginRecording();
        gfx::Clear(true, kClear.data(), true, 0);
        gfx::SetInterpKey(key);
        gfx::DrawMeshImmediateProg0(vertices, kTriangle, constants, block, {}, {}, &transform);
        return gfx::EndRecording();
    };
    auto before = record(0, 1);
    auto newest = record(1.0f, 2);
    auto expected = record(0.5f, 3);
    ASSERT_TRUE(gfx::RenderList(*expected, 1, {.canonical = true}));
    auto coverage = ReadCoverage(fixture);
    ASSERT_GT(coverage.count, 0);
    ASSERT_TRUE(gfx::RenderList(*newest, 1, {.canonical = true}));
    ASSERT_TRUE(gfx::RenderList(*newest, 0.5f, {.previous = before.get(), .present = true}));
    auto display = ReadCoverage(fixture);
    ASSERT_EQ(newest->cache.match[0], -1);
    EXPECT_EQ(display.count, coverage.count);
    EXPECT_NEAR(display.x, coverage.x, 0.01);
}

TEST(GfxGpuMeshDc2, TextureChangeCutsPoseMatching) {
    GfxFixture                       fixture(kWidth, kHeight);
    const std::vector<gfx::Vertex3D> vertices = {
        Vert({-0.1f, -0.1f, 0.5f}, {0, 0, 1}), Vert({0.1f, -0.1f, 0.5f}, {0, 0, 1}),
        Vert({0, 0.1f, 0.5f}, {0, 0, 1})};
    auto          texture_a = gfx::CreateTexture({1, 1});
    auto          texture_b = gfx::CreateTexture({1, 1});
    const uint8_t white[4] = {128, 128, 128, 128};
    ASSERT_TRUE(gfx::UpdateTexture(texture_a, 0, 0, 0, 1, 1, white));
    ASSERT_TRUE(gfx::UpdateTexture(texture_b, 0, 0, 0, 1, 1, white));
    auto record = [&](gfx::TextureBinding binding) {
        gfx::BeginRecording();
        gfx::Clear(true, kClear.data(), true, 0);
        gfx::SetInterpKey(88);
        auto               transform = gfx::IdentityMeshTransform();
        gfx::MeshConstants constants = {};
        Identity(constants.mvp);
        gfx::DrawMeshImmediate(vertices, kTriangle, constants, binding, {}, &transform);
        return gfx::EndRecording();
    };
    for (bool palette : {false, true}) {
        gfx::TextureBinding a, b;
        a.texture = b.texture = texture_a;
        if (palette) {
            a.palette = texture_a;
            b.palette = texture_b;
        } else {
            b.texture = texture_b;
        }
        auto before = record(a), after = record(b);
        ASSERT_TRUE(gfx::RenderList(*after, 1, {.canonical = true}));
        ASSERT_TRUE(gfx::RenderList(*after, 0.5f, {.previous = before.get(), .present = true}));
        EXPECT_EQ(after->cache.match[0], -1);
    }
    gfx::DestroyTexture(texture_a);
    gfx::DestroyTexture(texture_b);
}

TEST(GfxGpuMeshDc2, MirroredInstancesAtSameOriginKeepHandedness) {
    GfxFixture                       fixture(kWidth, kHeight);
    const std::vector<gfx::Vertex3D> vertices = {
        Vert({-0.1f, -0.1f, 0.5f}, {0, 0, 1}), Vert({0.1f, -0.1f, 0.5f}, {0, 0, 1}),
        Vert({0, 0.1f, 0.5f}, {0, 0, 1})};
    auto mesh = gfx::CreateMesh(vertices, kTriangle);
    auto record = [&](float x) {
        gfx::BeginRecording();
        gfx::Clear(true, kClear.data(), true, 0);
        gfx::SetInterpKey(99);
        for (float mirror : {1.0f, -1.0f}) {
            auto transform = gfx::IdentityMeshTransform();
            transform.model[5] = mirror;
            transform.model[12] = x;
            gfx::MeshConstants constants = {};
            std::memcpy(constants.mvp, transform.model, sizeof constants.mvp);
            constants.diffuse[0] = constants.diffuse[3] = 1;
            gfx::DrawMesh(mesh, 0, 3, constants, {}, {}, &transform);
        }
        return gfx::EndRecording();
    };
    auto before = record(0), after = record(0.1f);
    ASSERT_TRUE(gfx::RenderList(*after, 1, {.canonical = true}));
    ASSERT_TRUE(gfx::RenderList(*after, 0.5f, {.previous = before.get(), .present = true}));
    EXPECT_EQ(after->cache.match[0], 0);
    EXPECT_EQ(after->cache.match[1], 1);
    gfx::DestroyMesh(mesh);
}

// Tint changes presentation RGB only, including draws with no pose predecessor or transform.
TEST(GfxGpuMeshDc2, InterpolationTintClassifiesGeometryAndPreservesCanonical) {
    GfxFixture                       fixture(kWidth, kHeight);
    const std::vector<gfx::Vertex3D> vertices = {
        Vert({-1, -1, 0.5f}, {0, 0, 1}), Vert({3, -1, 0.5f}, {0, 0, 1}),
        Vert({-1, 3, 0.5f}, {0, 0, 1})};
    auto record = [&](gfx::InterpKey key, bool transformed, float camera) {
        gfx::BeginRecording();
        gfx::Clear(true, kClear.data(), true, 0);
        gfx::SetInterpKey(key);
        auto transform = gfx::IdentityMeshTransform();
        transform.view[12] = camera;
        gfx::MeshConstants constants = {};
        std::memcpy(constants.mvp, transform.view, sizeof constants.mvp);
        constants.diffuse[0] = constants.diffuse[1] = constants.diffuse[2] = constants.diffuse[3] = 1;
        gfx::DrawMeshImmediate(vertices, kTriangle, constants, {}, {}, transformed ? &transform : nullptr);
        return gfx::EndRecording();
    };
    auto before = record(71, true, 0);
    auto matched = record(71, true, 0.1f);
    auto unmatched = record(72, true, 0.1f);
    auto newest = record(0, false, 0.1f);
    auto check = [&](const gfx::DisplayListRef &list, int r, int g, int b) {
        ASSERT_TRUE(gfx::RenderList(*list, 1, {.canonical = true}));
        ASSERT_TRUE(gfx::ReadbackFrame(fixture.pixels, fixture.width, fixture.height));
        auto canonical = fixture.pixels;
        ASSERT_TRUE(gfx::RenderList(*list, 0, {.canonical = true, .interpolation_tint = true}));
        ASSERT_TRUE(gfx::ReadbackFrame(fixture.pixels, fixture.width, fixture.height));
        EXPECT_EQ(fixture.pixels, canonical);
        ASSERT_TRUE(gfx::RenderList(*list, 0.5f, {.previous = before.get(), .present = true, .interpolation_tint = true}));
        ASSERT_TRUE(gfx::ReadbackFrame(fixture.pixels, fixture.width, fixture.height));
        EXPECT_TRUE(fixture.PixelNear(kWidth / 2, kHeight / 2, r, g, b, 3));
        // No diagnostic state leaks into the next ordinary display replay.
        ASSERT_TRUE(gfx::RenderList(*list, 1, {.present = true}));
        ASSERT_TRUE(gfx::ReadbackFrame(fixture.pixels, fixture.width, fixture.height));
        EXPECT_EQ(fixture.pixels, canonical);
    };
    check(matched, 46, 230, 46);
    check(unmatched, 46, 230, 230);
    check(newest, 230, 46, 46);
    gfx::BeginRecording();
    gfx::Clear(true, kClear.data(), true, 0);
    gfx::Draw2D(gfx::Primitive::Quads, Quad(0, 0, 640, 480, {255, 255, 255, 128}), {}, {});
    auto ui = gfx::EndRecording();
    check(ui, 71, 92, 255);
    gfx::BeginRecording();
    gfx::Clear(true, kClear.data(), true, 0);
    gfx::SetInterpKey(71);
    auto transform = gfx::IdentityMeshTransform();
    transform.view[12] = 0.1f;
    gfx::MeshConstants constants = {};
    std::memcpy(constants.mvp, transform.view, sizeof constants.mvp);
    gfx::DrawMeshImmediate(vertices, kTriangle, constants, {}, {}, &transform);
    gfx::DrawState sprite_state;
    sprite_state.depth_test = gfx::DepthTest::GEqual;
    gfx::Draw2D(gfx::Primitive::Quads, Quad(0, 0, 640, 480, {255, 255, 255, 128}), {}, sprite_state);
    auto billboard = gfx::EndRecording();
    check(billboard, 255, 255, 71);
}

// Display diagnostics must not colour reusable targets that a later canonical tick can sample.
TEST(GfxGpuMeshDc2, InterpolationTintPreservesReusableTargetPixels) {
    GfxFixture fixture(kWidth, kHeight);
    const auto target = gfx::CreateRenderTarget(640, 480, true, true);
    ASSERT_NE(target, gfx::kNullTexture);
    gfx::BeginRecording();
    gfx::SetRenderTarget(target);
    gfx::Clear(true, kClear.data(), false, 0);
    gfx::Draw2D(gfx::Primitive::Quads, Quad(0, 0, 640, 480, {255, 255, 255, 128}), {}, {});
    gfx::SetRenderTarget(gfx::kMainTarget);
    gfx::Clear(true, kClear.data(), true, 0);
    gfx::TextureBinding binding;
    binding.texture = target;
    gfx::Draw2D(gfx::Primitive::Quads, Quad(0, 0, 640, 480, {128, 128, 128, 128}), binding, {});
    auto list = gfx::EndRecording();
    ASSERT_TRUE(gfx::RenderList(*list, 1, {.canonical = true}));
    std::vector<uint8_t> before, after;
    uint32_t             width = 0, height = 0;
    ASSERT_TRUE(gfx::ReadbackTexture(target, before, width, height));
    ASSERT_TRUE(gfx::RenderList(*list, 1, {.present = true, .interpolation_tint = true}));
    ASSERT_TRUE(gfx::ReadbackTexture(target, after, width, height));
    size_t changed = 0;
    for (size_t i = 0; i < before.size(); ++i) {
        changed += before[i] != after[i];
    }
    EXPECT_EQ(changed, 0u);
    gfx::DestroyTexture(target);
}

TEST(GfxDisplayList, WorldSpriteInterpolatesMotionDepthAndCamera) {
    auto before = PresentationQuad(8, 12, 32);
    auto after = PresentationQuad(24, 20, 96);
    before.scene = after.scene = 0;
    before.state.depth_test = after.state.depth_test = gfx::DepthTest::Greater;
    before.interpolation.world_corners = after.interpolation.world_corners = true;
    gfx::detail::Mat4 view{};
    view[0] = view[5] = view[10] = view[15] = 1;
    gfx::detail::SpriteScene scene{
        0, view, {1, 1, 0, 0}
    };
    auto newest_view = view;
    newest_view[12] = 4;
    for (auto &v : before.vertices) {
        v.z = 0.25f;
    }
    for (auto &v : after.vertices) {
        v.z = 0.75f;
        v.u += 16;
    }
    std::vector<gfx::Vertex2D> result;
    for (float alpha : {0.0f, 0.25f, 0.5f, 0.75f}) {
        auto display_view = view;
        display_view[12] = 4 * alpha;
        ASSERT_TRUE(gfx::detail::InterpolateSprite(before, scene, view, after, scene, newest_view,
                                                   display_view, alpha, result));
        EXPECT_FLOAT_EQ(result[0].x, 8 + 16 * alpha);
        EXPECT_FLOAT_EQ(result[0].y, 12 + 8 * alpha);
        EXPECT_FLOAT_EQ(result[0].z, 0.25f + 0.5f * alpha);
        EXPECT_EQ(result[0].color[3], 32 + 64 * alpha);
        EXPECT_EQ(result[0].u, after.vertices[0].u); // coin spin remains a discrete atlas frame
    }
    EXPECT_FALSE(gfx::detail::InterpolateSprite(before, scene, view, after, scene, newest_view,
                                                newest_view, 1, result));
    EXPECT_EQ(std::memcmp(result.data(), after.vertices.data(), result.size() * sizeof(result[0])), 0);
}

TEST(GfxGpuDisplayList, WorldSpriteRecordingBlendsOwnMotionAndKeepsCanonicalExact) {
    GfxFixture fixture(64, 48, 1.0f, true, 64, 48);
    auto       record = [&](float x, uint64_t generation, bool tagged, int cut, bool multiple_views = false) {
        gfx::BeginRecording();
        gfx::Clear(true, kClear.data(), true, 0);
        if (cut == 1) {
            gfx::CutInterpolation();
        }
        if (cut == 2) {
            gfx::CutCameraInterpolation();
        }
        gfx::MeshTransform transform = gfx::IdentityMeshTransform();
        gfx::MeshConstants constants{};
        Identity(constants.mvp);
        const std::vector<gfx::Vertex3D> background = {Vert({-1, -1, 0}, {0, 0, 1}),
                                                       Vert({3, -1, 0}, {0, 0, 1}),
                                                       Vert({-1, 3, 0}, {0, 0, 1})};
        gfx::DrawMeshImmediate(background, kTriangle, constants, {}, {}, &transform);
        if (multiple_views) {
            transform.view[12] = 4;
            gfx::DrawMeshImmediate(background, kTriangle, constants, {}, {}, &transform);
        }
        auto sprite = PresentationQuad(x, 12, 128);
        for (auto &v : sprite.vertices) {
            v.z = 0.5f;
        }
        sprite.state.depth_test = gfx::DepthTest::Greater;
        gfx::Interp2DScope scope({tagged ? 17u : 0u, generation, tagged});
        gfx::Draw2D(sprite.primitive, sprite.vertices, {}, sprite.state);
        return gfx::EndRecording();
    };
    auto render = [&](const gfx::DisplayList &list, float alpha, gfx::RenderOptions options) {
        options.present = !options.canonical;
        EXPECT_TRUE(gfx::RenderList(list, alpha, options));
        EXPECT_TRUE(gfx::ReadbackFrame(fixture.pixels, fixture.width, fixture.height));
        return fixture.pixels;
    };
    auto before = record(8, 3, true, 0);
    auto oldest = render(*before, 1, {.canonical = true});
    auto after = record(24, 3, true, 0);
    auto newest = render(*after, 1, {.canonical = true});
    ASSERT_NE(oldest, newest);
    for (float alpha : {0.0f, 0.25f, 0.5f, 0.75f, 1.0f}) {
        auto actual = render(*after, alpha, {.previous = before.get()});
        auto expected = record(8 + 16 * alpha, 3, false, 0);
        EXPECT_EQ(actual, render(*expected, 1, {})) << alpha;
        EXPECT_EQ(render(*after, alpha, {.canonical = true}), newest);
    }
    for (int cut : {0, 1, 2}) {
        auto reused = record(24, cut ? 3 : 4, true, cut);
        render(*reused, 1, {.canonical = true});
        EXPECT_EQ(render(*reused, 0, {.previous = before.get()}), newest);
    }
    EXPECT_EQ(render(*after, 0, {}), newest);
    auto multi_before = record(8, 3, true, 0, true);
    render(*multi_before, 1, {.canonical = true});
    auto multi_after = record(24, 3, true, 0, true);
    auto multi_newest = render(*multi_after, 1, {.canonical = true});
    EXPECT_EQ(render(*multi_after, 0.5f, {.previous = multi_before.get()}), multi_newest);
}

TEST(GfxDisplayList, WorldSpritePerspectiveUsesWorldEndpoints) {
    auto before = PresentationQuad(1, 0, 128);
    auto after = PresentationQuad(2, 0, 128);
    before.scene = after.scene = 0;
    before.state.depth_test = after.state.depth_test = gfx::DepthTest::Greater;
    before.interpolation.world_corners = after.interpolation.world_corners = true;
    for (auto &v : before.vertices) {
        v.z = 0.5f;
    }
    for (auto &v : after.vertices) {
        v.z = 0.25f;
    }
    gfx::detail::Mat4 view{};
    view[0] = view[5] = view[10] = view[15] = 1;
    auto projection = view;
    projection[10] = projection[15] = 0;
    projection[11] = projection[14] = 1; // clip w = world z; reversed depth = 1/z
    gfx::detail::SpriteScene scene{
        0, projection, {1, 1, 0, 0}
    };
    std::vector<gfx::Vertex2D> result;
    ASSERT_TRUE(gfx::detail::InterpolateSprite(before, scene, view, after, scene, view, view, 0.5f, result));
    EXPECT_FLOAT_EQ(result[0].x, 5.0f / 3.0f); // (world x,z)=(2,2) -> (8,4), midpoint (5,3)
    EXPECT_FLOAT_EQ(result[0].z, 1.0f / 3.0f);
}

TEST(GfxDisplayList, WorldSpriteRejectsReuseTeleportAndIncompatibleViews) {
    auto before = PresentationQuad(8, 12, 32);
    auto after = PresentationQuad(24, 20, 96);
    before.scene = after.scene = 0;
    before.state.depth_test = after.state.depth_test = gfx::DepthTest::Greater;
    before.interpolation.world_corners = after.interpolation.world_corners = true;
    for (auto &v : before.vertices) {
        v.z = 0.5f;
    }
    for (auto &v : after.vertices) {
        v.z = 0.5f;
    }
    gfx::detail::Mat4 view{};
    view[0] = view[5] = view[10] = view[15] = 1;
    gfx::detail::SpriteScene scene{
        0, view, {1, 1, 0, 0}
    };
    auto reject = [&](auto draw, auto new_scene, auto display_view) {
        std::vector<gfx::Vertex2D> result;
        EXPECT_FALSE(gfx::detail::InterpolateSprite(before, scene, view, draw, new_scene, view,
                                                    display_view, 0.5f, result));
        EXPECT_EQ(std::memcmp(result.data(), draw.vertices.data(), result.size() * sizeof(result[0])), 0);
    };
    auto change = [&](auto mutate) { auto draw = after; mutate(draw); reject(draw, scene, view); };
    change([](auto &d) { ++d.interpolation.generation; });
    change([](auto &d) { d.interpolation.key = 0; });
    change([](auto &d) { d.interpolation.world_corners = false; });
    change([](auto &d) { d.interpolation.teleport_distance = 10; });
    change([](auto &d) { d.vertices[0].x = INFINITY; });
    change([](auto &d) { d.vertices[0].z = -0.1f; });
    change([](auto &d) { ++d.binding.texture; });
    change([](auto &d) { ++d.target; });
    change([](auto &d) { ++d.occurrence; });
    change([](auto &d) { d.vertices.pop_back(); });
    auto changed_scene = scene;
    changed_scene.projection[0] = 2;
    reject(after, changed_scene, view);
    changed_scene = scene;
    changed_scene.to_clip[0] = 2;
    reject(after, changed_scene, view);
    auto behind = view;
    behind[15] = -1;
    reject(after, scene, behind);
    auto near_crossing = view;
    near_crossing[14] = -1;
    reject(after, scene, near_crossing);
    reject(after, scene, gfx::detail::Mat4{});
}

TEST(GfxGpuDisplayList, PresentationCompositeKeepsMovingMeshAndClearsReusedTarget) {
    GfxFixture fixture(64, 48, 1.0f, true, 64, 48);
    const auto target = gfx::NamedRenderTarget("moving-composite", 64, 48, true, true);
    ASSERT_NE(target, gfx::kNullTexture);
    const uint8_t                    transparent[4] = {0, 0, 0, 0};
    const uint8_t                    stale[4] = {0, 255, 0, 128};
    const gfx::LogicalRect           full{0, 0, 64, 48};
    const std::vector<gfx::Vertex3D> vertices = {
        Vert({-0.2f, -0.2f, 0.5f}, {0, 0, 1}),
        Vert({0.2f, -0.2f, 0.5f}, {0, 0, 1}),
        Vert({0, 0.2f, 0.5f}, {0, 0, 1})};
    auto record = [&](float x, bool replacement) {
        gfx::BeginRecording();
        gfx::SetRenderTarget(gfx::kMainTarget);
        gfx::Clear(true, kClear.data(), true, 0);
        gfx::SetRenderTarget(target);
        // A shared scratch image can contain another object's pixels outside the tick's box.
        gfx::Clear(true, stale, false, 0);
        const gfx::LogicalRect box{(x + 0.8f) * 32 - 1, 18, 15, 12};
        gfx::Clear(true, transparent, false, 0, &box, replacement ? &full : nullptr);
        gfx::MeshConstants constants{};
        Identity(constants.mvp);
        constants.mvp[12] = x;
        constants.diffuse[0] = constants.diffuse[3] = 1;
        auto transform = gfx::IdentityMeshTransform();
        transform.model[12] = x;
        gfx::SetInterpKey(1);
        gfx::DrawMeshImmediate(vertices, kTriangle, constants, {}, {}, &transform);
        gfx::SetRenderTarget(gfx::kMainTarget);
        auto quad = Quad(box.x, box.y, box.w, box.h, {128, 128, 128, 128});
        auto display_quad = Quad(0, 0, 64, 48, {128, 128, 128, 128});
        for (auto *q : {&quad, &display_quad}) {
            for (auto &v : *q) {
                v.u = v.x;
                v.v = v.y;
            }
        }
        gfx::DrawState state;
        state.blend = true;
        gfx::Presentation2D presentation{display_quad, state};
        presentation.state.alpha_test = true;
        presentation.state.alpha_func = gfx::AlphaFunc::Greater;
        gfx::TextureBinding binding;
        binding.texture = target;
        binding.filter = gfx::Filter::Nearest;
        gfx::Draw2D(gfx::Primitive::Quads, quad, binding, state,
                    replacement ? &presentation : nullptr);
        return gfx::EndRecording();
    };
    auto read = [&](const gfx::DisplayList &list, float alpha, gfx::RenderOptions options) {
        options.present = !options.canonical;
        EXPECT_TRUE(gfx::RenderList(list, alpha, options));
        EXPECT_TRUE(gfx::ReadbackFrame(fixture.pixels, fixture.width, fixture.height));
        return fixture.pixels;
    };
    auto before = record(-0.375f, true);
    read(*before, 1, {.canonical = true});
    auto       after = record(0.375f, true);
    const auto canonical = read(*after, 1, {.canonical = true});
    auto       unmodified = record(0.375f, false);
    EXPECT_EQ(read(*unmodified, 1, {.canonical = true}), canonical);
    for (int phase = 0; phase < 8; ++phase) {
        const float alpha = phase / 8.0f;
        const auto  actual = read(*after, alpha, {.previous = before.get()});
        int         red = 0, green = 0;
        double      x_sum = 0;
        for (uint32_t y = 0; y < fixture.height; ++y) {
            for (uint32_t x = 0; x < fixture.width; ++x) {
                const auto pixel = fixture.Pixel(x, y);
                if (pixel[0] > 100) {
                    ++red;
                    x_sum += x;
                }
                if (pixel[1] > 100) {
                    ++green;
                }
            }
        }
        EXPECT_GT(red, 45) << phase;
        EXPECT_EQ(green, 0) << phase;
        if (red > 0) {
            EXPECT_NEAR(x_sum / red, 19.5 + 24 * alpha, 0.6) << phase;
        }
        EXPECT_EQ(read(*after, 1, {.canonical = true}), canonical) << phase;
    }
}

TEST(GfxGpuDisplayList, PresentationCompositeDoesNotWriteTransparentDepth) {
    GfxFixture fixture(64, 48, 1.0f, true, 64, 48);
    const auto texture = gfx::CreateTexture({2, 1});
    ASSERT_NE(texture, gfx::kNullTexture);
    const uint8_t pixels[] = {255, 0, 0, 128, 0, 0, 0, 0};
    ASSERT_TRUE(gfx::UpdateTexture(texture, 0, 0, 0, 2, 1, pixels));
    gfx::BeginRecording();
    gfx::Clear(true, kClear.data(), true, 0);
    auto quad = Quad(0, 0, 16, 48, {128, 128, 128, 128}, 0, 0, 0, 0, 0.8f);
    auto display_quad = Quad(0, 0, 64, 48, {128, 128, 128, 128}, 0, 0, 0, 0, 0.8f);
    for (auto *q : {&quad, &display_quad}) {
        (*q)[0].u = (*q)[3].u = 0;
        (*q)[1].u = (*q)[2].u = 2;
        (*q)[0].v = (*q)[1].v = 0;
        (*q)[2].v = (*q)[3].v = 1;
    }
    gfx::DrawState state;
    state.blend = true;
    state.depth_write = true;
    gfx::Presentation2D presentation{display_quad, state};
    presentation.state.alpha_test = true;
    presentation.state.alpha_func = gfx::AlphaFunc::Greater;
    gfx::TextureBinding binding;
    binding.texture = texture;
    binding.filter = gfx::Filter::Nearest;
    gfx::Draw2D(gfx::Primitive::Quads, quad, binding, state, &presentation);
    gfx::DrawState behind;
    behind.depth_test = gfx::DepthTest::Greater;
    gfx::Draw2D(gfx::Primitive::Quads, Quad(0, 0, 64, 48, {0, 255, 0, 128}, 0, 0, 0, 0, 0.5f), {}, behind);
    const auto list = gfx::EndRecording();
    ASSERT_TRUE(gfx::RenderList(*list, 1, {.canonical = true}));
    ASSERT_TRUE(gfx::ReadbackFrame(fixture.pixels, fixture.width, fixture.height));
    const auto canonical = fixture.pixels;
    ASSERT_TRUE(gfx::RenderList(*list, 0.5f, {.present = true}));
    ASSERT_TRUE(gfx::ReadbackFrame(fixture.pixels, fixture.width, fixture.height));
    EXPECT_GT(fixture.Pixel(24, 24)[0], 100);
    EXPECT_EQ(fixture.Pixel(24, 24)[1], 0);
    EXPECT_GT(fixture.Pixel(48, 24)[1], 200);
    EXPECT_EQ(fixture.Pixel(48, 24)[0], 0);
    ASSERT_TRUE(gfx::RenderList(*list, 1, {.canonical = true}));
    ASSERT_TRUE(gfx::ReadbackFrame(fixture.pixels, fixture.width, fixture.height));
    EXPECT_EQ(fixture.pixels, canonical);
    gfx::DestroyTexture(texture);
}

TEST(GfxGpuMeshDc2, PresentationReplacementDoesNotInheritUiAnchor) {
    for (const auto [width, height] : {
             std::pair{128, 72 },
             std::pair{72,  128}
    }) {
        GfxFixture fixture(width, height);
        gfx::SetFrameLayout({gfx::AspectMode::Fill, 1.0f});
        const auto extent = gfx::VisibleLogicalRect(gfx::kMainTarget);
        const auto replacement = Quad(extent.x, extent.y, extent.w, extent.h, {128, 0, 0, 128});
        auto       record = [&](bool anchored, bool substitute) {
            gfx::BeginRecording();
            gfx::Clear(true, kClear.data(), true, 0);
            gfx::UiAnchorScope        anchor(anchored ? gfx::UiAnchor::Side(1, -1, true) : gfx::UiAnchor{});
            const auto                original = Quad(560, 100, 40, 100, {128, 0, 0, 128});
            gfx::DrawState            state;
            const gfx::Presentation2D presentation{replacement, state};
            gfx::Draw2D(gfx::Primitive::Quads, substitute ? original : replacement, {}, state,
                        substitute ? &presentation : nullptr);
            return gfx::EndRecording();
        };
        auto read = [&](const gfx::DisplayList &list, bool canonical) {
            EXPECT_TRUE(gfx::RenderList(list, 0.5f, {.canonical = canonical, .present = !canonical}));
            EXPECT_TRUE(gfx::ReadbackFrame(fixture.pixels, fixture.width, fixture.height));
            return fixture.pixels;
        };
        auto        anchored = record(true, true);
        const auto &draw = std::get<gfx::detail::Draw2DEntry>(anchored->entries.back());
        ASSERT_EQ(draw.side.x, 1);
        ASSERT_EQ(draw.side.y, -1);
        const auto canonical = read(*anchored, true);
        auto       reference = record(false, false);
        const auto expected = read(*reference, true);
        EXPECT_NE(canonical, expected);
        EXPECT_EQ(read(*anchored, false), expected) << width << 'x' << height;
        EXPECT_EQ(read(*anchored, true), canonical);
    }
}

TEST(GfxGpuDisplayList, HostReplacementSuppressesOnlyTheTaggedMainDisplayGroup) {
    GfxOptions settings;
    settings.offscreen = true;
    settings.logical_width = 64;
    settings.logical_height = 48;
    GfxFixture fixture(64, 48, 1, settings);
    const auto target = gfx::CreateRenderTarget(64, 48, true, true);
    ASSERT_NE(target, gfx::kNullTexture);
    gfx::BeginRecording();
    gfx::SetRenderTarget(target);
    gfx::Clear(true, kClear.data(), true, 0);
    gfx::SetInterp2D({77});
    gfx::Draw2D(gfx::Primitive::Quads, Quad(0, 0, 64, 48, {255, 0, 0, 255}), {}, {});
    gfx::SetRenderTarget(gfx::kMainTarget);
    gfx::Clear(true, kClear.data(), true, 0);
    gfx::SetInterp2D({77});
    gfx::Draw2D(gfx::Primitive::Quads, Quad(0, 0, 32, 48, {255, 0, 0, 255}), {}, {});
    gfx::SetInterp2D({78});
    gfx::Draw2D(gfx::Primitive::Quads, Quad(32, 0, 32, 48, {0, 0, 255, 255}), {}, {});
    const auto scene = gfx::EndRecording();
    ASSERT_TRUE(gfx::RenderList(*scene, 1, {.canonical = true, .suppressed_2d_key = 77}));
    std::vector<uint8_t> canonical, display;
    uint32_t             w = 0, h = 0;
    ASSERT_TRUE(gfx::ReadbackFrame(canonical, w, h));
    EXPECT_EQ(canonical[(32 * w + 8) * 4], 255);
    EXPECT_EQ(canonical[(32 * w + 8) * 4 + 2], 0);
    EXPECT_EQ(canonical[(32 * w + 48) * 4 + 2], 255);
    gfx::BeginRecording();
    gfx::SetInterp2D({77});
    gfx::Draw2D(gfx::Primitive::Quads, Quad(0, 0, 16, 16, {0, 255, 0, 255}), {}, {});
    const auto overlay = gfx::EndRecording();
    ASSERT_TRUE(gfx::RenderList(*scene, 1, {.present = true, .overlay = overlay.get(), .suppressed_2d_key = 77}));
    ASSERT_TRUE(gfx::ReadbackFrame(display, w, h));
    const auto pixel = [&](int x, int y, int c) { return display[(y * w + x) * 4 + c]; };
    EXPECT_EQ(pixel(8, 8, 1), 255);   // host overlay is not suppressed by its identical key
    EXPECT_EQ(pixel(8, 32, 0), 0);    // original hand is hidden
    EXPECT_EQ(pixel(48, 32, 2), 255); // unclaimed selection highlight is retained
    std::vector<uint8_t> offscreen;
    uint32_t             tw = 0, th = 0;
    ASSERT_TRUE(gfx::ReadbackTexture(target, offscreen, tw, th));
    EXPECT_EQ(offscreen[(32 * tw + 8) * 4], 255);
    EXPECT_EQ(offscreen[(32 * tw + 8) * 4 + 2], 0);
    ASSERT_TRUE(gfx::PresentCanonical());
    ASSERT_TRUE(gfx::ReadbackFrame(display, w, h));
    EXPECT_EQ(display, canonical);
    gfx::DestroyTexture(target);
}

TEST(GfxGpuDisplayList, FilteredOutlineCopiesDoNotSamplePreviousPartOutsideBounds) {
    GfxOptions options;
    options.offscreen = true;
    options.logical_width = 64;
    options.logical_height = 48;
    GfxFixture fixture(64, 48, 1, options);
    const auto target = gfx::CreateRenderTarget(64, 48, true, true);
    ASSERT_NE(target, gfx::kNullTexture);
    const uint8_t          opaque[4] = {128, 128, 128, 128};
    const uint8_t          transparent[4] = {0, 0, 0, 0};
    const gfx::LogicalRect full{0, 0, 64, 48};
    const gfx::LogicalRect crop{16, 8, 16, 24};
    gfx::BeginRecording();
    gfx::Clear(true, opaque, true, 0);
    gfx::SetRenderTarget(target);
    // An earlier part left opaque texels next to this part's cleared rectangle.
    gfx::Clear(true, opaque, false, 0);
    gfx::Clear(true, transparent, false, 0, &crop, &full);
    gfx::SetRenderTarget(gfx::kMainTarget);
    auto quad = Quad(crop.x + .25f, crop.y, crop.w, crop.h, {0, 0, 0, 128});
    auto display_quad = Quad(.25f, 0, 64, 48, {0, 0, 0, 128});
    for (auto *vertices : {&quad, &display_quad}) {
        for (auto &vertex : *vertices) {
            vertex.u = vertex.x - .25f;
            vertex.v = vertex.y;
        }
    }
    gfx::TextureBinding binding;
    binding.texture = target;
    binding.filter = gfx::Filter::Linear;
    gfx::DrawState state;
    state.blend = true;
    const gfx::Presentation2D presentation{display_quad, state};
    gfx::Draw2D(gfx::Primitive::Quads, quad, binding, state, &presentation);
    const auto list = gfx::EndRecording();
    ASSERT_TRUE(gfx::RenderList(*list, 1, {.canonical = true}));
    ASSERT_TRUE(gfx::ReadbackFrame(fixture.pixels, fixture.width, fixture.height));
    const auto canonical = fixture.pixels;
    for (int phase = 0; phase < 8; ++phase) {
        ASSERT_TRUE(gfx::RenderList(*list, phase / 8.0f, {.previous = list.get(), .present = true}));
        ASSERT_TRUE(gfx::ReadbackFrame(fixture.pixels, fixture.width, fixture.height));
        for (uint32_t y = 0; y < fixture.height; ++y) {
            for (uint32_t x = 0; x < fixture.width; ++x) {
                EXPECT_TRUE(fixture.PixelNear(x, y, 128, 128, 128)) << x << "," << y << " phase " << phase;
            }
        }
    }
    ASSERT_TRUE(gfx::RenderList(*list, 1, {.canonical = true}));
    ASSERT_TRUE(gfx::ReadbackFrame(fixture.pixels, fixture.width, fixture.height));
    EXPECT_EQ(fixture.pixels, canonical);
    gfx::DestroyTexture(target);
}

TEST(GfxGpuMeshDc2, RotatingThinOutlineKeepsIntermediateTips) {
    GfxOptions options;
    options.offscreen = true;
    options.prog0_meshes = true;
    options.logical_width = 640;
    options.logical_height = 480;
    GfxFixture fixture(640, 480, 1.0f, options);
    const auto target = gfx::CreateRenderTarget(640, 480, true, true);
    ASSERT_NE(target, gfx::kNullTexture);
    const gfx::LogicalRect           full{0, 0, 640, 480};
    const uint8_t                    clear[4] = {0, 0, 0, 0};
    const std::vector<gfx::Vertex3D> triangle = {
        {{-.3125f, -1.0f / 240, .5f}}, {{.3125f, -1.0f / 240, .5f}}, {{.3125f, 1.0f / 240, .5f}}, {{-.3125f, 1.0f / 240, .5f}}};
    const std::vector<uint32_t> indices = {0, 1, 2, 0, 2, 3};
    auto                        record = [&](float camera, bool tagged) {
        auto transform = gfx::IdentityMeshTransform();
        transform.model[0] = transform.model[5] = std::cos(camera);
        transform.model[1] = std::sin(camera);
        transform.model[4] = -std::sin(camera);
        gfx::MeshConstants constants{};
        std::memcpy(constants.mvp, transform.model, sizeof constants.mvp);
        gfx::Prog0Constants block{};
        block.flags = gfx::kProg0Unlit;
        for (int i = 0; i < 4; ++i) {
            block.object_color[i] = i == 3 ? 128 : 200;
            block.diffuse[i] = 1;
        }
        const float            w = 100 * std::abs(std::cos(camera)) + (640.0f / 480) * std::abs(std::sin(camera));
        const float            h = 75 * std::abs(std::sin(camera)) + std::abs(std::cos(camera));
        const gfx::LogicalRect crop{320 - w, 240 - h, 2 * w, 2 * h};
        gfx::BeginRecording();
        gfx::Clear(true, clear, true, 0);
        gfx::SetRenderTarget(target);
        gfx::Clear(true, clear, false, 0, &crop, tagged ? &full : nullptr);
        gfx::SetInterpKey(17);
        gfx::DrawMeshImmediateProg0(triangle, indices, constants, block, {}, {}, &transform);
        gfx::SetRenderTarget(gfx::kMainTarget);
        std::vector<gfx::Vertex2D> quad(4);
        for (int i = 0; i < 4; ++i) {
            auto &v = quad[i];
            v.x = v.u = crop.x + ((i == 1 || i == 2) ? crop.w : 0);
            v.y = v.v = crop.y + (i >= 2 ? crop.h : 0);
            std::memset(v.color, 128, 4);
        }
        gfx::TextureBinding binding;
        binding.texture = target;
        gfx::DrawState state;
        auto           display_quad = quad;
        for (int i = 0; i < 4; ++i) {
            auto &v = display_quad[i];
            v.x = v.u = (i == 1 || i == 2) ? full.w : 0;
            v.y = v.v = i >= 2 ? full.h : 0;
        }
        const gfx::Presentation2D presentation{display_quad, state};
        gfx::Draw2D(gfx::Primitive::Quads, quad, binding, state,
                    tagged ? &presentation : nullptr);
        return gfx::EndRecording();
    };
    auto render = [&](const gfx::DisplayList &list, float alpha, gfx::RenderOptions options) {
        options.present = !options.canonical;
        EXPECT_TRUE(gfx::RenderList(list, alpha, options));
        EXPECT_TRUE(gfx::ReadbackFrame(fixture.pixels, fixture.width, fixture.height));
        return fixture.pixels;
    };
    const float angle = std::acos(-1.0f) / 4;
    auto        before = record(-angle, true);
    auto        after = record(angle, true);
    auto        expected = record(0, false);
    const auto  reference = render(*expected, 1, {.canonical = true});
    render(*after, 1, {.canonical = true});
    EXPECT_EQ(render(*after, .5f, {.previous = before.get()}), reference);
    gfx::DestroyTexture(target);
}

TEST(GfxPresentationBounds, UsesActualMidpointRotationBeyondEndpointUnion) {
    const gfx::LogicalMapping mapping{.scale_x = 1, .scale_y = 1, .offset_x = 0, .offset_y = 0, .pixel_width = 64, .pixel_height = 48};
    gfx::detail::Mat4         before = {};
    gfx::detail::Mat4         after = {};
    for (size_t i = 0; i < 4; ++i) {
        before[i * 5] = after[i * 5] = 1;
    }
    const float diagonal = std::sqrt(0.5f);
    before[0] = before[5] = after[0] = after[5] = diagonal;
    before[1] = before[4] = diagonal;
    before[4] = -diagonal;
    after[1] = -diagonal;
    after[4] = diagonal;
    gfx::detail::Mat4 midpoint;
    ASSERT_TRUE(gfx::detail::InterpolateAffine(before, after, 0.5f, midpoint));
    const std::array<float, 3> bounds_min{-0.75f, -0.05f, 0.5f};
    const std::array<float, 3> bounds_max{0.75f, 0.05f, 0.5f};
    auto                       project = [&](const gfx::detail::Mat4 &matrix) {
        gfx::MeshConstants constants{};
        std::memcpy(constants.mvp, matrix.data(), sizeof constants.mvp);
        return gfx::detail::ProjectPresentationBounds(bounds_min, bounds_max, constants, mapping);
    };
    const auto first = project(before);
    const auto last = project(after);
    const auto middle = project(midpoint);
    ASSERT_TRUE(first);
    ASSERT_TRUE(last);
    ASSERT_TRUE(middle);
    EXPECT_LT(middle->x, std::min(first->x, last->x));
    EXPECT_GT(middle->x + middle->w, std::max(first->x + first->w, last->x + last->w));
}

TEST(GfxPresentationBounds, RejectsEyeCrossingsAndNonfiniteGeometry) {
    const gfx::LogicalMapping mapping{.scale_x = 1, .scale_y = 1, .offset_x = 0, .offset_y = 0, .pixel_width = 64, .pixel_height = 48};
    gfx::MeshConstants        constants{};
    Identity(constants.mvp);
    constants.mvp[3] = 1;
    constants.mvp[15] = 0.2f;
    EXPECT_FALSE(gfx::detail::ProjectPresentationBounds({-0.5f, -0.5f, 0.5f}, {0.5f, 0.5f, 0.5f}, constants, mapping));
    Identity(constants.mvp);
    EXPECT_FALSE(gfx::detail::ProjectPresentationBounds({-INFINITY, 0, 0}, {1, 1, 1}, constants, mapping));
    EXPECT_TRUE(gfx::detail::ProjectPresentationBounds({-0.5f, -0.5f, 0.5f}, {0.5f, 0.5f, 0.5f}, constants, mapping));
}

namespace {

/**
 * Compares bounded rigid and skinned presentation with complete RGBA and alpha-consumer references.
 */
void PresentationRegionComparisons(float render_scale) {
    /**
     * Selects a rigid stream, an animated skin or the conservative missing-palette fallback.
     */
    enum class SourceMode {
        Rigid,
        Skinned,
        MissingPalette
    };

    /**
     * Pixels and submitted work for one reference or bounded presentation.
     */
    struct Sample {
        std::vector<uint8_t> pixels;          /**< Complete output RGBA image. */
        uint32_t             draws;           /**< Submitted 2D draw count, including frame flanks. */
        uint32_t             bounded_regions; /**< Groups using a proven smaller display rectangle. */
    };

    GfxOptions options;
    options.offscreen = true;
    options.prog0_meshes = true;
    options.logical_width = 64;
    options.logical_height = 48;
    GfxFixture fixture(96, 48, render_scale, options);
    if (!gfx::ActiveRendererFeatures().dynamic_color_write_mask) {
        GTEST_SKIP() << "Partial colour masks unavailable";
    }
    const auto scratch = gfx::CreateRenderTarget(64, 48, true, true);
    ASSERT_NE(scratch, gfx::kNullTexture);
    const std::vector<gfx::Vertex3D> vertices = {
        {{-.75f, -.05f, .5f}}, {{.75f, -.05f, .5f}}, {{0, .05f, .5f}}};
    const auto rigid = gfx::CreateMesh(vertices, kTriangle);
    const auto skinned_vertices = Skinned(vertices, {.3f, .7f, 0, 0}, {0, 1, 0, 0});
    const auto skinned = gfx::CreateMesh(skinned_vertices, kTriangle);
    ASSERT_NE(rigid, gfx::kNullMesh);
    ASSERT_NE(skinned, gfx::kNullMesh);
    for (const auto aspect : {gfx::AspectMode::Letterbox, gfx::AspectMode::Fill}) {
        for (const float ui_scale : {1.0f, 1.3f}) {
            gfx::SetFrameLayout({.aspect = aspect, .ui_scale = ui_scale});
            for (const auto source : {SourceMode::Rigid, SourceMode::Skinned, SourceMode::MissingPalette}) {
                const bool use_skin = source != SourceMode::Rigid;
                const bool missing_palette = source == SourceMode::MissingPalette;
                for (const bool consume_alpha : {false, true}) {
                    auto record = [&](float angle, bool bounded) {
                        auto        transform = gfx::IdentityMeshTransform();
                        const float cosine = std::cos(angle);
                        const float sine = std::sin(angle);
                        if (!use_skin) {
                            transform.model[0] = transform.model[5] = cosine;
                            transform.model[1] = sine;
                            transform.model[4] = -sine;
                        }
                        gfx::MeshConstants constants{};
                        std::memcpy(constants.mvp, transform.model, sizeof constants.mvp);
                        constants.diffuse[0] = constants.diffuse[3] = 1;
                        gfx::Prog0Constants prog0{};
                        prog0.flags = gfx::kProg0Unlit | gfx::kProg0Skinned;
                        for (size_t i = 0; i < 4; ++i) {
                            prog0.object_color[i] = 128;
                            prog0.diffuse[i] = 1;
                        }
                        gfx::Prog0Palette palette{};
                        for (uint32_t bone = 0; bone < gfx::kProg0Bones; ++bone) {
                            SetBone(palette, bone, kIdentityRows);
                        }
                        if (use_skin) {
                            const Rows rotation = {cosine, -sine, 0, 0, sine, cosine, 0, 0, 0, 0, 1, 0};
                            SetBone(palette, 0, rotation);
                            SetBone(palette, 1, rotation);
                        }
                        gfx::BeginRecording();
                        const uint8_t background[4] = {19, 37, 53, 93};
                        gfx::Clear(true, background, true, 0);
                        const auto full = gfx::VisibleLogicalRect(gfx::kMainTarget);
                        gfx::SetRenderTarget(scratch);
                        const uint8_t poison[4] = {117, 83, 41, 128};
                        gfx::Clear(true, poison, false, 0);
                        gfx::SetRenderTarget(gfx::kMainTarget);
                        if (bounded) {
                            gfx::BeginPresentationRegion(scratch, full, 1.0f);
                        }
                        gfx::SetRenderTarget(scratch);
                        const uint8_t empty[4] = {};
                        gfx::Clear(true, empty, false, 0, &full, &full);
                        gfx::SetInterpKey(17);
                        if (use_skin) {
                            gfx::DrawMeshProg0(skinned, 0, 3, constants, prog0, {}, {}, &transform, missing_palette ? nullptr : &palette);
                        } else {
                            gfx::DrawMesh(rigid, 0, 3, constants, {}, {}, &transform);
                        }
                        gfx::SetRenderTarget(gfx::kMainTarget);
                        const gfx::TextureBinding binding{.texture = scratch, .filter = gfx::Filter::Linear};
                        gfx::DrawState            body;
                        body.blend = true;
                        auto edge = body;
                        edge.color_write_mask = gfx::kWriteRed | gfx::kWriteGreen | gfx::kWriteBlue;
                        const auto copy = [&](float dx, float dy, const gfx::DrawState &state) {
                            const auto                quad = Quad(full.x + dx, full.y + dy, full.w, full.h, {128, 128, 128, 128},
                                                                  full.x, full.y, full.x + full.w, full.y + full.h);
                            const gfx::Presentation2D presentation{.vertices = quad, .state = state, .bounded_region = bounded};
                            gfx::Draw2D(gfx::Primitive::Quads, quad, binding, state, &presentation);
                        };
                        copy(.75f, 0, edge);
                        copy(-.75f, 0, edge);
                        copy(0, .75f, edge);
                        copy(0, -.75f, edge);
                        if (bounded) {
                            auto reset = body;
                            reset.blend = false;
                            reset.color_write_mask = gfx::kWriteAlpha;
                            const auto                quad = Quad(full.x, full.y, full.w, full.h, {0, 0, 0, 0});
                            const gfx::Presentation2D presentation{.vertices = quad, .state = reset, .presentation_only = true, .region_alpha_reset = true};
                            gfx::Draw2D(gfx::Primitive::Quads, quad, {}, reset, &presentation);
                        }
                        copy(0, 0, body);
                        if (bounded) {
                            gfx::EndPresentationRegion(true);
                        }
                        if (consume_alpha) {
                            auto consumer = body;
                            consumer.alpha.c = 1;
                            const auto quad = Quad(full.x, full.y, full.w, full.h, {128, 19, 83, 128});
                            gfx::Draw2D(gfx::Primitive::Quads, quad, {}, consumer);
                        }
                        return gfx::EndRecording();
                    };
                    auto render = [&](const gfx::DisplayList &list, const gfx::DisplayList *previous, bool canonical) {
                        EXPECT_TRUE(gfx::RenderList(list, .5f, {.canonical = canonical, .previous = previous, .present = !canonical}));
                        const auto draws = gfx::detail::g.counters.draws_2d;
                        const auto bounded_regions = gfx::detail::g.counters.bounded_presentation_regions;
                        EXPECT_TRUE(gfx::ReadbackFrame(fixture.pixels, fixture.width, fixture.height));
                        return Sample{.pixels = fixture.pixels, .draws = draws, .bounded_regions = bounded_regions};
                    };
                    const float before_angle = use_skin ? 0.0f : -.785398163f;
                    const float after_angle = use_skin ? 3.1415925f : .785398163f;
                    const auto  before = record(before_angle, false);
                    const auto  reference = record(after_angle, false);
                    const auto  optimized = record(after_angle, true);
                    const auto  canonical_reference = render(*reference, nullptr, true);
                    const auto  canonical_optimized = render(*optimized, nullptr, true);
                    EXPECT_EQ(canonical_optimized.pixels, canonical_reference.pixels);
                    const auto display_reference = render(*reference, before.get(), false);
                    const auto display_optimized = render(*optimized, before.get(), false);
                    EXPECT_EQ(display_optimized.pixels, display_reference.pixels)
                        << static_cast<int>(aspect) << " ui " << ui_scale << " source " << static_cast<int>(source) << " consume " << consume_alpha;
                    EXPECT_EQ(display_optimized.bounded_regions, missing_palette ? 0u : 1u);
                    EXPECT_EQ(display_reference.bounded_regions, 0u);
                    EXPECT_LE(display_optimized.draws, display_reference.draws + (missing_palette ? 0u : 1u));
                }
            }
        }
    }
    gfx::DestroyMesh(rigid);
    gfx::DestroyMesh(skinned);
    gfx::DestroyTexture(scratch);
}

} // namespace

TEST(GfxGpuMeshDc2, PresentationRegionsPreserveRgbaAndDestinationAlpha) {
    PresentationRegionComparisons(1.0f);
}

TEST(GfxGpuMeshDc2, LowResolutionPresentationRegionsPreserveFilteredRgba) {
    PresentationRegionComparisons(0.5f);
}

TEST(GfxGpuMeshDc2, ConsecutivePresentationBodiesPreserveRgbaAndAlphaReaders) {
    /**
     * Selects writes or signature changes between consecutive outlined bodies.
     */
    enum class Interference {
        None,
        RgbAlphaReader,
        MainAlphaWrite,
        ScratchWrite,
        ChangedExtent,
        UnknownMiddleBody,
        ChangedDestination,
        ChangedSourceMapping,
        PartialScissor
    };

    /**
     * Retains both destination images and the exercised bounded-region count.
     */
    struct Sample {
        std::vector<uint8_t> main;                /**< Complete main-target RGBA image. */
        std::vector<uint8_t> alternate;           /**< Complete alternate-target RGBA image. */
        uint32_t             bounded_regions = 0; /**< Regions taking the bounded presentation path. */
    };

    GfxOptions options;
    options.offscreen = true;
    options.logical_width = 64;
    options.logical_height = 48;
    GfxFixture fixture(96, 48, .75f, options);
    if (!gfx::ActiveRendererFeatures().dynamic_color_write_mask) {
        GTEST_SKIP() << "Partial colour masks unavailable";
    }
    const auto scratch = gfx::CreateRenderTarget(64, 48, true, true);
    const auto differently_mapped_scratch = gfx::CreateRenderTarget(64, 48, true);
    const auto alternate = gfx::CreateRenderTarget(80, 48, true);
    ASSERT_NE(scratch, gfx::kNullTexture);
    ASSERT_NE(differently_mapped_scratch, gfx::kNullTexture);
    ASSERT_NE(alternate, gfx::kNullTexture);
    const auto main_mapping = gfx::GetLogicalMapping(scratch);
    const auto other_mapping = gfx::GetLogicalMapping(differently_mapped_scratch);
    ASSERT_TRUE(main_mapping.scale_x != other_mapping.scale_x || main_mapping.scale_y != other_mapping.scale_y ||
                main_mapping.offset_x != other_mapping.offset_x || main_mapping.offset_y != other_mapping.offset_y ||
                main_mapping.pixel_width != other_mapping.pixel_width || main_mapping.pixel_height != other_mapping.pixel_height);
    const auto pixel_differences = [&](const std::vector<uint8_t> &actual, const std::vector<uint8_t> &expected) {
        const size_t          width = std::max<uint32_t>(fixture.width, 1u);
        size_t                differing_pixels = 0;
        std::array<size_t, 4> channels = {};
        std::string           first;
        for (size_t i = 0; i + 3 < std::min(actual.size(), expected.size()); i += 4) {
            bool different = false;
            for (size_t channel = 0; channel < channels.size(); ++channel) {
                if (actual[i + channel] != expected[i + channel]) {
                    ++channels[channel];
                    different = true;
                }
            }
            if (!different) {
                continue;
            }
            if (differing_pixels < 8) {
                first += std::format(" ({},{}) actual={}/{}/{}/{} expected={}/{}/{}/{}",
                                     (i / 4) % width, (i / 4) / width,
                                     actual[i], actual[i + 1], actual[i + 2], actual[i + 3],
                                     expected[i], expected[i + 1], expected[i + 2], expected[i + 3]);
            }
            ++differing_pixels;
        }
        return std::format("sizes={}/{} differing_pixels={} rgba_channels={}/{}/{}/{} shared_mapping={}/{}/{}/{}/{}/{} independent_mapping={}/{}/{}/{}/{}/{} first:{}",
                           actual.size(), expected.size(), differing_pixels, channels[0], channels[1], channels[2], channels[3],
                           main_mapping.scale_x, main_mapping.scale_y, main_mapping.offset_x, main_mapping.offset_y,
                           main_mapping.pixel_width, main_mapping.pixel_height,
                           other_mapping.scale_x, other_mapping.scale_y, other_mapping.offset_x, other_mapping.offset_y,
                           other_mapping.pixel_width, other_mapping.pixel_height, first);
    };
    const std::vector<gfx::Vertex3D> vertices = {
        {{-.08f, -.06f, .5f}}, {{.08f, -.06f, .5f}}, {{0, .08f, .5f}}};
    const auto mesh = gfx::CreateMesh(vertices, kTriangle);
    ASSERT_NE(mesh, gfx::kNullMesh);
    constexpr std::array cases = {
        Interference::None, Interference::RgbAlphaReader, Interference::MainAlphaWrite, Interference::ScratchWrite,
        Interference::ChangedExtent, Interference::UnknownMiddleBody, Interference::ChangedDestination,
        Interference::ChangedSourceMapping, Interference::PartialScissor};
    for (const auto interference : cases) {
        for (const bool consume_alpha : {false, true}) {
            SCOPED_TRACE(std::format("interference {} consume alpha {}", static_cast<int>(interference), consume_alpha));
            auto record = [&](float pose, bool bounded) {
                gfx::BeginRecording();
                const uint8_t background[4] = {19, 37, 53, 93};
                gfx::Clear(true, background, true, 0);
                const auto full = gfx::VisibleLogicalRect(gfx::kMainTarget);
                gfx::SetRenderTarget(alternate);
                const uint8_t other_background[4] = {41, 71, 23, 191};
                gfx::Clear(true, other_background, true, 0);
                const uint8_t poison[4] = {117, 83, 41, 127};
                for (const auto source : {scratch, differently_mapped_scratch}) {
                    gfx::SetRenderTarget(source);
                    gfx::Clear(true, poison, false, 0);
                }
                gfx::SetRenderTarget(gfx::kMainTarget);
                const auto alpha_reader = [&](gfx::TextureHandle target) {
                    gfx::SetRenderTarget(target);
                    const auto     extent = gfx::VisibleLogicalRect(target);
                    gfx::DrawState state;
                    state.blend = true;
                    state.alpha.c = 1;
                    state.color_write_mask = gfx::kWriteRed | gfx::kWriteGreen | gfx::kWriteBlue;
                    const auto quad = Quad(extent.x, extent.y, extent.w, extent.h, {83, 19, 109, 128});
                    gfx::Draw2D(gfx::Primitive::Quads, quad, {}, state);
                };
                const auto body = [&](size_t index) {
                    const bool middle = index == 1;
                    const auto destination = middle && interference == Interference::ChangedDestination ? alternate : gfx::kMainTarget;
                    const auto source = middle && interference == Interference::ChangedSourceMapping ? differently_mapped_scratch : scratch;
                    auto       extent = gfx::VisibleLogicalRect(destination);
                    if (middle && interference == Interference::ChangedExtent) {
                        extent.x += extent.w * .5f;
                        extent.w *= .5f;
                    }
                    gfx::SetRenderTarget(destination);
                    if (bounded) {
                        gfx::BeginPresentationRegion(source, extent, 1.0f);
                    }
                    gfx::SetRenderTarget(source);
                    const uint8_t empty[4] = {};
                    gfx::Clear(true, empty, false, 0, &extent, &extent);
                    constexpr std::array<std::array<float, 2>, 3> centers = {
                        {{-.55f, -.4f}, {.45f, .35f}, {.55f, -.35f}}
                    };
                    auto transform = gfx::IdentityMeshTransform();
                    transform.model[12] = centers[index][0] + .045f * pose;
                    transform.model[13] = centers[index][1] + .025f * pose;
                    gfx::MeshConstants constants{};
                    std::memcpy(constants.mvp, transform.model, sizeof constants.mvp);
                    constants.diffuse[0] = .75f;
                    constants.diffuse[1] = .375f;
                    constants.diffuse[2] = .625f;
                    constants.diffuse[3] = .75f;
                    gfx::SetInterpKey(100 + index);
                    if (middle && interference == Interference::UnknownMiddleBody) {
                        gfx::DrawMeshImmediate(vertices, kTriangle, constants, {}, {}, &transform);
                    } else {
                        gfx::DrawMesh(mesh, 0, 3, constants, {}, {}, &transform);
                    }
                    gfx::SetRenderTarget(destination);
                    const gfx::TextureBinding binding{.texture = source, .filter = gfx::Filter::Linear};
                    gfx::DrawState            state;
                    state.blend = true;
                    if (interference == Interference::PartialScissor) {
                        state.scissor = true;
                        state.extend_scissor = index == 2;
                        state.scissor_rect = index == 2 ? gfx::LogicalRect{0, 2, 64, 44} : gfx::LogicalRect{0, 4, 64, 40};
                    }
                    auto edge = state;
                    edge.color_write_mask = gfx::kWriteRed | gfx::kWriteGreen | gfx::kWriteBlue;
                    const auto copy = [&](float dx, float dy, const gfx::DrawState &draw_state) {
                        const auto                quad = Quad(extent.x + dx, extent.y + dy, extent.w, extent.h, {128, 128, 128, 128},
                                                              extent.x, extent.y, extent.x + extent.w, extent.y + extent.h);
                        const gfx::Presentation2D presentation{.vertices = quad, .state = draw_state, .bounded_region = bounded};
                        gfx::Draw2D(gfx::Primitive::Quads, quad, binding, draw_state, &presentation);
                    };
                    copy(.75f, 0, edge);
                    copy(-.75f, 0, edge);
                    copy(0, .75f, edge);
                    copy(0, -.75f, edge);
                    if (bounded) {
                        auto reset = state;
                        reset.blend = false;
                        reset.color_write_mask = gfx::kWriteAlpha;
                        const auto                quad = Quad(extent.x, extent.y, extent.w, extent.h, {0, 0, 0, 0});
                        const gfx::Presentation2D presentation{
                            .vertices = quad, .state = reset, .presentation_only = true, .region_alpha_reset = true};
                        gfx::Draw2D(gfx::Primitive::Quads, quad, {}, reset, &presentation);
                    }
                    copy(0, 0, state);
                    if (bounded) {
                        gfx::EndPresentationRegion(true);
                    }
                };
                body(0);
                if (interference == Interference::RgbAlphaReader || interference == Interference::PartialScissor) {
                    alpha_reader(gfx::kMainTarget);
                } else if (interference == Interference::MainAlphaWrite) {
                    gfx::DrawState state;
                    state.color_write_mask = gfx::kWriteAlpha;
                    const auto quad = Quad(full.x + 5.25f, full.y + 2.25f, 3.5f, 3.5f, {0, 0, 0, 127});
                    gfx::Draw2D(gfx::Primitive::Quads, quad, {}, state);
                } else if (interference == Interference::ScratchWrite) {
                    gfx::SetRenderTarget(scratch);
                    gfx::Clear(true, poison, false, 0);
                    gfx::SetRenderTarget(gfx::kMainTarget);
                }
                body(1);
                if (interference == Interference::PartialScissor) {
                    alpha_reader(gfx::kMainTarget);
                }
                if (interference == Interference::UnknownMiddleBody || interference == Interference::ChangedDestination ||
                    interference == Interference::ChangedSourceMapping || interference == Interference::PartialScissor) {
                    body(2);
                }
                if (consume_alpha) {
                    alpha_reader(alternate);
                    alpha_reader(gfx::kMainTarget);
                }
                gfx::SetRenderTarget(gfx::kMainTarget);
                return gfx::EndRecording();
            };
            auto render = [&](const gfx::DisplayList &list, const gfx::DisplayList *previous, bool canonical) {
                EXPECT_TRUE(gfx::RenderList(list, .5f, {.canonical = canonical, .previous = previous, .present = !canonical}));
                Sample result;
                result.bounded_regions = gfx::detail::g.counters.bounded_presentation_regions;
                EXPECT_TRUE(gfx::ReadbackFrame(result.main, fixture.width, fixture.height));
                uint32_t width = 0;
                uint32_t height = 0;
                EXPECT_TRUE(gfx::ReadbackTexture(alternate, result.alternate, width, height));
                return result;
            };
            const auto before = record(0, false);
            const auto reference = record(1, false);
            const auto optimized = record(1, true);
            const auto canonical_reference = render(*reference, nullptr, true);
            const auto canonical_optimized = render(*optimized, nullptr, true);
            EXPECT_EQ(canonical_optimized.main, canonical_reference.main);
            EXPECT_EQ(canonical_optimized.alternate, canonical_reference.alternate);
            const auto display_reference = render(*reference, before.get(), false);
            const auto display_optimized = render(*optimized, before.get(), false);
            EXPECT_EQ(display_optimized.main, display_reference.main)
                << pixel_differences(display_optimized.main, display_reference.main);
            EXPECT_EQ(display_optimized.alternate, display_reference.alternate);
            bool zero_alpha = false;
            bool nonzero_alpha = false;
            for (size_t i = 3; i < display_reference.main.size(); i += 4) {
                zero_alpha |= display_reference.main[i] == 0;
                nonzero_alpha |= display_reference.main[i] != 0;
            }
            EXPECT_TRUE(zero_alpha);
            EXPECT_TRUE(nonzero_alpha);
            EXPECT_EQ(display_reference.bounded_regions, 0u);
            EXPECT_GT(display_optimized.bounded_regions, 0u);
        }
    }
    gfx::DestroyMesh(mesh);
    gfx::DestroyTexture(scratch);
    gfx::DestroyTexture(differently_mapped_scratch);
    gfx::DestroyTexture(alternate);
}

TEST(GfxSkinBounds, PositiveWeightSumsScaleBoneUnionAboutOrigin) {
    const std::vector<gfx::Vertex3D> positions = {
        {{-5, -3, -2}}, {{-2, -1, -1}}, {{-4, -2, -1.5f}}};
    gfx::Prog0Palette palette{};
    for (uint32_t bone = 0; bone < gfx::kProg0Bones; ++bone) {
        SetBone(palette, bone, kIdentityRows);
    }
    SetBone(palette, 0, {1, 0, 0, -10, 0, 1, 0, -7, 0, 0, 1, -3});
    SetBone(palette, 1, {0, -1, 0, -20, 1, 0, 0, -11, 0, 0, 1, -5});
    const std::array<std::array<float, 4>, 6> weights{
        {{.3f, .7f, 0, 0}, {.4f, .6f, 0, 0}, {.25f, .25f, .25f, .25f}, {.5f, 0, 0, 0}, {1, .5f, 0, 0}, {0, 0, 0, 0}}
    };
    for (const auto &weight : weights) {
        gfx::detail::Mesh mesh;
        const auto        packed = Skinned(positions, weight, {0, 1, 0, 1});
        gfx::detail::SetMeshSkinBounds(mesh, packed, kTriangle);
        ASSERT_TRUE(mesh.skin_bounds_known);
        std::array<float, 3> lo, hi;
        ASSERT_TRUE(gfx::detail::SkinnedMeshPositionBounds(mesh, palette, lo, hi));
        for (const auto &vertex : positions) {
            for (size_t axis = 0; axis < 3; ++axis) {
                float actual = 0;
                for (size_t influence = 0; influence < 4; ++influence) {
                    const auto &row = palette.bones[influence % 2][axis];
                    float       term = row[3] * weight[influence];
                    for (size_t column = 0; column < 3; ++column) {
                        term += row[column] * (vertex.position[column] * weight[influence]);
                    }
                    actual += term;
                }
                EXPECT_LE(lo[axis], actual);
                EXPECT_GE(hi[axis], actual);
            }
        }
    }
}

TEST(GfxSkinBounds, ValidatesPackedWeightsAndAllReferencedBones) {
    const std::vector<gfx::Vertex3D> positions = {
        {{-1, 0, .5f}}, {{1, 0, .5f}}, {{0, 1, .5f}}};
    gfx::detail::Mesh                         mesh;
    const std::array<std::array<float, 4>, 4> invalid{
        {{-.1f, 1, 0, 0}, {1.1f, 0, 0, 0}, {INFINITY, 0, 0, 0}, {NAN, 0, 0, 0}}
    };
    for (const auto &weights : invalid) {
        gfx::detail::SetMeshSkinBounds(mesh, Skinned(positions, weights, {0, 1, 0, 0}), kTriangle);
        EXPECT_FALSE(mesh.skin_bounds_known);
    }
    gfx::detail::SetMeshSkinBounds(mesh, positions, kTriangle);
    EXPECT_FALSE(mesh.skin_bounds_known);
    gfx::detail::SetMeshSkinBounds(mesh, Skinned(positions, {1, 0, 0, 0}, {255, 1, 0, 0}), kTriangle);
    ASSERT_TRUE(mesh.skin_bounds_known);
    EXPECT_NE(mesh.skin_bones & (uint32_t{1} << 31), 0u);
    gfx::Prog0Palette palette{};
    for (uint32_t bone = 0; bone < gfx::kProg0Bones; ++bone) {
        SetBone(palette, bone, kIdentityRows);
    }
    palette.bones[31][0][3] = 300;
    std::array<float, 3> lo, hi;
    ASSERT_TRUE(gfx::detail::SkinnedMeshPositionBounds(mesh, palette, lo, hi));
    EXPECT_LE(lo[0], 299);
    EXPECT_GE(hi[0], 301);
    palette.bones[1][0][0] = INFINITY;
    EXPECT_FALSE(gfx::detail::SkinnedMeshPositionBounds(mesh, palette, lo, hi));
}

TEST(GfxSkinBounds, GuardsCancellationUnderflowAndEyeCrossings) {
    const std::vector<gfx::Vertex3D> positions(3, gfx::Vertex3D{
                                                      .position = {1000000, 0, .5f}
    });
    gfx::detail::Mesh                mesh;
    gfx::detail::SetMeshSkinBounds(mesh, Skinned(positions, {.5f, .5f, 0, 0}, {0, 0, 0, 0}), kTriangle);
    gfx::Prog0Palette palette{};
    for (uint32_t bone = 0; bone < gfx::kProg0Bones; ++bone) {
        SetBone(palette, bone, kIdentityRows);
    }
    palette.bones[0][0][3] = -1000000;
    std::array<float, 3> lo, hi;
    ASSERT_TRUE(gfx::detail::SkinnedMeshPositionBounds(mesh, palette, lo, hi));
    EXPECT_LT(lo[0], -1);
    EXPECT_GT(hi[0], 1);
    const float                      tiny = std::numeric_limits<float>::denorm_min();
    const std::vector<gfx::Vertex3D> subnormal(3, gfx::Vertex3D{
                                                      .position = {tiny, 0, 0}
    });
    gfx::detail::SetMeshSkinBounds(mesh, Skinned(subnormal, {.5f, 0, 0, 0}, {0, 0, 0, 0}), kTriangle);
    palette.bones[0][0][0] = 1.0e30f;
    palette.bones[0][0][3] = 0;
    ASSERT_TRUE(gfx::detail::SkinnedMeshPositionBounds(mesh, palette, lo, hi));
    EXPECT_LE(lo[0], 0);
    EXPECT_GT(hi[0], tiny * 1.0e30f);
    gfx::MeshConstants constants{};
    Identity(constants.mvp);
    constants.mvp[15] = tiny;
    const gfx::LogicalMapping mapping{.scale_x = 1, .scale_y = 1, .offset_x = 0, .offset_y = 0, .pixel_width = 64, .pixel_height = 48};
    EXPECT_FALSE(gfx::detail::ProjectPresentationBounds(lo, hi, constants, mapping));
    constants.mvp[15] = 1;
    constants.mvp[3] = 1;
    const std::vector<gfx::Vertex3D> crossing = {
        {{-2, 0, .5f}}, {{2, 0, .5f}}, {{0, 1, .5f}}};
    gfx::detail::SetMeshSkinBounds(mesh, Skinned(crossing, {.3f, .7f, 0, 0}, {0, 0, 0, 0}), kTriangle);
    SetBone(palette, 0, kIdentityRows);
    ASSERT_TRUE(gfx::detail::SkinnedMeshPositionBounds(mesh, palette, lo, hi));
    EXPECT_FALSE(gfx::detail::ProjectPresentationBounds(lo, hi, constants, mapping));
}

TEST(GfxGpuMeshDc2, VertexUpdatesRetireSkinBounds) {
    GfxFixture                       fixture(64, 48);
    const std::vector<gfx::Vertex3D> positions = {
        {{-1, 0, .5f}}, {{1, 0, .5f}}, {{0, 1, .5f}}};
    const auto packed = Skinned(positions, {.3f, .7f, 0, 0}, {0, 1, 0, 0});
    const auto handle = gfx::CreateMesh(packed, kTriangle);
    ASSERT_NE(handle, gfx::kNullMesh);
    ASSERT_TRUE(gfx::detail::LookupMesh(handle)->skin_bounds_known);
    ASSERT_TRUE(gfx::UpdateMeshVertices(handle, 0, packed));
    EXPECT_FALSE(gfx::detail::LookupMesh(handle)->skin_bounds_known);
    gfx::DestroyMesh(handle);
}
