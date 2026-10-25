#include "gfx/displaylist.hpp"

#include <algorithm>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <unordered_map>
#include <unordered_set>

#include "gfx/context.hpp"
#include "gfx/profile.hpp"

namespace gfx {

namespace detail {

namespace {

constexpr Mat4 kIdentity = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};

// Validate bindings while their recording still owns them, before a later replay can outlive them.
bool CanRecordBinding(const TextureBinding &binding) {
    if (binding.texture == kNullTexture || binding.texture == kPreviousFrame) {
        return true;
    }
    const Texture *texture = LookupTexture(binding.texture);
    if (texture == nullptr) {
        Error("a recorded draw names unavailable texture %#x", binding.texture);
        return false;
    }
    if (texture->desc.format == TextureFormat::Index8) {
        const Texture *palette = LookupTexture(binding.palette);
        if (palette == nullptr || palette->desc.format != TextureFormat::Rgba8) {
            Error("a recorded indexed draw names unavailable palette %#x", binding.palette);
            return false;
        }
    }
    return true;
}

// Consecutive game frames usually record similar numbers of draws. Reserve their
// large arrays once instead of repeatedly copying commands and bone blocks as
// each vector grows. Only sizes survive; commands and resource ownership do not.
struct RecordingSizes {
    uint64_t instance = 0;
    size_t   entries = 0;
    size_t   records = 0;
    size_t   prog0 = 0;
};

std::array<RecordingSizes, 2> g_recording_sizes;
size_t                        g_recording_kind = 0;
// Read by the worker, which must not touch g.list while the game thread records.
std::atomic<const DisplayList *> g_recording_list{nullptr};

struct Quat {
    float x, y, z, w;
};

struct Vec3 {
    float x, y, z;
};

Vec3 Column(const Mat4 &m, int c) { return {m[c * 4 + 0], m[c * 4 + 1], m[c * 4 + 2]}; }

float Dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

Vec3 Cross(Vec3 a, Vec3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }

Vec3 Scale(Vec3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }

Vec3 Sub(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }

bool Normalize(Vec3 &v) {
    float length = std::sqrt(Dot(v, v));
    if (!(length > 1e-20f)) {
        return false;
    }
    v = Scale(v, 1.0f / length);
    return true;
}

bool IsAffine(const Mat4 &m) { return m[3] == 0.0f && m[7] == 0.0f && m[11] == 0.0f && m[15] == 1.0f; }

// 3x3 row r, column c of a rotation held as three columns.
using Mat3 = std::array<float, 9>;

float At(const Mat3 &m, int r, int c) { return m[c * 3 + r]; }

Quat QuatFromRotation(const Mat3 &m) {
    float trace = At(m, 0, 0) + At(m, 1, 1) + At(m, 2, 2);
    Quat  q;
    if (trace > 0.0f) {
        float s = std::sqrt(trace + 1.0f) * 2.0f;
        q = {(At(m, 2, 1) - At(m, 1, 2)) / s, (At(m, 0, 2) - At(m, 2, 0)) / s,
             (At(m, 1, 0) - At(m, 0, 1)) / s, 0.25f * s};
    } else if (At(m, 0, 0) > At(m, 1, 1) && At(m, 0, 0) > At(m, 2, 2)) {
        float s = std::sqrt(1.0f + At(m, 0, 0) - At(m, 1, 1) - At(m, 2, 2)) * 2.0f;
        q = {0.25f * s, (At(m, 0, 1) + At(m, 1, 0)) / s, (At(m, 0, 2) + At(m, 2, 0)) / s,
             (At(m, 2, 1) - At(m, 1, 2)) / s};
    } else if (At(m, 1, 1) > At(m, 2, 2)) {
        float s = std::sqrt(1.0f + At(m, 1, 1) - At(m, 0, 0) - At(m, 2, 2)) * 2.0f;
        q = {(At(m, 0, 1) + At(m, 1, 0)) / s, 0.25f * s, (At(m, 1, 2) + At(m, 2, 1)) / s,
             (At(m, 0, 2) - At(m, 2, 0)) / s};
    } else {
        float s = std::sqrt(1.0f + At(m, 2, 2) - At(m, 0, 0) - At(m, 1, 1)) * 2.0f;
        q = {(At(m, 0, 2) + At(m, 2, 0)) / s, (At(m, 1, 2) + At(m, 2, 1)) / s, 0.25f * s,
             (At(m, 1, 0) - At(m, 0, 1)) / s};
    }
    return q;
}

Mat3 RotationFromQuat(Quat q) {
    float x = q.x, y = q.y, z = q.z, w = q.w;
    // Columns.
    return {1 - 2 * (y * y + z * z), 2 * (x * y + w * z), 2 * (x * z - w * y),
            2 * (x * y - w * z), 1 - 2 * (x * x + z * z), 2 * (y * z + w * x),
            2 * (x * z + w * y), 2 * (y * z - w * x), 1 - 2 * (x * x + y * y)};
}

Quat Slerp(Quat a, Quat b, float t) {
    float dot = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
    if (dot < 0.0f) {
        b = {-b.x, -b.y, -b.z, -b.w};
        dot = -dot;
    }
    float wa;
    float wb;
    if (dot > 0.9995f) {
        wa = 1.0f - t;
        wb = t;
    } else {
        float theta = std::acos(std::min(dot, 1.0f));
        float sine = std::sin(theta);
        wa = std::sin((1.0f - t) * theta) / sine;
        wb = std::sin(t * theta) / sine;
    }
    Quat  q = {a.x * wa + b.x * wb, a.y * wa + b.y * wb, a.z * wa + b.z * wb, a.w * wa + b.w * wb};
    float length = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    return {q.x / length, q.y / length, q.z / length, q.w / length};
}

// m's 3x3 = R * K with R a rotation (Gram-Schmidt of m's columns) and K the scale and shear in
// R's frame; a reflection stays in K.
struct Decomposed {
    Vec3  translation;
    Quat  rotation;
    Mat3  rest;
    float determinant;
};

bool Decompose(const Mat4 &m, Decomposed &out) {
    if (!IsAffine(m)) {
        return false;
    }
    Vec3 c0 = Column(m, 0);
    Vec3 c1 = Column(m, 1);
    Vec3 c2 = Column(m, 2);
    out.determinant = Dot(c0, Cross(c1, c2));
    if (!(std::fabs(out.determinant) > 1e-12f)) {
        return false;
    }
    Vec3 r0 = c0;
    Vec3 r1 = Sub(c1, Scale(r0, Dot(c1, r0) / Dot(r0, r0)));
    if (!Normalize(r0) || !Normalize(r1)) {
        return false;
    }
    Vec3 r2 = Cross(r0, r1);
    Mat3 rotation = {r0.x, r0.y, r0.z, r1.x, r1.y, r1.z, r2.x, r2.y, r2.z};
    out.rotation = QuatFromRotation(rotation);
    // Rebuilt from the quaternion so that K is measured against the rotation slerp will produce.
    Mat3 exact = RotationFromQuat(out.rotation);
    Vec3 columns[3] = {c0, c1, c2};
    Vec3 axes[3] = {
        {exact[0], exact[1], exact[2]},
        {exact[3], exact[4], exact[5]},
        {exact[6], exact[7], exact[8]}
    };
    for (int c = 0; c < 3; c++) {
        for (int r = 0; r < 3; r++) {
            out.rest[c * 3 + r] = Dot(axes[r], columns[c]);
        }
    }
    out.translation = Column(m, 3);
    return true;
}

Mat4 Compose(Vec3 translation, const Mat3 &rotation, const Mat3 &rest) {
    Mat4 m = kIdentity;
    for (int c = 0; c < 3; c++) {
        for (int r = 0; r < 3; r++) {
            m[c * 4 + r] = At(rotation, r, 0) * rest[c * 3 + 0] + At(rotation, r, 1) * rest[c * 3 + 1] +
                           At(rotation, r, 2) * rest[c * 3 + 2];
        }
    }
    m[12] = translation.x;
    m[13] = translation.y;
    m[14] = translation.z;
    return m;
}

// Also gives the rotation the result has relative to b, for the normals.
bool Interpolate(const Mat4 &a, const Mat4 &b, float alpha, Mat4 &out, Mat3 *delta) {
    Decomposed da;
    Decomposed db;
    if (!Decompose(a, da) || !Decompose(b, db) || (da.determinant < 0.0f) != (db.determinant < 0.0f)) {
        out = b;
        return false;
    }
    Vec3 translation = {da.translation.x + (db.translation.x - da.translation.x) * alpha,
                        da.translation.y + (db.translation.y - da.translation.y) * alpha,
                        da.translation.z + (db.translation.z - da.translation.z) * alpha};
    Quat q = Slerp(da.rotation, db.rotation, alpha);
    Mat3 rest;
    for (int i = 0; i < 9; i++) {
        rest[i] = da.rest[i] + (db.rest[i] - da.rest[i]) * alpha;
    }
    Mat3 rotation = RotationFromQuat(q);
    out = Compose(translation, rotation, rest);
    if (delta != nullptr) {
        Mat3 current = RotationFromQuat(db.rotation);
        for (int c = 0; c < 3; c++) {
            for (int r = 0; r < 3; r++) {
                (*delta)[c * 3 + r] = At(rotation, r, 0) * At(current, c, 0) +
                                      At(rotation, r, 1) * At(current, c, 1) +
                                      At(rotation, r, 2) * At(current, c, 2);
            }
        }
    }
    return true;
}

// Cameras are interpolated as placed in the world, not as the world seen from them.
bool InterpolateView(const Mat4 &a, const Mat4 &b, float alpha, Mat4 &out) {
    Mat4 place_a;
    Mat4 place_b;
    Mat4 place;
    if (!InvertAffine(a, place_a) || !InvertAffine(b, place_b) ||
        !Interpolate(place_a, place_b, alpha, place, nullptr) || !InvertAffine(place, out)) {
        out = b;
        return false;
    }
    return true;
}

Mat4 ToMat4(const float *m) {
    Mat4 out;
    std::memcpy(out.data(), m, sizeof(float) * 16);
    return out;
}

uint32_t CameraIndex(DisplayList &list, const float *view) {
    for (uint32_t i = 0; i < list.cameras.size(); i++) {
        if (std::memcmp(list.cameras[i].data(), view, sizeof(float) * 16) == 0) {
            return i;
        }
    }
    list.cameras.push_back(ToMat4(view));
    return static_cast<uint32_t>(list.cameras.size() - 1);
}

// The scene the 3D sprites recorded after this mesh stand in.
void NoteScene(DisplayList &list, const MeshRecord &record) {
    LogicalMapping mapping = GetLogicalMapping(g.record_target);
    if (mapping.pixel_width == 0 || mapping.pixel_height == 0 || mapping.scale_x == 0.0f || mapping.scale_y == 0.0f) {
        list.scene = -1;
        list.target_scenes.erase(g.record_target);
        return;
    }
    SpriteScene scene;
    scene.camera = record.camera;
    scene.projection = record.projection;
    scene.to_clip[0] = 2.0f * mapping.scale_x / static_cast<float>(mapping.pixel_width);
    scene.to_clip[1] = 2.0f * mapping.scale_y / static_cast<float>(mapping.pixel_height);
    scene.to_clip[2] = 2.0f * mapping.offset_x / static_cast<float>(mapping.pixel_width) - 1.0f;
    scene.to_clip[3] = 2.0f * mapping.offset_y / static_cast<float>(mapping.pixel_height) - 1.0f;
    if (list.scene >= 0) {
        const SpriteScene &current = list.scenes[list.scene];
        if (current.camera == scene.camera && current.projection == scene.projection &&
            std::memcmp(current.to_clip, scene.to_clip, sizeof(scene.to_clip)) == 0) {
            return;
        }
    }
    list.scene = static_cast<int32_t>(list.scenes.size());
    list.scenes.push_back(scene);
    list.target_scenes[g.record_target] = list.scene;
}

bool CoversLogicalFrame(const LogicalRect &rect) {
    return rect.x <= 0.0f && rect.y <= 0.0f && rect.x + rect.w >= g.config.logical_width &&
           rect.y + rect.h >= g.config.logical_height;
}

void NoteMainDraw(DisplayList &list) {
    if (g.record_target == kMainTarget && !list.main_cleared) {
        list.needs_base = true;
    }
}

// Whether an immediate mesh is the one before with its vertices moved, none further than a teleport.
bool VerticesBlend(const MeshEntry &before, const MeshEntry &after, float teleport_distance) {
    if (before.mesh != kNullMesh || after.mesh != kNullMesh || before.vertices.size() != after.vertices.size()) {
        return false;
    }
    bool moved = false;
    for (size_t i = 0; i < after.vertices.size(); i++) {
        const Vertex3D &a = before.vertices[i];
        const Vertex3D &b = after.vertices[i];
        float           dx = b.position[0] - a.position[0];
        float           dy = b.position[1] - a.position[1];
        float           dz = b.position[2] - a.position[2];
        if (std::sqrt(dx * dx + dy * dy + dz * dz) > teleport_distance) {
            return false;
        }
        moved = moved || dx != 0.0f || dy != 0.0f || dz != 0.0f ||
                std::memcmp(a.normal, b.normal, sizeof(a.normal)) != 0;
    }
    return moved;
}

// Positions lerped, normals lerped and normalised; the rest is b's.
void BlendVertices(const std::vector<Vertex3D> &a, const std::vector<Vertex3D> &b, float alpha,
                   std::vector<Vertex3D> &out) {
    out = b;
    for (size_t i = 0; i < out.size(); i++) {
        for (int k = 0; k < 3; k++) {
            out[i].position[k] = a[i].position[k] + (b[i].position[k] - a[i].position[k]) * alpha;
        }
        Vec3 normal = {a[i].normal[0] + (b[i].normal[0] - a[i].normal[0]) * alpha,
                       a[i].normal[1] + (b[i].normal[1] - a[i].normal[1]) * alpha,
                       a[i].normal[2] + (b[i].normal[2] - a[i].normal[2]) * alpha};
        if (Normalize(normal)) {
            out[i].normal[0] = normal.x;
            out[i].normal[1] = normal.y;
            out[i].normal[2] = normal.z;
        }
    }
}

// A key drawn with more than one model in a tick is one model placed several times (the town's
// parts and grid cells, each a shared frame moved from place to place), and which of its places are
// drawn changes with the camera's culling, so the n-th draw of one tick is not the n-th of the next.
// Keys seen so since the last cut.
std::unordered_set<InterpKey> g_instanced_keys;
// Cuts recorded so far. Each list keeps the count it was finished at; see BuildMatches.
uint64_t g_cut_generation = 0;

// How far one place of an instanced key may move in a tick and still be the same place.
constexpr float kInstanceStep = 25.0f;

struct KeyDraws {
    std::vector<int32_t> records;
    bool                 instanced = false;
};

std::unordered_map<InterpKey, KeyDraws> DrawsByKey(const DisplayList &list) {
    std::unordered_map<InterpKey, KeyDraws> keys;
    for (size_t i = 0; i < list.records.size(); i++) {
        const MeshRecord &record = list.records[i];
        if (record.key == 0 || !record.has_transform) {
            continue;
        }
        KeyDraws &draws = keys[record.key];
        draws.instanced = draws.instanced ||
                          (!draws.records.empty() && list.records[draws.records.front()].model != record.model);
        draws.records.push_back(static_cast<int32_t>(i));
    }
    return keys;
}

float Moved(const Mat4 &before, const Mat4 &after) {
    float dx = after[12] - before[12];
    float dy = after[13] - before[13];
    float dz = after[14] - before[14];
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

// Placement identifies an instance, but its faces still need the same surface. In particular,
// borrowing a different face's bone palette or vertex rewrite is not a valid interpolation.
bool SameSurface(const MeshEntry &a, const MeshEntry &b) {
    if (a.binding.texture != b.binding.texture || a.binding.palette != b.binding.palette ||
        a.binding.filter != b.binding.filter || a.binding.wrap_u != b.binding.wrap_u ||
        a.binding.wrap_v != b.binding.wrap_v || a.binding.ignore_alpha != b.binding.ignore_alpha ||
        a.first_index != b.first_index || a.index_count != b.index_count ||
        (a.prog0 >= 0) != (b.prog0 >= 0)) {
        return false;
    }
    return true;
}

bool SameHandedness(const Mat4 &a, const Mat4 &b) {
    const float sign_a = Dot(Column(a, 0), Cross(Column(a, 1), Column(a, 2)));
    const float sign_b = Dot(Column(b, 0), Cross(Column(b, 1), Column(b, 2)));
    return (sign_a < 0.0f) == (sign_b < 0.0f);
}

bool SamePartGeometry(const MeshEntry &a, const MeshEntry &b, const MeshRecord &record,
                      const MeshRecord &earlier) {
    if (a.mesh != b.mesh) {
        return false;
    }
    if (a.mesh != kNullMesh) {
        return true;
    }
    if (a.indices != b.indices || a.vertices.size() != b.vertices.size()) {
        return false;
    }
    bool same_vertices = true;
    for (size_t i = 0; i < a.vertices.size(); ++i) {
        same_vertices &= std::memcmp(&a.vertices[i], &b.vertices[i], sizeof(Vertex3D)) == 0;
    }
    if (same_vertices) {
        return true;
    }
    // Explicit vertex blending promises the same points in submission order.
    // Without an immutable mesh handle, moving geometry cannot resynchronize to
    // another occurrence: that would consume an unrelated part's pose.
    if (!record.blend_vertices || !earlier.blend_vertices || record.occurrence != earlier.occurrence) {
        return false;
    }
    for (size_t i = 0; i < a.vertices.size(); ++i) {
        if (std::memcmp(a.vertices[i].uv, b.vertices[i].uv, sizeof(a.vertices[i].uv)) != 0 ||
            std::memcmp(a.vertices[i].color, b.vertices[i].color, sizeof(a.vertices[i].color)) != 0) {
            return false;
        }
    }
    return true;
}

bool SameInstanceFace(const DisplayList &list, const DisplayList &previous, int32_t index, int32_t candidate) {
    const auto &a = std::get<MeshEntry>(list.entries[list.records[index].entry]);
    const auto &b = std::get<MeshEntry>(previous.entries[previous.records[candidate].entry]);
    // Normal and mirrored sky passes share an origin and surface. An opposite reflection
    // cannot interpolate; keep each pass with its own handedness before comparing distance.
    const Mat4 &model_a = list.records[index].model;
    const Mat4 &model_b = previous.records[candidate].model;
    return SameHandedness(model_a, model_b) && SameSurface(a, b) &&
           SamePartGeometry(a, b, list.records[index], previous.records[candidate]);
}

// Each place with the previous draw that stood exactly there, then what is left with the nearest
// previous place nothing stood still at, within a step.
void MatchInstanced(const DisplayList &list, const DisplayList &previous, const KeyDraws &draws,
                    const KeyDraws &before, std::vector<int32_t> &match) {
    std::vector<bool> stood(before.records.size(), false);
    for (int32_t index : draws.records) {
        const MeshRecord &record = list.records[index];
        if (record.no_interpolation) {
            continue;
        }
        for (size_t i = 0; i < before.records.size(); i++) {
            if (!stood[i] && previous.records[before.records[i]].model == record.model &&
                SameInstanceFace(list, previous, index, before.records[i])) {
                stood[i] = true;
                match[index] = before.records[i];
                break;
            }
        }
    }
    for (int32_t index : draws.records) {
        const MeshRecord &record = list.records[index];
        if (record.no_interpolation || match[index] >= 0) {
            continue;
        }
        float nearest = std::min(kInstanceStep, record.teleport_distance);
        for (size_t i = 0; i < before.records.size(); i++) {
            float moved = Moved(previous.records[before.records[i]].model, record.model);
            if (!stood[i] && moved <= nearest && SameInstanceFace(list, previous, index, before.records[i])) {
                nearest = moved;
                match[index] = before.records[i];
            }
        }
        if (match[index] >= 0) {
            auto found = std::find(before.records.begin(), before.records.end(), match[index]);
            stood[static_cast<size_t>(found - before.records.begin())] = true;
        }
    }
}

void BuildMatches(const DisplayList &list, const DisplayList &previous) {
    KZ_PROFILE_ZONE("Kazarov BuildMatches");
    // The first list finished after a cut clears the keys seen before it.
    static uint64_t seen = 0;
    if (list.cut_generation != seen) {
        g_instanced_keys.clear();
        seen = list.cut_generation;
    }
    MatchCache &cache = list.cache;
    cache.previous_serial = previous.serial;
    cache.match.assign(list.records.size(), -1);
    cache.blend.assign(list.records.size(), false);
    cache.draws_2d.clear();
    if (!list.camera_cut && !list.tagged_2d.empty()) {
        std::unordered_map<InterpKey, std::vector<uint32_t> > before_2d;
        for (uint32_t entry : previous.tagged_2d) {
            before_2d[std::get<Draw2DEntry>(previous.entries[entry]).interpolation.key].push_back(entry);
        }
        for (uint32_t entry : list.tagged_2d) {
            const auto &draw = std::get<Draw2DEntry>(list.entries[entry]);
            auto        found = before_2d.find(draw.interpolation.key);
            if (found == before_2d.end()) {
                continue;
            }
            for (uint32_t candidate : found->second) {
                const auto &earlier = std::get<Draw2DEntry>(previous.entries[candidate]);
                if (earlier.occurrence == draw.occurrence &&
                    earlier.interpolation.generation == draw.interpolation.generation) {
                    cache.draws_2d.push_back({entry, candidate});
                    break;
                }
            }
        }
    }

    std::unordered_map<InterpKey, KeyDraws> before = DrawsByKey(previous);
    std::unordered_map<InterpKey, KeyDraws> now = DrawsByKey(list);
    for (const auto *keys : {&before, &now}) {
        for (const auto &[key, draws] : *keys) {
            if (draws.instanced) {
                g_instanced_keys.insert(key);
            }
        }
    }
    for (const auto &[key, draws] : now) {
        auto it = before.find(key);
        if (it == before.end()) {
            continue;
        }
        if (g_instanced_keys.contains(key)) {
            MatchInstanced(list, previous, draws, it->second, cache.match);
            continue;
        }
        // Pair occurrences within each surface, not the entire model. A culled or inserted
        // part must not shift every later part away from its own predecessor.
        std::vector<bool> claimed(it->second.records.size(), false);
        for (int32_t index : draws.records) {
            const MeshRecord &record = list.records[index];
            for (size_t face = 0; face < it->second.records.size(); ++face) {
                const int32_t     candidate = it->second.records[face];
                const MeshRecord &earlier = previous.records[candidate];
                if (claimed[face] || !SameInstanceFace(list, previous, index, candidate)) {
                    continue;
                }
                claimed[face] = true;
                if (!record.no_interpolation && !(Moved(earlier.model, record.model) > record.teleport_distance)) {
                    cache.match[index] = candidate;
                    cache.blend[index] =
                        record.blend_vertices && earlier.blend_vertices &&
                        VerticesBlend(std::get<MeshEntry>(previous.entries[earlier.entry]),
                                      std::get<MeshEntry>(list.entries[record.entry]), record.teleport_distance);
                }
                break;
            }
        }
    }
    cache.cameras.assign(list.cameras.size(), -1);
    if (!list.camera_cut) {
        for (size_t i = 0; i < list.cameras.size() && i < previous.cameras.size(); i++) {
            cache.cameras[i] = static_cast<int32_t>(i);
        }
    }
}

struct Overrides {
    std::vector<int32_t>               index; // per record, into constants, or -1
    std::vector<MeshConstants>         constants;
    std::vector<int32_t>               vertex_index; // per record, into vertices, or -1
    std::vector<std::vector<Vertex3D>> vertices;
    // The 3D sprites whose scene's camera moved, as the interpolated camera sees them.
    std::unordered_map<const Draw2DEntry *, std::vector<Vertex2D>> sprites;
    // The vertex rewrites of meshes a matched draw uses (skinning), between the previous tick's
    // rewrite and this one's.
    std::unordered_map<const UpdateMeshEntry *, std::vector<Vertex3D>> poses;
    std::vector<int32_t>                                           prog0_index; // per record, into prog0, or -1
    std::vector<Prog0Constants>                                    prog0;
    std::vector<int32_t>                                           palette_index; // per record, into palettes, or -1
    std::vector<Prog0Palette>                                      palettes;

    // Empties every container, so a reused Overrides holds nothing from its previous frame.
    void Clear() {
        index.clear();
        constants.clear();
        vertex_index.clear();
        vertices.clear();
        sprites.clear();
        poses.clear();
        prog0_index.clear();
        prog0.clear();
        palette_index.clear();
        palettes.clear();
    }
};

bool Invert(const Mat4 &m, Mat4 &out) {
    double a[4][8];
    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 4; c++) {
            a[r][c] = m[c * 4 + r];
            a[r][c + 4] = r == c ? 1.0 : 0.0;
        }
    }
    for (int column = 0; column < 4; column++) {
        int pivot = column;
        for (int r = column + 1; r < 4; r++) {
            if (std::fabs(a[r][column]) > std::fabs(a[pivot][column])) {
                pivot = r;
            }
        }
        if (std::fabs(a[pivot][column]) < 1e-30) {
            return false;
        }
        std::swap(a[pivot], a[column]);
        double scale = 1.0 / a[column][column];
        for (int c = 0; c < 8; c++) {
            a[column][c] *= scale;
        }
        for (int r = 0; r < 4; r++) {
            if (r == column) {
                continue;
            }
            double factor = a[r][column];
            for (int c = 0; c < 8; c++) {
                a[r][c] -= factor * a[column][c];
            }
        }
    }
    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 4; c++) {
            out[c * 4 + r] = static_cast<float>(a[r][c + 4]);
        }
    }
    return true;
}

// A 3D sprite is recorded as the screen corners the tick's camera gave it, so replayed as it is
// it stands a tick ahead of the meshes a display render draws under the interpolated camera: a
// torch's flame and glow shake against the torch whenever the camera moves. Its corners are taken
// back through the tick's camera to where they stood in the world and seen again from the
// interpolated one. A corner that falls behind that camera leaves the sprite as recorded.
bool MoveSprite(const Draw2DEntry &sprite, const SpriteScene &scene, const Mat4 &carry,
                std::vector<Vertex2D> &out) {
    out = sprite.vertices;
    for (Vertex2D &vertex : out) {
        const float in[4] = {vertex.x * scene.to_clip[0] + scene.to_clip[2],
                             vertex.y * scene.to_clip[1] + scene.to_clip[3], vertex.z, 1.0f};
        float       clip[4];
        for (int r = 0; r < 4; r++) {
            clip[r] = carry[0 * 4 + r] * in[0] + carry[1 * 4 + r] * in[1] + carry[2 * 4 + r] * in[2] +
                      carry[3 * 4 + r] * in[3];
        }
        if (!(clip[3] > 1e-6f) || !std::isfinite(clip[0]) || !std::isfinite(clip[1]) || !std::isfinite(clip[2])) {
            return false;
        }
        vertex.x = (clip[0] / clip[3] - scene.to_clip[2]) / scene.to_clip[0];
        vertex.y = (clip[1] / clip[3] - scene.to_clip[3]) / scene.to_clip[1];
        vertex.z = clip[2] / clip[3];
    }
    return true;
}

void MoveSprites(const DisplayList &list, const std::vector<Mat4> &views, const std::vector<bool> &moved,
                 Overrides &out) {
    std::vector<Mat4> carry(list.scenes.size());
    std::vector<int>  state(list.scenes.size(), 0);
    for (const Entry &entry : list.entries) {
        const Draw2DEntry *sprite = std::get_if<Draw2DEntry>(&entry);
        if (sprite == nullptr || sprite->scene < 0) {
            continue;
        }
        const SpriteScene &scene = list.scenes[sprite->scene];
        if (!moved[scene.camera]) {
            continue;
        }
        if (state[sprite->scene] == 0) {
            Mat4 unproject;
            Mat4 place;
            state[sprite->scene] = -1;
            if (Invert(scene.projection, unproject) && InvertAffine(list.cameras[scene.camera], place)) {
                carry[sprite->scene] = Multiply(Multiply(scene.projection, views[scene.camera]),
                                                Multiply(place, unproject));
                state[sprite->scene] = 1;
            }
        }
        std::vector<Vertex2D> vertices;
        if (state[sprite->scene] == 1 && MoveSprite(*sprite, scene, carry[sprite->scene], vertices)) {
            out.sprites.emplace(sprite, std::move(vertices));
        }
    }
}

// For each record, the rewrite of its mesh's vertices that the draw saw (the latest before it), or null.
std::vector<const UpdateMeshEntry *> RecordPoses(const DisplayList &list) {
    std::vector<const UpdateMeshEntry *>                    poses(list.records.size(), nullptr);
    std::unordered_map<MeshHandle, const UpdateMeshEntry *> latest;
    for (const Entry &entry : list.entries) {
        if (const UpdateMeshEntry *update = std::get_if<UpdateMeshEntry>(&entry)) {
            latest[update->mesh] = update;
        } else if (const MeshEntry *mesh = std::get_if<MeshEntry>(&entry); mesh != nullptr && mesh->mesh != kNullMesh) {
            if (auto pose = latest.find(mesh->mesh); pose != latest.end()) {
                poses[mesh->record] = pose->second;
            }
        }
    }
    return poses;
}

uint64_t RewriteKey(const UpdateMeshEntry &update) {
    return (static_cast<uint64_t>(update.mesh) << 32) | update.first;
}

Rewrites ListRewrites(const DisplayList &list) {
    Rewrites rewrites;
    for (const Entry &entry : list.entries) {
        if (const UpdateMeshEntry *update = std::get_if<UpdateMeshEntry>(&entry)) {
            auto [it, added] = rewrites.last.insert_or_assign(RewriteKey(*update), update);
            if (!added) {
                rewrites.repeated.insert(it->first);
            }
        }
    }
    return rewrites;
}

// Built on first use: a finished list never changes, and each recording is a new DisplayList.
const Rewrites &CachedRewrites(const DisplayList &list) {
    if (!list.cache.rewrites) {
        list.cache.rewrites = ListRewrites(list);
    }
    return *list.cache.rewrites;
}

// A skinned mesh is rewritten every tick in its model's space and drawn under the model's matrix.
// The matrix is interpolated, and so is everything that hangs from the body by a matrix or vertices
// of its own (a weapon in the hand, the cloth), so a body left in the tick's pose stands ahead of
// them by what is left of the tick: the cloth cuts through it and the hand leaves the weapon, the
// further the faster the pose changes. The rewrite a matched draw saw is blended with the one its
// match saw in the previous tick (the two are different meshes where the game keeps a buffer per
// display buffer), unless a vertex moved further than the draw's teleport distance. Each draw of a
// mesh shared by several models takes its own rewrite.
void PosesBetween(const DisplayList &list, const DisplayList &previous, float alpha, Overrides &out) {
    KZ_PROFILE_ZONE("Kazarov PosesBetween");
    std::vector<const UpdateMeshEntry *> now = RecordPoses(list);
    if (std::ranges::none_of(now, [](const UpdateMeshEntry *pose) { return pose != nullptr; })) {
        return;
    }
    std::vector<const UpdateMeshEntry *> before = RecordPoses(previous);
    const MatchCache                    &cache = list.cache;
    for (size_t i = 0; i < list.records.size(); i++) {
        const MeshRecord &record = list.records[i];
        int32_t           match = cache.match[i];
        const UpdateMeshEntry *pose = now[i];
        if (match < 0 || pose == nullptr || out.poses.contains(pose)) {
            continue;
        }
        const UpdateMeshEntry *old = before[match];
        if (old == nullptr || old->first != pose->first || old->vertices.size() != pose->vertices.size()) {
            continue;
        }
        const std::vector<Vertex3D> &a = old->vertices;
        const std::vector<Vertex3D> &b = pose->vertices;
        bool                         same = true;
        bool                         teleported = false;
        for (size_t v = 0; v < b.size() && !teleported; v++) {
            float dx = b[v].position[0] - a[v].position[0];
            float dy = b[v].position[1] - a[v].position[1];
            float dz = b[v].position[2] - a[v].position[2];
            same = same && dx == 0.0f && dy == 0.0f && dz == 0.0f;
            teleported = std::sqrt(dx * dx + dy * dy + dz * dz) > record.teleport_distance;
        }
        if (same || teleported) {
            continue;
        }
        BlendVertices(a, b, alpha, out.poses[pose]);
    }
}

Mat4 BoneMatrix(const float (&rows)[3][4]) {
    Mat4 m = {};
    for (int r = 0; r < 3; r++) {
        for (int c = 0; c < 4; c++) {
            m[c * 4 + r] = rows[r][c];
        }
    }
    m[15] = 1.0f;
    return m;
}

void SetBoneRows(const Mat4 &m, float (&rows)[3][4]) {
    for (int r = 0; r < 3; r++) {
        for (int c = 0; c < 4; c++) {
            rows[r][c] = m[c * 4 + r];
        }
    }
}

// Re-bases a block recorded at view_b and model_b onto view_i and model_i. The clip goes through
// inverse(eye_b) so the game's left factor (fit * world clip, or the view) is kept whichever of the two
// clip_or_view holds. False, block untouched, when either eye transform is singular.
// The eye transforms MoveProg0 last derived, kept for the draws that follow with the same matrices: a
// visual's faces are drawn one after another under one transform.
struct Prog0Rebase {
    Mat4 view_b = {};
    Mat4 view_i = {};
    Mat4 middle = {};
    Mat4 model_b = {};
    Mat4 model_i = {};
    bool valid = false;
    bool invertible = false;
    Mat4 back_b = {};    // inverse of the tick's eye transform
    Mat4 eye_i = {};     // the interpolated eye transform
    Mat4 to_object = {}; // the tick's eye space back to the interpolated object space
};

bool MoveProg0(const MeshRecord &record, const Mat4 &view_b, const Mat4 &view_i, const Mat4 &model_i,
               Prog0Constants &block, Prog0Rebase &rebase) {
    if (!rebase.valid || rebase.view_b != view_b || rebase.view_i != view_i || rebase.middle != record.middle ||
        rebase.model_b != record.model || rebase.model_i != model_i) {
        rebase.view_b = view_b;
        rebase.view_i = view_i;
        rebase.middle = record.middle;
        rebase.model_b = record.model;
        rebase.model_i = model_i;
        rebase.valid = true;
        Mat4 eye_b = Multiply(Multiply(view_b, record.middle), record.model);
        rebase.eye_i = Multiply(Multiply(view_i, record.middle), model_i);
        Mat4 back_i = {};
        rebase.invertible = Invert(eye_b, rebase.back_b) && Invert(rebase.eye_i, back_i);
        if (rebase.invertible) {
            rebase.to_object = Multiply(back_i, eye_b);
        }
    }
    if (!rebase.invertible) {
        return false;
    }
    Mat4 clip = Multiply(Multiply(ToMat4(block.clip_or_view), rebase.back_b), rebase.eye_i);
    std::memcpy(block.clip_or_view, clip.data(), sizeof block.clip_or_view);

    // The camera is an object-space point: it goes to eye space under the tick's transform and back
    // under the interpolated one. A projective middle leaves w != 1, so the point is homogenised.
    const Mat4 &to_object = rebase.to_object;
    const float camera[4] = {block.eye[0], block.eye[1], block.eye[2], 1.0f};
    float       h[4];
    for (int r = 0; r < 4; r++) {
        h[r] = to_object[0 * 4 + r] * camera[0] + to_object[1 * 4 + r] * camera[1] +
               to_object[2 * 4 + r] * camera[2] + to_object[3 * 4 + r] * camera[3];
    }
    for (int k = 0; k < 3; k++) {
        block.eye[k] = std::fabs(h[3]) > 1e-12f ? h[k] / h[3] : h[k];
    }

    std::memcpy(block.model_world, model_i.data(), sizeof block.model_world);
    // Point lights are relative to the object's origin, so they follow the origin.
    for (int k = 0; k < 3; k++) {
        float shift = record.model[12 + k] - model_i[12 + k];
        for (int i = 0; i < 4; i++) {
            block.point_light[i][k] += shift;
        }
    }
    return true;
}

void Interpolated(const DisplayList &list, const DisplayList &previous, float alpha, Overrides &out) {
    KZ_PROFILE_ZONE("Kazarov Interpolated");
    if (list.cache.previous_serial != previous.serial) {
        BuildMatches(list, previous);
    }
    const MatchCache &cache = list.cache;

    std::vector<Mat4> views(list.cameras.size());
    std::vector<bool> moved(list.cameras.size(), false);
    {
        KZ_PROFILE_ZONE("Kazarov interpolate cameras");
        for (size_t i = 0; i < list.cameras.size(); i++) {
            views[i] = list.cameras[i];
            int32_t before = cache.cameras[i];
            if (before >= 0 && previous.cameras[before] != list.cameras[i]) {
                moved[i] = InterpolateView(previous.cameras[before], list.cameras[i], alpha, views[i]);
            }
        }
    }

    MoveSprites(list, views, moved, out);
    for (const auto &pair : cache.draws_2d) {
        const auto           &draw = std::get<Draw2DEntry>(list.entries[pair[0]]);
        const auto           &before = std::get<Draw2DEntry>(previous.entries[pair[1]]);
        std::vector<Vertex2D> vertices;
        bool                  blended = false;
        // Camera pairing currently uses first-appearance order. Multi-view lists cannot prove
        // that association after a reorder, so keep their own sprite motion newest.
        if (draw.interpolation.world_corners && draw.scene >= 0 && before.scene >= 0 &&
            list.cameras.size() == 1 && previous.cameras.size() == 1) {
            const auto &scene = list.scenes[draw.scene];
            const auto &earlier = previous.scenes[before.scene];
            if (cache.cameras[scene.camera] == static_cast<int32_t>(earlier.camera)) {
                blended = InterpolateSprite(before, earlier, previous.cameras[earlier.camera],
                                            draw, scene, list.cameras[scene.camera], views[scene.camera], alpha, vertices);
            }
        } else {
            blended = Interpolate2D(before, draw, alpha, vertices);
        }
        if (blended) {
            out.sprites[&draw] = std::move(vertices);
        }
    }

    out.index.assign(list.records.size(), -1);
    out.constants.clear();
    out.vertex_index.assign(list.records.size(), -1);
    out.vertices.clear();
    out.prog0_index.assign(list.records.size(), -1);
    out.prog0.clear();
    out.palette_index.assign(list.records.size(), -1);
    out.palettes.clear();
    const Mat4 *last_before = nullptr;
    const Mat4 *last_after = nullptr;
    Mat4        last_model;
    Mat3        last_delta = {};
    bool        last_turned = false;
    Prog0Rebase rebase;
    // Draws under one camera share their world-to-clip matrix; it is rebuilt only when the camera,
    // projection or middle matrix changes.
    std::optional<uint32_t> clip_camera;
    Mat4                    clip_projection = {};
    Mat4                    clip_middle = {};
    Mat4                    world_to_clip = {};
    for (const Entry &entry : list.entries) {
        const MeshEntry *mesh = std::get_if<MeshEntry>(&entry);
        if (mesh == nullptr) {
            continue;
        }
        const MeshRecord &record = list.records[mesh->record];
        if (!record.has_transform) {
            continue;
        }
        Mat4    model = record.model;
        int32_t match = cache.match[mesh->record];
        if (match >= 0 && cache.blend[mesh->record]) {
            const MeshEntry &before = std::get<MeshEntry>(previous.entries[previous.records[match].entry]);
            out.vertex_index[mesh->record] = static_cast<int32_t>(out.vertices.size());
            BlendVertices(before.vertices, mesh->vertices, alpha, out.vertices.emplace_back());
        }
        if (match >= 0 && previous.records[match].model != record.model) {
            // A model's strips and passes come one after another with the same pair of matrices.
            const Mat4 &before = previous.records[match].model;
            if (last_before == nullptr || *last_before != before || *last_after != record.model) {
                last_before = &before;
                last_after = &record.model;
                last_turned = Interpolate(before, record.model, alpha, last_model, &last_delta);
            }
            model = last_model;
        }
        bool        turned = match >= 0 && previous.records[match].model != record.model && last_turned;
        const Mat3 &delta = last_delta;
        if (mesh->prog0 >= 0) {
            const Prog0Constants &newest = list.prog0_blocks[static_cast<size_t>(mesh->prog0)];
            const bool            skinned = (newest.flags & kProg0Skinned) != 0;
            // Camera motion applies to every transformed draw, including newly visible faces
            // and material cuts with no previous pose. Their clip planes and eye must follow it.
            const bool move = model != record.model || moved[record.camera];
            // Overrides are built in place and dropped again when nothing in them changed.
            if (skinned && match >= 0 && mesh->palette >= 0) {
                const MeshEntry &earlier = std::get<MeshEntry>(previous.entries[previous.records[match].entry]);
                if (earlier.prog0 >= 0 && earlier.palette >= 0 &&
                    (previous.prog0_blocks[static_cast<size_t>(earlier.prog0)].flags & kProg0Skinned) != 0) {
                    const Prog0Palette &newest_palette = list.palettes[static_cast<size_t>(mesh->palette)];
                    Prog0Palette       &blended = out.palettes.emplace_back();
                    BlendProg0Bones(previous.palettes[static_cast<size_t>(earlier.palette)], newest_palette, alpha,
                                    record.teleport_distance, blended);
                    if (std::memcmp(&blended, &newest_palette, sizeof blended) != 0) {
                        out.palette_index[mesh->record] = static_cast<int32_t>(out.palettes.size() - 1);
                    } else {
                        out.palettes.pop_back();
                    }
                }
            }
            if (move) {
                Prog0Constants &block = out.prog0.emplace_back(newest);
                if (MoveProg0(record, list.cameras[record.camera], views[record.camera], model, block, rebase)) {
                    out.prog0_index[mesh->record] = static_cast<int32_t>(out.prog0.size() - 1);
                } else {
                    out.prog0.pop_back();
                }
            }
        }
        if (!turned && !moved[record.camera]) {
            continue;
        }
        if (clip_camera != record.camera || clip_projection != record.projection || clip_middle != record.middle) {
            clip_camera = record.camera;
            clip_projection = record.projection;
            clip_middle = record.middle;
            world_to_clip = Multiply(Multiply(record.projection, views[record.camera]), record.middle);
        }
        MeshConstants constants = mesh->constants;
        Mat4          mvp = Multiply(world_to_clip, Multiply(model, record.local));
        std::memcpy(constants.mvp, mvp.data(), sizeof(constants.mvp));
        if (turned) {
            for (int c = 0; c < 3; c++) {
                float column[3];
                for (int r = 0; r < 3; r++) {
                    column[r] = At(delta, r, 0) * mesh->constants.normal_matrix[c * 4 + 0] +
                                At(delta, r, 1) * mesh->constants.normal_matrix[c * 4 + 1] +
                                At(delta, r, 2) * mesh->constants.normal_matrix[c * 4 + 2];
                }
                std::memcpy(&constants.normal_matrix[c * 4], column, sizeof(column));
            }
        }
        out.index[mesh->record] = static_cast<int32_t>(out.constants.size());
        out.constants.push_back(constants);
    }

    PosesBetween(list, previous, alpha, out);
}

// A grab of the frame (a copy or blit out of the main target) is taken by the canonical render at
// the tick's camera. A display render draws the scene under another camera, so what it draws back
// from the canonical grab (the depth of field's blurred bands, the water's refraction) stands
// beside the scene it was taken from. It takes the grab again from its own image instead, into a
// twin of the destination that its draws then sample; the destination keeps what the canonical
// render left.
struct GrabTwin {
    TextureHandle twin = kNullTexture;
    TextureInfo   shape = {};
    bool          active = false;
};

std::unordered_map<TextureHandle, GrabTwin> g_grab_twins;
uint64_t                                    g_grab_instance = 0;

bool SameShape(const TextureInfo &a, const TextureInfo &b) {
    return a.width == b.width && a.height == b.height && a.pixel_width == b.pixel_width &&
           a.pixel_height == b.pixel_height && a.format == b.format && a.mip_levels == b.mip_levels &&
           a.has_alpha == b.has_alpha && a.render_target == b.render_target &&
           a.shares_main_depth == b.shares_main_depth && a.frame_target == b.frame_target;
}

// Every twin starts a display render unused; those whose destination is gone are destroyed.
void ResetGrabTwins() {
    if (g_grab_instance != g.renderer_instance) {
        g_grab_twins.clear();
        g_grab_instance = g.renderer_instance;
    }
    for (auto it = g_grab_twins.begin(); it != g_grab_twins.end();) {
        it->second.active = false;
        if (!GetTextureInfo(it->first)) {
            DestroyTexture(it->second.twin);
            it = g_grab_twins.erase(it);
        } else {
            ++it;
        }
    }
}

// The twin this display render samples in place of texture, if it has grabbed into one.
TextureHandle GrabbedTwin(TextureHandle texture) {
    auto it = g_grab_twins.find(texture);
    return it != g_grab_twins.end() && it->second.active ? it->second.twin : texture;
}

// The pixels [low, high) a logical span maps to along one axis, clamped to the image as the transfer
// clamps them. An extended axis is widened to the whole image by the transfer, so this never
// overstates what it writes.
struct PixelSpan {
    int32_t low;
    int32_t high;
};

PixelSpan MappedSpan(int32_t start, int32_t length, float scale, float offset, uint32_t pixels) {
    auto at = [&](int32_t value) {
        return std::clamp(static_cast<int32_t>(std::lround(static_cast<float>(value) * scale + offset)), 0,
                          static_cast<int32_t>(pixels));
    };
    int32_t a = at(start);
    int32_t b = at(start + length);
    return {std::min(a, b), std::max(a, b)};
}

// Whether the grab writes every texel of its destination, so the destination's prior contents are
// never read. A copy writes only the span both images share, and only the first mip is written.
bool CoversDestination(const CopyEntry &entry, const TextureInfo &info) {
    std::optional<TextureInfo> src_info = GetTextureInfo(entry.src);
    if (info.mip_levels > 1 || !src_info || src_info->format != info.format) {
        return false;
    }
    LogicalMapping dst_mapping = GetLogicalMapping(entry.dst);
    PixelSpan      dst_x = MappedSpan(entry.dst_rect.x, entry.dst_rect.w, dst_mapping.scale_x, dst_mapping.offset_x,
                                      info.pixel_width);
    PixelSpan      dst_y = MappedSpan(entry.dst_rect.y, entry.dst_rect.h, dst_mapping.scale_y, dst_mapping.offset_y,
                                      info.pixel_height);
    if (dst_x.low != 0 || dst_x.high != static_cast<int32_t>(info.pixel_width) || dst_y.low != 0 ||
        dst_y.high != static_cast<int32_t>(info.pixel_height)) {
        return false;
    }
    LogicalMapping src_mapping = GetLogicalMapping(entry.src);
    PixelSpan      src_x = MappedSpan(entry.src_rect.x, entry.src_rect.w, src_mapping.scale_x, src_mapping.offset_x,
                                      src_info->pixel_width);
    PixelSpan      src_y = MappedSpan(entry.src_rect.y, entry.src_rect.h, src_mapping.scale_y, src_mapping.offset_y,
                                      src_info->pixel_height);
    // A blit stretches whatever the source spans; a copy needs a source at least as large as the destination.
    bool copy = entry.kind == CopyEntry::Copy;
    int32_t need_x = copy ? static_cast<int32_t>(info.pixel_width) : 1;
    int32_t need_y = copy ? static_cast<int32_t>(info.pixel_height) : 1;
    return src_x.high - src_x.low >= need_x && src_y.high - src_y.low >= need_y;
}

// dst's twin, created to its shape. Its contents are copied from dst on the first grab unless that
// grab overwrites them all.
TextureHandle ActivateGrabTwin(TextureHandle dst, bool needs_contents) {
    std::optional<TextureInfo> info = GetTextureInfo(dst);
    if (!info || dst == kMainTarget || dst == kPreviousFrame || info->shares_main_depth) {
        return kNullTexture;
    }
    auto it = g_grab_twins.find(dst);
    if (it != g_grab_twins.end() && (!SameShape(it->second.shape, *info) || !GetTextureInfo(it->second.twin))) {
        DestroyTexture(it->second.twin);
        g_grab_twins.erase(it);
        it = g_grab_twins.end();
    }
    if (it == g_grab_twins.end()) {
        TextureHandle twin =
            info->render_target
                ? CreateRenderTarget(info->width, info->height, info->has_alpha, false, info->frame_target)
                : CreateTexture({info->width, info->height, info->format, info->mip_levels, info->has_alpha});
        if (twin == kNullTexture) {
            return kNullTexture;
        }
        it = g_grab_twins.emplace(dst, GrabTwin{twin, *info, false}).first;
    }
    if (!it->second.active) {
        if (needs_contents) {
            CopyTexture(dst, Rect{0, 0, static_cast<int32_t>(info->width), static_cast<int32_t>(info->height)},
                        it->second.twin, 0, 0);
            g.counters.grab_precopies++;
        }
        it->second.active = true;
    }
    return it->second.twin;
}

void GrabAgain(const CopyEntry &entry) {
    TextureHandle src = GrabbedTwin(entry.src);
    if (entry.src != kMainTarget && src == entry.src) {
        return;
    }
    std::optional<TextureInfo> info = GetTextureInfo(entry.dst);
    bool covers = info && CoversDestination(entry, *info);
    // A self-grab reads the twin it writes, so the twin needs the destination's contents.
    TextureHandle dst = ActivateGrabTwin(entry.dst, entry.src == entry.dst || !covers);
    if (dst == kNullTexture) {
        return;
    }
    if (entry.src == entry.dst) {
        src = dst;
    }
    if (entry.kind == CopyEntry::Copy) {
        CopyTexture(src, entry.src_rect, dst, entry.dst_rect.x, entry.dst_rect.y);
    } else {
        BlitTexture(src, entry.src_rect, dst, entry.dst_rect, entry.filter);
    }
}

TextureBinding GrabbedBinding(const TextureBinding &binding, bool canonical) {
    TextureBinding out = binding;
    if (!canonical) {
        out.texture = GrabbedTwin(binding.texture);
    }
    return out;
}

// Geometry status is deliberately separate from tick-exact fog, colour and texture inputs.
uint32_t MeshTint(const MeshEntry &mesh, const DisplayList &list, const Overrides *overrides) {
    if (overrides == nullptr || !list.records[mesh.record].has_transform) {
        return 2; // newest only
    }
    if (list.cache.match[mesh.record] >= 0) {
        return 1; // matched, including unchanged geometry under the display camera
    }
    return overrides->index[mesh.record] >= 0 || overrides->prog0_index[mesh.record] >= 0 ||
                   overrides->palette_index[mesh.record] >= 0
               ? 5
               : 2;
}

const char *MeshReason(const MeshEntry &mesh, const DisplayList &list, const DisplayList *previous,
                       const Overrides *overrides) {
    const MeshRecord &record = list.records[mesh.record];
    if (!record.has_transform) {
        return "no_transform";
    }
    if (overrides == nullptr) {
        return list.cut ? "list_cut" : "no_interpolation_pair";
    }
    if (record.no_interpolation) {
        return "excluded_pose";
    }
    if (record.key == 0) {
        return "no_key";
    }
    if (list.cache.match[mesh.record] >= 0) {
        return "matched";
    }
    if (g_instanced_keys.contains(record.key)) {
        return "unmatched_instance_surface";
    }
    bool key_seen = false;
    for (const MeshRecord &old : previous->records) {
        if (old.key != record.key) {
            continue;
        }
        key_seen = true;
        if (old.occurrence != record.occurrence) {
            continue;
        }
        if (Moved(old.model, record.model) > record.teleport_distance) {
            return "teleport";
        }
        return "surface_changed";
    }
    return key_seen ? "missing_occurrence" : "unmatched";
}

void TraceInterpolation(const DisplayList &list, const DisplayList *previous, const Overrides *overrides,
                        float alpha) {
    const char *path = std::getenv("KZ_INTERPOLATION_TRACE");
    if (path == nullptr || *path == '\0' || alpha != 0.0f) {
        return;
    }
    FILE *file = std::fopen(path, "a");
    if (file == nullptr) {
        return;
    }
    std::fprintf(file, "frame,%llu,previous,%llu,cameras,%zu,cut,%d,camera_cut,%d\n",
                 static_cast<unsigned long long>(list.serial),
                 static_cast<unsigned long long>(previous ? previous->serial : 0),
                 list.cameras.size(), list.cut, list.camera_cut);
    std::fprintf(file, "entry,kind,status,reason,key,occurrence,match,camera,mesh,texture,palette,blend,depth,fog,fog_changed,material_changed,projection_changed,local_changed,prog0_flags,x,y,z,x0,y0,x1,y1,target,tint_applied\n");
    TextureHandle target = kMainTarget;
    for (size_t i = 0; i < list.entries.size(); ++i) {
        if (const auto *change = std::get_if<TargetEntry>(&list.entries[i])) {
            target = change->target;
        }
        if (const MeshEntry *mesh = std::get_if<MeshEntry>(&list.entries[i])) {
            const MeshRecord &r = list.records[mesh->record];
            int32_t           match = overrides ? list.cache.match[mesh->record] : -1;
            bool              fog = false, material = false, projection = false, local = false;
            if (match >= 0 && previous) {
                const MeshRecord &old = previous->records[match];
                const auto       &a = std::get<MeshEntry>(previous->entries[old.entry]);
                projection = r.projection != old.projection;
                local = r.local != old.local || r.middle != old.middle;
                fog = std::memcmp(a.state.fog_color, mesh->state.fog_color, 3) != 0;
                if (a.prog0 >= 0 && mesh->prog0 >= 0) {
                    const auto &p = previous->prog0_blocks[a.prog0];
                    const auto &q = list.prog0_blocks[mesh->prog0];
                    fog = fog || std::memcmp(p.fog, q.fog, sizeof p.fog) != 0;
                    material = std::memcmp(p.object_color, q.object_color, sizeof p.object_color) != 0 ||
                               std::memcmp(p.ambient, q.ambient, sizeof p.ambient) != 0 ||
                               std::memcmp(p.diffuse, q.diffuse, sizeof p.diffuse) != 0;
                } else {
                    fog = fog || std::memcmp(a.constants.fog, mesh->constants.fog, sizeof a.constants.fog) != 0;
                    material = std::memcmp(a.constants.diffuse, mesh->constants.diffuse, sizeof a.constants.diffuse) != 0;
                }
            }
            std::fprintf(file, "%zu,%s,%u,%s,%llu,%u,%d,%u,%u,%u,%u,%d,%d,%d,%d,%d,%d,%d,%u,%.6g,%.6g,%.6g,,,,,%u,%d\n",
                         i, mesh->prog0 >= 0 ? "prog0" : "mesh", MeshTint(*mesh, list, overrides),
                         MeshReason(*mesh, list, previous, overrides), static_cast<unsigned long long>(r.key),
                         r.occurrence, match, r.camera, mesh->mesh, mesh->binding.texture, mesh->binding.palette,
                         mesh->state.blend, static_cast<int>(mesh->state.depth_test), mesh->state.fog,
                         fog, material, projection, local,
                         mesh->prog0 >= 0 ? list.prog0_blocks[mesh->prog0].flags : 0,
                         r.model[12], r.model[13], r.model[14], target, target == kMainTarget);
        } else if (const Draw2DEntry *sprite = std::get_if<Draw2DEntry>(&list.entries[i])) {
            bool     carried = overrides && overrides->sprites.contains(sprite);
            uint32_t status = carried ? 4 : (sprite->state.depth_test == DepthTest::Always || sprite->state.screen_space) ? 3
                                                                                                                          : 2;
            float    x0 = INFINITY, y0 = INFINITY, x1 = -INFINITY, y1 = -INFINITY;
            for (const auto &v : sprite->vertices) {
                x0 = std::min(x0, v.x);
                y0 = std::min(y0, v.y);
                x1 = std::max(x1, v.x);
                y1 = std::max(y1, v.y);
            }
            std::fprintf(file, "%zu,2d,%u,%s,0,0,-1,%d,0,%u,%u,%d,%d,%d,0,0,0,0,0,0,0,0,%.6g,%.6g,%.6g,%.6g,%u,%d\n",
                         i, status, carried ? "reprojected_corners" : status == 3     ? (sprite->state.screen_space ? "screen_effect" : "flat_2d")
                                                                  : sprite->scene < 0 ? "missing_sprite_scene"
                                                                                      : "newest_sprite_corners",
                         sprite->scene, sprite->binding.texture, sprite->binding.palette,
                         sprite->state.blend, static_cast<int>(sprite->state.depth_test), sprite->state.fog,
                         x0, y0, x1, y1, target, target == kMainTarget);
        }
    }
    std::fclose(file);
}

void Replay(const DisplayList &list, bool canonical, const Overrides *overrides, bool host = false,
            bool tint = false, InterpKey suppressed_2d_key = 0) {
    AssertDeviceThread(__func__);
    KZ_PROFILE_ZONE("Kazarov Replay");
    tls.replaying = true;
    // Sources from an earlier replay may sit at addresses this one reuses.
    g.bound_constants.mesh_source = nullptr;
    g.bound_constants.prog0_source = nullptr;
    tls.quiet = !canonical;
    tls.host_draws = host;
    const Rewrites *rewrites = nullptr;
    if (!canonical) {
        ResetGrabTwins();
        rewrites = &CachedRewrites(list);
    }
    for (const Entry &entry : list.entries) {
        tls.interpolation_tint = 0;
        std::visit(
            [&](const auto &e) {
                using T = std::decay_t<decltype(e)>;
                if constexpr (std::is_same_v<T, Draw2DEntry>) {
                    if (!canonical && suppressed_2d_key != 0 && e.target == kMainTarget &&
                        e.interpolation.key == suppressed_2d_key) {
                        return;
                    }
                    const std::vector<Vertex2D> *vertices = &e.vertices;
                    const DrawState             *state = &e.state;
                    UiSide                       side = e.side;
                    if (!canonical && !e.presentation_vertices.empty()) {
                        vertices = &e.presentation_vertices;
                        state = &e.presentation_state;
                        // Composite replacements are in target space, independent of the original UI box.
                        side = {};
                    } else if (overrides != nullptr) {
                        if (auto it = overrides->sprites.find(&e); it != overrides->sprites.end()) {
                            vertices = &it->second;
                        }
                    }
                    if (tint && !canonical && CurrentRenderTarget() == kMainTarget) {
                        tls.interpolation_tint = overrides && overrides->sprites.contains(&e) ? 4 : (e.state.depth_test == DepthTest::Always || e.state.screen_space) ? 3
                                                                                                                                                                      : 2;
                    }
                    Draw2DSided(e.primitive, *vertices, GrabbedBinding(e.binding, canonical), *state, side);
                } else if constexpr (std::is_same_v<T, MeshEntry>) {
                    if (tint && !canonical && CurrentRenderTarget() == kMainTarget) {
                        tls.interpolation_tint = MeshTint(e, list, overrides);
                    }
                    const MeshConstants *constants = &e.constants;
                    if (overrides != nullptr && overrides->index[e.record] >= 0) {
                        constants = &overrides->constants[overrides->index[e.record]];
                    }
                    const Prog0Constants *prog0 =
                        e.prog0 >= 0 ? &list.prog0_blocks[static_cast<size_t>(e.prog0)] : nullptr;
                    if (prog0 != nullptr && overrides != nullptr && overrides->prog0_index[e.record] >= 0) {
                        prog0 = &overrides->prog0[static_cast<size_t>(overrides->prog0_index[e.record])];
                    }
                    const Prog0Palette *palette =
                        e.palette >= 0 ? &list.palettes[static_cast<size_t>(e.palette)] : nullptr;
                    if (palette != nullptr && overrides != nullptr && overrides->palette_index[e.record] >= 0) {
                        palette = &overrides->palettes[static_cast<size_t>(overrides->palette_index[e.record])];
                    }
                    TextureBinding binding = GrabbedBinding(e.binding, canonical);
                    if (e.mesh != kNullMesh) {
                        if (prog0 != nullptr) {
                            DrawMeshProg0(e.mesh, e.first_index, e.index_count, *constants, *prog0, binding, e.state,
                                          nullptr, palette);
                        } else {
                            DrawMesh(e.mesh, e.first_index, e.index_count, *constants, binding, e.state);
                        }
                    } else {
                        const std::vector<Vertex3D> *vertices = &e.vertices;
                        if (overrides != nullptr && overrides->vertex_index[e.record] >= 0) {
                            vertices = &overrides->vertices[overrides->vertex_index[e.record]];
                        }
                        if (prog0 != nullptr) {
                            DrawMeshImmediateProg0(*vertices, e.indices, *constants, *prog0, binding, e.state,
                                                   nullptr, palette);
                        } else {
                            DrawMeshImmediate(*vertices, e.indices, *constants, binding, e.state);
                        }
                    }
                } else if constexpr (std::is_same_v<T, ClearEntry>) {
                    if (e.stencil) {
                        ClearStencil(e.stencil_value, e.has_rect ? &e.rect : nullptr);
                    } else {
                        const LogicalRect *rect = !canonical && e.has_presentation_rect
                                                      ? &e.presentation_rect
                                                  : e.has_rect ? &e.rect
                                                               : nullptr;
                        // Keep the caller's scratch clear even when the copy is bounded. A
                        // shifted, filtered outline copy samples beyond its scissor's texels;
                        // narrowing the clear to that scissor leaks the previous part's image.
                        Clear(e.color, e.rgba, e.depth, e.z, rect);
                    }
                } else if constexpr (std::is_same_v<T, TargetEntry>) {
                    // A display replay draws into a target's twin, leaving the canonical contents for later ticks.
                    TextureHandle target = e.target;
                    if (!canonical) {
                        TextureHandle twin = ActivateGrabTwin(target, true);
                        if (twin != kNullTexture) {
                            target = twin;
                        }
                    }
                    SetRenderTarget(target);
                } else if (canonical) {
                    if constexpr (std::is_same_v<T, CopyEntry>) {
                        if (e.kind == CopyEntry::Copy) {
                            CopyTexture(e.src, e.src_rect, e.dst, e.dst_rect.x, e.dst_rect.y);
                        } else {
                            BlitTexture(e.src, e.src_rect, e.dst, e.dst_rect, e.filter);
                        }
                    } else if constexpr (std::is_same_v<T, UpdateTextureEntry>) {
                        UpdateTexture(e.texture, e.mip, e.x, e.y, e.w, e.h, e.pixels.data());
                    } else if constexpr (std::is_same_v<T, UpdateMeshEntry>) {
                        UpdateMeshVertices(e.mesh, e.first, e.vertices);
                    } else if constexpr (std::is_same_v<T, ReadDepthEntry>) {
                        ReadDepth(e.id, e.rect.x, e.rect.y, e.rect.w, e.rect.h);
                    }
                } else if constexpr (std::is_same_v<T, CopyEntry>) {
                    GrabAgain(e);
                } else if constexpr (std::is_same_v<T, UpdateMeshEntry>) {
                    // A range rewritten more than once holds the tick's last rewrite, so each takes its own.
                    const std::vector<Vertex3D> *vertices = nullptr;
                    if (overrides != nullptr) {
                        if (auto pose = overrides->poses.find(&e); pose != overrides->poses.end()) {
                            vertices = &pose->second;
                        }
                    }
                    if (vertices == nullptr && rewrites->repeated.contains(RewriteKey(e))) {
                        vertices = &e.vertices;
                    }
                    if (vertices != nullptr) {
                        UpdateMeshVertices(e.mesh, e.first, *vertices);
                    }
                }
            },
            entry);
    }
    // The meshes hold the tick's pose again for whoever draws them next.
    if (!canonical && overrides != nullptr) {
        for (const auto &[update, pose] : overrides->poses) {
            if (rewrites->last.at(RewriteKey(*update)) == update) {
                UpdateMeshVertices(update->mesh, update->first, update->vertices);
            }
        }
    }
    tls.interpolation_tint = 0;
    tls.replaying = false;
    tls.quiet = false;
    tls.host_draws = false;
}

} // namespace

Mat4 Multiply(const Mat4 &a, const Mat4 &b) {
    Mat4 out;
    for (int c = 0; c < 4; c++) {
        for (int r = 0; r < 4; r++) {
            out[c * 4 + r] = a[0 * 4 + r] * b[c * 4 + 0] + a[1 * 4 + r] * b[c * 4 + 1] +
                             a[2 * 4 + r] * b[c * 4 + 2] + a[3 * 4 + r] * b[c * 4 + 3];
        }
    }
    return out;
}

bool InvertAffine(const Mat4 &m, Mat4 &out) {
    if (!IsAffine(m)) {
        return false;
    }
    Vec3  c0 = Column(m, 0);
    Vec3  c1 = Column(m, 1);
    Vec3  c2 = Column(m, 2);
    Vec3  r0 = Cross(c1, c2);
    Vec3  r1 = Cross(c2, c0);
    Vec3  r2 = Cross(c0, c1);
    float determinant = Dot(c0, r0);
    if (!(std::fabs(determinant) > 1e-12f)) {
        return false;
    }
    float inverse = 1.0f / determinant;
    r0 = Scale(r0, inverse);
    r1 = Scale(r1, inverse);
    r2 = Scale(r2, inverse);
    // The rows of the inverse are r0, r1, r2.
    out = kIdentity;
    Vec3 rows[3] = {r0, r1, r2};
    Vec3 t = Column(m, 3);
    for (int r = 0; r < 3; r++) {
        out[0 * 4 + r] = rows[r].x;
        out[1 * 4 + r] = rows[r].y;
        out[2 * 4 + r] = rows[r].z;
        out[3 * 4 + r] = -Dot(rows[r], t);
    }
    return true;
}

bool InterpolateAffine(const Mat4 &a, const Mat4 &b, float alpha, Mat4 &out) {
    return Interpolate(a, b, alpha, out, nullptr);
}

bool Interpolate2D(const Draw2DEntry &before, const Draw2DEntry &after, float alpha, std::vector<Vertex2D> &out) {
    out = after.vertices;
    if (!(alpha < 1.0f) || after.interpolation.key == 0 || before.interpolation.key != after.interpolation.key ||
        before.interpolation.generation != after.interpolation.generation || before.occurrence != after.occurrence ||
        before.primitive != after.primitive || before.target != after.target || before.binding != after.binding ||
        before.state != after.state || before.side != after.side || before.scene != -1 || after.scene != -1 ||
        before.interpolation.world_corners || after.interpolation.world_corners ||
        after.state.depth_test != DepthTest::Always || after.state.depth_write || before.vertices.size() != out.size()) {
        return false;
    }
    for (size_t i = 0; i < out.size(); i++) {
        const auto &a = before.vertices[i];
        const auto &b = after.vertices[i];
        if (a.u != b.u || a.v != b.v || a.z != b.z || a.fog != b.fog ||
            !std::isfinite(a.x) || !std::isfinite(a.y) || !std::isfinite(b.x) || !std::isfinite(b.y)) {
            return false;
        }
    }
    alpha = std::max(alpha, 0.0f);
    for (size_t i = 0; i < out.size(); i++) {
        const auto &a = before.vertices[i];
        const auto &b = after.vertices[i];
        out[i].x = a.x + (b.x - a.x) * alpha;
        out[i].y = a.y + (b.y - a.y) * alpha;
        out[i].color[3] = static_cast<uint8_t>(std::lround(a.color[3] + (float(b.color[3]) - a.color[3]) * alpha));
    }
    return true;
}

bool InterpolateSprite(const Draw2DEntry &before, const SpriteScene &before_scene, const Mat4 &before_view,
                       const Draw2DEntry &after, const SpriteScene &after_scene, const Mat4 &after_view,
                       const Mat4 &display_view, float alpha, std::vector<Vertex2D> &out) {
    out = after.vertices;
    if (!(alpha < 1.0f) || !before.interpolation.world_corners || !after.interpolation.world_corners ||
        after.interpolation.key == 0 || before.interpolation.key != after.interpolation.key ||
        before.interpolation.generation != after.interpolation.generation || before.occurrence != after.occurrence ||
        before.scene < 0 || after.scene < 0 || before.primitive != after.primitive || before.target != after.target ||
        before.binding != after.binding || before.state != after.state || before.side != after.side ||
        after.state.depth_test == DepthTest::Always || before.vertices.size() != after.vertices.size() ||
        before_scene.projection != after_scene.projection ||
        std::memcmp(before_scene.to_clip, after_scene.to_clip, sizeof after_scene.to_clip) != 0) {
        return false;
    }

    Mat4 old_inverse, new_inverse;
    if (!Invert(Multiply(before_scene.projection, before_view), old_inverse) ||
        !Invert(Multiply(after_scene.projection, after_view), new_inverse)) {
        return false;
    }
    const Mat4 project = Multiply(after_scene.projection, display_view);
    auto       transform = [](const Mat4 &matrix, const std::array<float, 4> &point, std::array<float, 4> &result) {
        for (int r = 0; r < 4; ++r) {
            result[r] = 0;
            for (int c = 0; c < 4; ++c) {
                result[r] += matrix[c * 4 + r] * point[c];
            }
        }
        if (!(result[3] > 1e-6f)) {
            return false;
        }
        for (int r = 0; r < 3; ++r) {
            result[r] /= result[3];
            if (!std::isfinite(result[r])) {
                return false;
            }
        }
        result[3] = 1;
        return true;
    };
    auto unproject = [&](const Vertex2D &vertex, const Mat4 &inverse, std::array<float, 4> &world) {
        if (!(vertex.z >= 0 && vertex.z <= 1)) {
            return false;
        }
        return transform(inverse, {vertex.x * after_scene.to_clip[0] + after_scene.to_clip[2], vertex.y * after_scene.to_clip[1] + after_scene.to_clip[3], vertex.z, 1}, world);
    };
    if (after_scene.to_clip[0] == 0 || after_scene.to_clip[1] == 0) {
        return false;
    }
    alpha = std::max(alpha, 0.0f);
    auto blended = after.vertices;
    for (size_t i = 0; i < blended.size(); ++i) {
        std::array<float, 4> a, b, point, clip;
        if (!unproject(before.vertices[i], old_inverse, a) || !unproject(after.vertices[i], new_inverse, b)) {
            return false;
        }
        float distance_squared = 0;
        for (int r = 0; r < 3; ++r) {
            const float delta = b[r] - a[r];
            distance_squared += delta * delta;
            point[r] = a[r] + delta * alpha;
        }
        if (!(std::sqrt(distance_squared) <= after.interpolation.teleport_distance)) {
            return false;
        }
        point[3] = 1;
        if (!transform(project, point, clip) || !(clip[2] >= 0 && clip[2] <= 1)) {
            return false;
        }
        blended[i].x = (clip[0] - after_scene.to_clip[2]) / after_scene.to_clip[0];
        blended[i].y = (clip[1] - after_scene.to_clip[3]) / after_scene.to_clip[1];
        blended[i].z = clip[2];
        blended[i].color[3] = static_cast<uint8_t>(std::lround(before.vertices[i].color[3] +
                                                               (float(after.vertices[i].color[3]) - before.vertices[i].color[3]) * alpha));
    }
    out = std::move(blended);
    return true;
}

void BlendProg0Bones(const Prog0Palette &before, const Prog0Palette &after, float alpha, float teleport_distance,
                     Prog0Palette &out) {
    out = after;
    for (uint32_t slot = 0; slot < kProg0Bones; slot++) {
        if (std::memcmp(before.bones[slot], after.bones[slot], sizeof after.bones[slot]) == 0) {
            continue;
        }
        Mat4 a = BoneMatrix(before.bones[slot]);
        Mat4 b = BoneMatrix(after.bones[slot]);
        float dx = b[12] - a[12];
        float dy = b[13] - a[13];
        float dz = b[14] - a[14];
        if (std::sqrt(dx * dx + dy * dy + dz * dz) > teleport_distance) {
            continue;
        }
        Mat4 blended = {};
        Interpolate(a, b, alpha, blended, nullptr);
        SetBoneRows(blended, out.bones[slot]);
    }
}

bool RecordingCalls() { return !tls.replaying && !OnWorkerThread() && g.list != nullptr; }

const DisplayList *RecordingList() { return g_recording_list.load(); }

void RecordEntry(Entry &&entry) {
    DisplayList &list = *g.list;
    if (Draw2DEntry *draw = std::get_if<Draw2DEntry>(&entry)) {
        if (!CanRecordBinding(draw->binding)) {
            return;
        }
        NoteMainDraw(list);
        if (draw->state.depth_test != DepthTest::Always && !draw->state.screen_space) {
            draw->scene = list.scene;
        }
        draw->target = g.record_target;
        draw->interpolation = g.interp_2d;
        if (draw->interpolation.key != 0) {
            draw->occurrence = list.occurrences_2d[draw->interpolation.key]++;
            list.tagged_2d.push_back(static_cast<uint32_t>(list.entries.size()));
        }
    } else if (const TargetEntry *target = std::get_if<TargetEntry>(&entry)) {
        auto it = list.target_scenes.find(target->target);
        list.scene = it != list.target_scenes.end() ? it->second : -1;
    }
    list.entries.push_back(std::move(entry));
}

// Appends value to pool, or reuses the last element when it is identical: consecutive draws often share a block.
template <typename T>
int32_t PoolIndex(std::vector<T> &pool, const T &value) {
    if (pool.empty() || std::memcmp(&pool.back(), &value, sizeof(T)) != 0) {
        pool.push_back(value);
    }
    return static_cast<int32_t>(pool.size() - 1);
}

void RecordMesh(MeshEntry &&entry, const MeshTransform *transform, const Prog0Constants *prog0,
                const Prog0Palette *palette) {
    if (!CanRecordBinding(entry.binding)) {
        return;
    }
    DisplayList &list = *g.list;
    NoteMainDraw(list);
    MeshRecord record;
    record.key = g.interp_key;
    record.no_interpolation = g.interp_no_interpolation;
    record.teleport_distance = g.interp_teleport_distance;
    record.blend_vertices = g.interp_blend_vertices;
    if (record.key != 0) {
        record.occurrence = list.occurrences[record.key]++;
    }
    if (transform != nullptr) {
        record.has_transform = true;
        record.projection = ToMat4(transform->projection);
        record.middle = ToMat4(transform->middle);
        record.model = ToMat4(transform->model);
        record.local = ToMat4(transform->local);
        record.camera = CameraIndex(list, transform->view);
        NoteScene(list, record);
    }
    if (prog0 != nullptr) {
        entry.prog0 = PoolIndex(list.prog0_blocks, *prog0);
        if (palette != nullptr) {
            entry.palette = PoolIndex(list.palettes, *palette);
        }
    }
    entry.record = static_cast<uint32_t>(list.records.size());
    record.entry = static_cast<uint32_t>(list.entries.size());
    list.records.push_back(record);
    list.entries.push_back(std::move(entry));
}

void RecordClear(const ClearEntry &entry) {
    DisplayList &list = *g.list;
    if (!entry.stencil && entry.color && g.record_target == kMainTarget &&
        (!entry.has_rect || CoversLogicalFrame(entry.rect))) {
        list.main_cleared = true;
    }
    list.entries.push_back(entry);
}

void RecordDoomedTexture(TextureHandle handle) { g.list->doomed_textures.push_back(handle); }

void RecordDoomedMesh(MeshHandle handle) { g.list->doomed_meshes.push_back(handle); }

} // namespace detail

using namespace detail;

DisplayList::~DisplayList() {
    if (instance != g.renderer_instance || g.device == VK_NULL_HANDLE) {
        return;
    }
    if (doomed_textures.empty() && doomed_meshes.empty()) {
        return;
    }
    RunOnRenderThread([this] {
        for (TextureHandle texture : doomed_textures) {
            DestroyDoomedTexture(texture);
        }
        for (MeshHandle mesh : doomed_meshes) {
            DestroyDoomedMesh(mesh);
        }
    });
}

MeshTransform IdentityMeshTransform() {
    MeshTransform transform;
    for (float *m :
         {transform.projection, transform.view, transform.middle, transform.model, transform.local}) {
        std::memcpy(m, kIdentity.data(), sizeof(float) * 16);
    }
    return transform;
}

bool InvertAffineTransform(const float matrix[16], float inverse[16]) {
    Mat4 out;
    if (!InvertAffine(ToMat4(matrix), out)) {
        return false;
    }
    std::memcpy(inverse, out.data(), sizeof(float) * 16);
    return true;
}

void SetInterpKey(InterpKey key, bool no_interpolation, float teleport_distance, bool blend_vertices) {
    g.interp_key = key;
    g.interp_no_interpolation = no_interpolation;
    g.interp_teleport_distance = teleport_distance;
    g.interp_blend_vertices = blend_vertices;
}

InterpKey CurrentInterpKey() { return g.interp_key; }

bool CurrentNoInterpolation() { return g.interp_no_interpolation; }

void SetInterp2D(Interp2D interpolation) { g.interp_2d = interpolation; }

Interp2D CurrentInterp2D() { return g.interp_2d; }

void BeginRecording(RecordingKind kind) {
    if (g.list != nullptr) {
        return;
    }
    if (GameFrameOpen()) {
        Error("BeginRecording inside a frame");
        return;
    }
    PrepareRecordingFromCaller();
    g.list = std::make_shared<DisplayList>();
    g_recording_list.store(g.list.get());
    g_recording_kind = kind == RecordingKind::Overlay ? 1 : 0;
    const auto &sizes = g_recording_sizes[g_recording_kind];
    if (sizes.instance == g.renderer_instance) {
        g.list->entries.reserve(sizes.entries);
        g.list->records.reserve(sizes.records);
        g.list->prog0_blocks.reserve(sizes.prog0);
    }
    g.list->instance = g.renderer_instance;
    g.list->serial = ++g.list_serial;
    g.record_target = kMainTarget;
    g.interp_key = 0;
    g.interp_2d = {};
    g.interp_no_interpolation = false;
    g.interp_teleport_distance = INFINITY;
    g.interp_blend_vertices = false;
}

DisplayListRef EndRecording() {
    std::shared_ptr<DisplayList> list = std::move(g.list);
    g.list = nullptr;
    g_recording_list.store(nullptr);
    if (list != nullptr) {
        list->cut_generation = g_cut_generation;
        // Leave the usual geometric-growth slack for small visibility changes.
        // Derive it from size, so a smaller scene does not retain a high-water mark.
        auto capacity = [](size_t size) { return size == 0 ? size_t{0} : std::bit_ceil(size); };
        g_recording_sizes[g_recording_kind] = {list->instance, capacity(list->entries.size()), capacity(list->records.size()),
                                               capacity(list->prog0_blocks.size())};
    }
    return list;
}

bool Recording() { return g.list != nullptr; }

void CutInterpolation() {
    if (g.list != nullptr) {
        g.list->cut = true;
    }
    ++g_cut_generation;
}

void CutCameraInterpolation() {
    if (g.list != nullptr) {
        g.list->camera_cut = true;
    }
}

bool RenderList(const DisplayList &list, float alpha, const RenderOptions &options) {
    KZ_PROFILE_ZONE("Kazarov RenderList");
    if (list.instance != g.renderer_instance || g.device == VK_NULL_HANDLE) {
        return false;
    }
    // Read on the game thread: the worker must not look at the list the game thread records.
    if (GameFrameOpen() || RecordingCalls()) {
        Error("RenderList inside a frame");
        return false;
    }
    bool rendered = false;
    RunOnRenderThread([&] {
        if (options.canonical) {
            OpenListFrame(true, false, false);
            Replay(list, true, nullptr);
            CloseListFrame(true, false);
            rendered = true;
            return;
        }
        if (!g.canonical_newest) {
            return;
        }
        // Reused across frames for its capacity; RenderList runs on one thread.
        static Overrides overrides;
        overrides.Clear();
        bool interpolate = options.previous != nullptr && options.previous->instance == g.renderer_instance;
        interpolate = interpolate && alpha < 1.0f && !list.cut;
        if (interpolate) {
            KZ_PROFILE_ZONE("Kazarov interpolate");
            Interpolated(list, *options.previous, std::max(alpha, 0.0f), overrides);
        }
        if (!OpenListFrame(false, options.present, list.needs_base)) {
            return;
        }
        static const bool env_tint = [] {
            const char *setting = std::getenv("KZ_INTERPOLATION_TINT");
            return setting && std::strcmp(setting, "1") == 0;
        }();
        bool tint = options.interpolation_tint || env_tint;
        if (tint) {
            TraceInterpolation(list, options.previous, interpolate ? &overrides : nullptr, alpha);
        }
        Replay(list, false, interpolate ? &overrides : nullptr, options.host, tint, options.suppressed_2d_key);
        if (options.overlay != nullptr && options.overlay->instance == g.renderer_instance) {
            KZ_PROFILE_ZONE("Kazarov overlay");
            SetRenderTarget(kMainTarget);
            Replay(*options.overlay, false, nullptr, true, tint);
        }
        CloseListFrame(false, options.present);
        rendered = true;
    });
    return rendered;
}

DisplayListStats ListStats(const DisplayList &list) {
    DisplayListStats stats = {};
    for (const Entry &entry : list.entries) {
        if (std::holds_alternative<Draw2DEntry>(entry)) {
            stats.draws_2d++;
        } else if (const MeshEntry *mesh = std::get_if<MeshEntry>(&entry)) {
            stats.mesh_draws++;
            if (list.records[mesh->record].key != 0) {
                stats.keyed_mesh_draws++;
            }
        } else if (!std::holds_alternative<ClearEntry>(entry) &&
                   !std::holds_alternative<TargetEntry>(entry)) {
            stats.stateful++;
        }
    }
    stats.cameras = static_cast<uint32_t>(list.cameras.size());
    return stats;
}

} // namespace gfx
