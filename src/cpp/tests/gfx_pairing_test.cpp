#include <gtest/gtest.h>

#include <cstring>
#include <vector>

#include "gfx/displaylist.hpp"
#include "tests/gfx_fixture.hpp"

using namespace dc::test;

TEST(GfxGpuMeshDc2, SkippedAndInsertedPartsResynchronizeSurfacePairing) {
    GfxOptions options;
    options.offscreen = true;
    GfxFixture                      fixture(64, 48, 1, options);
    std::vector<gfx::TextureHandle> textures;
    const uint8_t                   white[4] = {128, 128, 128, 128};
    for (int i = 0; i < 4; ++i) {
        const auto texture = gfx::CreateTexture({1, 1});
        ASSERT_TRUE(gfx::UpdateTexture(texture, 0, 0, 0, 1, 1, white));
        textures.push_back(texture);
    }
    const std::vector<gfx::Vertex3D> vertices = {
        {{-.1f, -.1f, .5f}}, {{.1f, -.1f, .5f}}, {{0, .1f, .5f}}};
    const std::vector<uint32_t> indices{0, 1, 2};
    auto                        record = [&](const std::vector<int> &parts, float x, bool mirrored = false) {
        gfx::BeginRecording();
        gfx::Clear(true, white, true, 0);
        gfx::SetInterpKey(123);
        auto transform = gfx::IdentityMeshTransform();
        transform.model[12] = x;
        if (mirrored) {
            transform.model[0] = -1;
        }
        gfx::MeshConstants constants{};
        std::memcpy(constants.mvp, transform.model, sizeof constants.mvp);
        for (int part : parts) {
            gfx::TextureBinding binding;
            binding.texture = textures[part];
            gfx::DrawMeshImmediate(vertices, indices, constants, binding, {}, &transform);
        }
        return gfx::EndRecording();
    };
    auto before = record({0, 1, 2}, 0);
    for (const auto &parts : {
             std::vector<int>{0, 2},
             std::vector<int>{0, 3, 1, 2},
             std::vector<int>{0, 3, 2},
             std::vector<int>{2, 0, 1}
    }) {
        auto after = record(parts, .1f);
        ASSERT_TRUE(gfx::RenderList(*after, 1, {.canonical = true}));
        ASSERT_TRUE(gfx::RenderList(*after, .5f, {.previous = before.get(), .present = true}));
        ASSERT_EQ(after->cache.match.size(), parts.size());
        for (size_t i = 0; i < parts.size(); ++i) {
            EXPECT_EQ(after->cache.match[i], parts[i] == 3 ? -1 : parts[i]) << "part " << parts[i];
        }
    }
    auto mirrored = record({0, 1, 2}, .1f, true);
    ASSERT_TRUE(gfx::RenderList(*mirrored, 1, {.canonical = true}));
    ASSERT_TRUE(gfx::RenderList(*mirrored, .5f, {.previous = before.get(), .present = true}));
    EXPECT_EQ(mirrored->cache.match, (std::vector<int32_t>{-1, -1, -1}));
    auto repeated = record({0, 2, 2}, .1f);
    ASSERT_TRUE(gfx::RenderList(*repeated, 1, {.canonical = true}));
    ASSERT_TRUE(gfx::RenderList(*repeated, .5f, {.previous = before.get(), .present = true}));
    EXPECT_EQ(repeated->cache.match, (std::vector<int32_t>{0, 2, -1}));
    for (auto texture : textures) {
        gfx::DestroyTexture(texture);
    }
}

TEST(GfxGpuMeshDc2, NewGeometryCannotClaimSurvivingSurfacePose) {
    GfxOptions options;
    options.offscreen = true;
    GfxFixture    fixture(64, 48, 1, options);
    const uint8_t white[4] = {128, 128, 128, 128};
    const auto    x = gfx::CreateTexture({1, 1});
    const auto    y = gfx::CreateTexture({1, 1});
    ASSERT_TRUE(gfx::UpdateTexture(x, 0, 0, 0, 1, 1, white));
    ASSERT_TRUE(gfx::UpdateTexture(y, 0, 0, 0, 1, 1, white));
    std::vector<gfx::Vertex3D> geometry[] = {
        {{{-.1f, -.1f, .5f}}, {{.1f, -.1f, .5f}}, {{0, .1f, .5f}}},
        {{{-.2f, -.1f, .5f}}, {{.2f, -.1f, .5f}}, {{0, .2f, .5f}}},
        {{{-.3f, -.1f, .5f}}, {{.3f, -.1f, .5f}}, {{0, .3f, .5f}}}
    };
    const std::vector<uint32_t> triangle{0, 1, 2}, reversed{0, 2, 1};
    gfx::MeshHandle             meshes[3];
    for (int part = 0; part < 3; ++part) {
        meshes[part] = gfx::CreateMesh(geometry[part], triangle);
    }
    for (int mode = 0; mode < 3; ++mode) {
        auto record = [&](bool newest) {
            gfx::BeginRecording();
            gfx::Clear(true, white, true, 0);
            gfx::SetInterpKey(456 + mode);
            auto               transform = gfx::IdentityMeshTransform();
            gfx::MeshConstants constants{};
            std::memcpy(constants.mvp, transform.model, sizeof constants.mvp);
            for (int part : {newest ? 2 : 0, 1}) {
                gfx::TextureBinding binding;
                binding.texture = part == 0 ? x : y;
                if (mode == 0) {
                    gfx::DrawMesh(meshes[part], 0, 3, constants, binding, {}, &transform);
                } else {
                    // Mode 1 differs by topology, mode 2 by immutable immediate geometry.
                    const auto &vertices = mode == 1 ? geometry[0] : geometry[part];
                    const auto &indices = mode == 1 && part == 2 ? reversed : triangle;
                    gfx::DrawMeshImmediate(vertices, indices, constants, binding, {}, &transform);
                }
            }
            return gfx::EndRecording();
        };
        auto before = record(false), after = record(true);
        ASSERT_TRUE(gfx::RenderList(*after, 1, {.canonical = true}));
        ASSERT_TRUE(gfx::RenderList(*after, .5f, {.previous = before.get(), .present = true}));
        EXPECT_EQ(after->cache.match, (std::vector<int32_t>{-1, 1})) << "geometry mode " << mode;
    }
    for (auto mesh : meshes) {
        gfx::DestroyMesh(mesh);
    }
    gfx::DestroyTexture(x);
    gfx::DestroyTexture(y);
}
