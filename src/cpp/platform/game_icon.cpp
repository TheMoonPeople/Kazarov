#include "platform/game_icon.hpp"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define RECT WindowsRect
#include <objbase.h>
#include <shobjidl.h>
#include <windows.h>
#undef RECT
#endif

#include <fastgltf/core.hpp>
#include <fastgltf/tools.hpp>
#include <stb_image.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <system_error>
#include <type_traits>

#include "kazarov/log.hpp"
#include "platform/paths.hpp"
#include "platform/window.hpp"

namespace fs = std::filesystem;

namespace {

constexpr int kSupersample = 4;

/**
 * One transformed triangle corner and its material texture.
 */
struct Vertex {
    float                pos[3];                /**< Position in the standard glTF scene. */
    float                normal[3];             /**< Surface normal in the standard glTF scene. */
    float                uv[2];                 /**< Texture coordinates. */
    std::size_t          texture = 0;           /**< Texture used by the triangle. */
    std::array<float, 4> factor = {1, 1, 1, 1}; /**< Standard material base colour. */
};

/**
 * An RGBA image used by a glTF material.
 */
struct Texture {
    int                       width = 1;                     /**< Horizontal texel count. */
    int                       height = 1;                    /**< Vertical texel count. */
    std::vector<std::uint8_t> pixels = {255, 255, 255, 255}; /**< RGBA texels. */
};

/**
 * Expanded standard glTF triangles and material images.
 */
struct Model {
    std::vector<Vertex>  vertices;            /**< Triangle corners in drawing order. */
    std::vector<Texture> textures{Texture{}}; /**< Material images, starting with white. */
};

/**
 * Decodes standard glTF scene geometry and embedded images for icon rendering.
 */
bool Decode(const std::uint8_t *data, std::size_t size, Model &model, const fs::path &base = {}) {
    try {
        auto input = fastgltf::GltfDataBuffer::FromBytes(reinterpret_cast<const std::byte *>(data), size);
        if (input.error() != fastgltf::Error::None) {
            return false;
        }
        fastgltf::Parser parser;
        const auto       options = base.empty() ? fastgltf::Options::None : fastgltf::Options::LoadExternalBuffers | fastgltf::Options::LoadExternalImages;
        auto             parsed = parser.loadGltf(input.get(), base, options);
        if (parsed.error() != fastgltf::Error::None || fastgltf::validate(parsed.get()) != fastgltf::Error::None) {
            return false;
        }
        const auto &asset = parsed.get();
        for (const auto &buffer : asset.buffers) {
            if (std::holds_alternative<fastgltf::sources::URI>(buffer.data)) {
                return false;
            }
        }
        Model result;
        for (const auto &image : asset.images) {
            std::vector<unsigned char> bytes;
            std::visit([&](const auto &source) {
                using T = std::decay_t<decltype(source)>;
                if constexpr (std::is_same_v<T, fastgltf::sources::Array> || std::is_same_v<T, fastgltf::sources::Vector> || std::is_same_v<T, fastgltf::sources::ByteView>) {
                    bytes.resize(source.bytes.size());
                    std::memcpy(bytes.data(), source.bytes.data(), bytes.size());
                } else if constexpr (std::is_same_v<T, fastgltf::sources::BufferView>) {
                    const auto view = fastgltf::DefaultBufferDataAdapter{}(asset, source.bufferViewIndex);
                    bytes.resize(view.size());
                    std::memcpy(bytes.data(), view.data(), view.size());
                }
            },
                       image.data);
            Texture texture;
            int     components = 0;
            auto   *pixels = stbi_load_from_memory(bytes.data(), static_cast<int>(bytes.size()), &texture.width, &texture.height, &components, 4);
            if (!pixels) {
                return false;
            }
            texture.pixels.assign(pixels, pixels + std::size_t(texture.width) * texture.height * 4);
            stbi_image_free(pixels);
            result.textures.push_back(std::move(texture));
        }
        std::vector<bool>                                                 active(asset.nodes.size());
        std::function<bool(std::size_t, const fastgltf::math::fmat4x4 &)> visit;
        visit = [&](std::size_t index, const fastgltf::math::fmat4x4 &parent) {
            if (index >= asset.nodes.size() || active[index]) {
                return false;
            }
            active[index] = true;
            const auto &node = asset.nodes[index];
            const auto  world = parent * fastgltf::getLocalTransformMatrix(node);
            const auto  normal_matrix = fastgltf::math::transpose(fastgltf::math::inverse(world));
            if (node.meshIndex) {
                for (const auto &primitive : asset.meshes.at(*node.meshIndex).primitives) {
                    const auto positions = primitive.findAttribute("POSITION");
                    if (positions == primitive.attributes.end()) {
                        return false;
                    }
                    const auto           normals = primitive.findAttribute("NORMAL");
                    const auto           uvs = primitive.findAttribute("TEXCOORD_0");
                    std::size_t          texture = 0;
                    std::array<float, 4> factor{1, 1, 1, 1};
                    if (primitive.materialIndex) {
                        const auto &material = asset.materials.at(*primitive.materialIndex).pbrData;
                        for (int component = 0; component < 4; ++component) {
                            factor[component] = material.baseColorFactor[component];
                        }
                        if (material.baseColorTexture) {
                            const auto &source = asset.textures.at(material.baseColorTexture->textureIndex);
                            if (source.imageIndex) {
                                texture = *source.imageIndex + 1;
                            }
                        }
                    }
                    const auto &position_accessor = asset.accessors.at(positions->accessorIndex);
                    const auto  count = primitive.indicesAccessor ? asset.accessors.at(*primitive.indicesAccessor).count : position_accessor.count;
                    auto        corner = [&](std::size_t at) {
                        const auto source = primitive.indicesAccessor ? fastgltf::getAccessorElement<std::uint32_t>(asset, asset.accessors.at(*primitive.indicesAccessor), at) : at;
                        if (source >= position_accessor.count) {
                            throw std::runtime_error("invalid glTF icon index");
                        }
                        const auto position = fastgltf::getAccessorElement<fastgltf::math::fvec3>(asset, position_accessor, source);
                        const auto point = world * fastgltf::math::fvec4(position[0], position[1], position[2], 1);
                        auto       normal = fastgltf::math::fvec4(0, 0, 1, 0);
                        if (normals != primitive.attributes.end()) {
                            const auto value = fastgltf::getAccessorElement<fastgltf::math::fvec3>(asset, asset.accessors.at(normals->accessorIndex), source);
                            normal = fastgltf::math::fvec4(value[0], value[1], value[2], 0);
                        }
                        normal = normal_matrix * normal;
                        Vertex vertex{};
                        for (int component = 0; component < 3; ++component) {
                            vertex.pos[component] = point[component];
                            vertex.normal[component] = normal[component];
                        }
                        if (uvs != primitive.attributes.end()) {
                            const auto uv = fastgltf::getAccessorElement<fastgltf::math::fvec2>(asset, asset.accessors.at(uvs->accessorIndex), source);
                            vertex.uv[0] = uv[0];
                            vertex.uv[1] = uv[1];
                        }
                        vertex.texture = texture;
                        vertex.factor = factor;
                        result.vertices.push_back(vertex);
                    };
                    if (primitive.type == fastgltf::PrimitiveType::Triangles) {
                        if (count % 3) {
                            return false;
                        }
                        for (std::size_t at = 0; at < count; ++at) {
                            corner(at);
                        }
                    } else if (primitive.type == fastgltf::PrimitiveType::TriangleStrip || primitive.type == fastgltf::PrimitiveType::TriangleFan) {
                        for (std::size_t at = 2; at < count; ++at) {
                            corner(primitive.type == fastgltf::PrimitiveType::TriangleFan ? 0 : at - 2);
                            corner(at - 1);
                            corner(at);
                        }
                    } else {
                        return false;
                    }
                }
            }
            for (auto child : node.children) {
                if (!visit(child, world)) {
                    return false;
                }
            }
            active[index] = false;
            return true;
        };
        if (asset.scenes.empty()) {
            return false;
        }
        for (auto root : asset.scenes.at(asset.defaultScene.value_or(0)).nodeIndices) {
            if (!visit(root, fastgltf::math::fmat4x4(1))) {
                return false;
            }
        }
        if (result.vertices.empty()) {
            return false;
        }
        model = std::move(result);
        return true;
    } catch (const std::exception &) {
        return false;
    }
}

// Orthographic, two-sided, with one directional light, turned by yaw and then roll (degrees).
std::vector<std::uint8_t> Draw(const Model &model, int size, float yaw_degrees, float roll_degrees) {
    const int   big = size * kSupersample;
    const float pi = 3.14159265358979f;
    const float yaw = yaw_degrees * pi / 180.0f, roll = roll_degrees * pi / 180.0f;
    // Rz(roll) * Ry(yaw)
    const float cy = std::cos(yaw), sy = std::sin(yaw), cr = std::cos(roll), sr = std::sin(roll);
    const float m[3][3] = {
        {cr * cy, -sr,  cr * sy},
        {sr * cy, cr,   sr * sy},
        {-sy,     0.0f, cy     }
    };

    const std::size_t  count = model.vertices.size();
    std::vector<float> px(count), py(count), pz(count), nx(count), ny(count), nz(count);
    float              mean[3] = {};
    for (const Vertex &v : model.vertices) {
        for (int i = 0; i < 3; ++i) {
            mean[i] += v.pos[i] / count;
        }
    }
    float min_x = 1e30f, max_x = -1e30f, min_y = 1e30f, max_y = -1e30f;
    for (std::size_t i = 0; i < count; ++i) {
        const Vertex &v = model.vertices[i];
        const float   p[3] = {v.pos[0] - mean[0], v.pos[1] - mean[1], v.pos[2] - mean[2]};
        float        *out[3] = {&px[i], &py[i], &pz[i]};
        float        *nout[3] = {&nx[i], &ny[i], &nz[i]};
        for (int r = 0; r < 3; ++r) {
            *out[r] = m[r][0] * p[0] + m[r][1] * p[1] + m[r][2] * p[2];
            *nout[r] = m[r][0] * v.normal[0] + m[r][1] * v.normal[1] + m[r][2] * v.normal[2];
        }
        min_x = std::min(min_x, px[i]);
        max_x = std::max(max_x, px[i]);
        min_y = std::min(min_y, py[i]);
        max_y = std::max(max_y, py[i]);
    }
    const float extent = std::max(max_x - min_x, max_y - min_y);
    if (!(extent > 0.0f)) {
        return {};
    }
    const float scale = big * (1.0f - 2.0f * 0.06f) / extent;
    const float center_x = (max_x + min_x) / 2, center_y = (max_y + min_y) / 2;
    for (std::size_t i = 0; i < count; ++i) {
        px[i] = (px[i] - center_x) * scale + big / 2.0f;
        py[i] = big / 2.0f - (py[i] - center_y) * scale;
    }
    float       light[3] = {0.4f, 0.6f, 0.7f};
    const float length = std::sqrt(light[0] * light[0] + light[1] * light[1] + light[2] * light[2]);
    for (float &l : light) {
        l /= length;
    }

    std::vector<float>        depth(std::size_t(big) * big, -1e30f);
    std::vector<std::uint8_t> image(std::size_t(big) * big * 4, 0);
    for (std::size_t t = 0; t + 2 < count; t += 3) {
        const std::size_t a = t, b = t + 1, c = t + 2;
        const float       area = (px[b] - px[a]) * (py[c] - py[a]) - (px[c] - px[a]) * (py[b] - py[a]);
        if (std::fabs(area) < 1e-9f) {
            continue;
        }
        const int x0 = std::max(0, int(std::floor(std::min({px[a], px[b], px[c]}))));
        const int x1 = std::min(big, int(std::ceil(std::max({px[a], px[b], px[c]}))) + 1);
        const int y0 = std::max(0, int(std::floor(std::min({py[a], py[b], py[c]}))));
        const int y1 = std::min(big, int(std::ceil(std::max({py[a], py[b], py[c]}))) + 1);
        for (int y = y0; y < y1; ++y) {
            for (int x = x0; x < x1; ++x) {
                const float gx = x + 0.5f, gy = y + 0.5f;
                const float w0 = ((px[b] - gx) * (py[c] - gy) - (px[c] - gx) * (py[b] - gy)) / area;
                const float w1 = ((px[c] - gx) * (py[a] - gy) - (px[a] - gx) * (py[c] - gy)) / area;
                const float w2 = 1.0f - w0 - w1;
                if (w0 < 0 || w1 < 0 || w2 < 0) {
                    continue;
                }
                const float z = w0 * pz[a] + w1 * pz[b] + w2 * pz[c];
                float      &stored = depth[std::size_t(y) * big + x];
                if (z <= stored) {
                    continue;
                }
                stored = z;
                const Vertex &va = model.vertices[a], &vb = model.vertices[b], &vc = model.vertices[c];
                float         u = w0 * va.uv[0] + w1 * vb.uv[0] + w2 * vc.uv[0];
                float         v = w0 * va.uv[1] + w1 * vb.uv[1] + w2 * vc.uv[1];
                u -= std::floor(u);
                v -= std::floor(v);
                const auto &texture = model.textures[va.texture];
                const int   tx = std::clamp(int(u * texture.width), 0, texture.width - 1);
                const int   ty = std::clamp(int(v * texture.height), 0, texture.height - 1);
                float       n[3] = {w0 * nx[a] + w1 * nx[b] + w2 * nx[c], w0 * ny[a] + w1 * ny[b] + w2 * ny[c],
                                    w0 * nz[a] + w1 * nz[b] + w2 * nz[c]};
                const float nl = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]) + 1e-9f;
                const float lit =
                    0.45f + 0.75f * std::fabs((n[0] * light[0] + n[1] * light[1] + n[2] * light[2]) / nl);
                const auto   *texel = &texture.pixels[(std::size_t(ty) * texture.width + tx) * 4];
                std::uint8_t *out = &image[(std::size_t(y) * big + x) * 4];
                for (int i = 0; i < 3; ++i) {
                    out[i] = std::uint8_t(std::clamp(texel[i] * va.factor[i] * lit, 0.0f, 255.0f));
                }
                out[3] = std::uint8_t(std::clamp(texel[3] * va.factor[3], 0.0f, 255.0f));
            }
        }
    }

    // Box-filter the supersampled image down, weighting colour by coverage so the edges do not darken.
    std::vector<std::uint8_t> result(std::size_t(size) * size * 4, 0);
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            float sum[4] = {};
            for (int sy2 = 0; sy2 < kSupersample; ++sy2) {
                for (int sx = 0; sx < kSupersample; ++sx) {
                    const std::uint8_t *in =
                        &image[((std::size_t(y) * kSupersample + sy2) * big + std::size_t(x) * kSupersample + sx) * 4];
                    const float alpha = in[3] / 255.0f;
                    for (int i = 0; i < 3; ++i) {
                        sum[i] += in[i] * alpha;
                    }
                    sum[3] += alpha;
                }
            }
            std::uint8_t *out = &result[(std::size_t(y) * size + x) * 4];
            if (sum[3] > 0) {
                for (int i = 0; i < 3; ++i) {
                    out[i] = std::uint8_t(std::lround(sum[i] / sum[3]));
                }
                out[3] = std::uint8_t(std::lround(255.0f * sum[3] / (kSupersample * kSupersample)));
            }
        }
    }
    return result;
}

#ifdef _WIN32
// A Windows icon file of 32-bit DIB images; alpha carries the shape, so the AND masks stay clear.
std::vector<std::uint8_t> IcoFile(const std::vector<std::vector<std::uint8_t> > &images, const std::vector<int> &sizes) {
    std::vector<std::uint8_t> out;
    auto                      put16 = [&](std::uint16_t v) { out.insert(out.end(), {std::uint8_t(v), std::uint8_t(v >> 8)}); };
    auto                      put32 = [&](std::uint32_t v) {
        out.insert(out.end(), {std::uint8_t(v), std::uint8_t(v >> 8), std::uint8_t(v >> 16), std::uint8_t(v >> 24)});
    };
    put16(0);
    put16(1);
    put16(std::uint16_t(sizes.size()));
    std::uint32_t offset = 6 + 16 * std::uint32_t(sizes.size());
    for (int size : sizes) {
        const std::uint32_t mask_row = ((size + 31) / 32) * 4;
        const std::uint32_t bytes = 40 + size * size * 4 + mask_row * size;
        out.push_back(std::uint8_t(size >= 256 ? 0 : size));
        out.push_back(std::uint8_t(size >= 256 ? 0 : size));
        out.push_back(0);
        out.push_back(0);
        put16(1);
        put16(32);
        put32(bytes);
        put32(offset);
        offset += bytes;
    }
    for (std::size_t i = 0; i < sizes.size(); ++i) {
        const int size = sizes[i];
        put32(40);
        put32(size);
        put32(size * 2);
        put16(1);
        put16(32);
        put32(0);
        put32(size * size * 4);
        put32(0);
        put32(0);
        put32(0);
        put32(0);
        for (int y = size - 1; y >= 0; --y) {
            for (int x = 0; x < size; ++x) {
                const std::uint8_t *p = &images[i][(std::size_t(y) * size + x) * 4];
                out.insert(out.end(), {p[2], p[1], p[0], p[3]});
            }
        }
        out.insert(out.end(), std::size_t(((size + 31) / 32) * 4) * size, 0);
    }
    return out;
}

std::wstring Quoted(const fs::path &path) { return L"\"" + path.wstring() + L"\""; }

void CreateShortcut(const fs::path &link, const fs::path &target, const std::wstring &arguments, const fs::path &icon,
                    const std::wstring &description) {
    const HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    IShellLinkW  *shell_link = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_IShellLinkW,
                                   reinterpret_cast<void **>(&shell_link)))) {
        shell_link->SetPath(target.c_str());
        shell_link->SetArguments(arguments.c_str());
        shell_link->SetWorkingDirectory(target.parent_path().c_str());
        shell_link->SetIconLocation(icon.c_str(), 0);
        shell_link->SetDescription(description.c_str());
        IPersistFile *file = nullptr;
        if (SUCCEEDED(shell_link->QueryInterface(IID_IPersistFile, reinterpret_cast<void **>(&file)))) {
            if (FAILED(file->Save(link.c_str(), TRUE))) {
                kz::Logf(kz::LogLevel::Warning, "icon", "could not write {}", PathsDisplay(link));
            }
            file->Release();
        }
        shell_link->Release();
    }
    if (SUCCEEDED(init)) {
        CoUninitialize();
    }
}

void InstallWindowsIcon(const fs::path &data_root, const GameIconSource &source, const Model &model) {
    if (source.ico_file == nullptr) {
        return;
    }
    std::error_code error;
    const fs::path  icon = fs::absolute(data_root.parent_path() / source.ico_file, error);
    if (error || fs::exists(icon, error)) {
        return;
    }
    const std::vector<int>                  sizes = {16, 24, 32, 48, 64, 128, 256};
    std::vector<std::vector<std::uint8_t> > images;
    for (int size : sizes) {
        images.push_back(Draw(model, size, source.yaw, source.roll));
        if (images.back().empty()) {
            return;
        }
    }
    const std::vector<std::uint8_t> bytes = IcoFile(images, sizes);
    {
        std::ofstream file(icon, std::ios::binary | std::ios::trunc);
        if (!file.write(reinterpret_cast<const char *>(bytes.data()), std::streamsize(bytes.size()))) {
            return;
        }
    }
    // Explorer shows the executable's own icon, which the build cannot carry; the shortcut can point at
    // the icon drawn here. Made only alongside a new icon so a moved or deleted shortcut stays that way.
    const fs::path exe = PathsExecutable();
    if (source.shortcut == nullptr || exe.empty()) {
        return;
    }
    const fs::path     data = fs::absolute(data_root, error);
    const fs::path     save = fs::absolute(PathsSaveRoot(), error);
    const std::wstring arguments = L"--data " + Quoted(data) + L" --save " + Quoted(save);
    CreateShortcut(exe.parent_path() / (std::wstring(source.shortcut) + L".lnk"), exe, arguments, icon, source.shortcut);
}
#endif

bool LoadModel(const GameIconSource &source, Model &model) {
    if (!source.candidates) {
        return false;
    }
    for (const fs::path &directory : source.packs) {
        const auto relative = directory / source.member;
        if (relative.is_absolute()) {
            continue;
        }
        bool safe = true;
        for (const auto &component : relative) {
            if (component == "..") {
                safe = false;
            }
        }
        if (!safe) {
            continue;
        }
        for (const auto &file : source.candidates(relative)) {
            std::vector<std::uint8_t> bytes;
            const auto                host = vfs::HostPath(file);
            if (vfs::Read(file, bytes) && Decode(bytes.data(), bytes.size(), model, host ? host->parent_path() : fs::path{})) {
                return true;
            }
        }
    }
    return false;
}

} // namespace

std::vector<std::uint8_t> GameIconRender(const std::uint8_t *ico, std::size_t ico_size, int size, float yaw, float roll) {
    Model model;
    if (ico == nullptr || size <= 0 || !Decode(ico, ico_size, model)) {
        return {};
    }
    return Draw(model, size, yaw, roll);
}

std::vector<std::uint8_t> GameIconFromData(const GameIconSource &source, int size) {
    Model model;
    if (size <= 0 || !LoadModel(source, model)) {
        return {};
    }
    return Draw(model, size, source.yaw, source.roll);
}

void GameIconInstall(const fs::path &data_root, const GameIconSource &source) {
    constexpr int                    window_icon_size = 256;
    static std::vector<std::uint8_t> window_icon;
    Model                            model;
    if (!LoadModel(source, model)) {
        return;
    }
    window_icon = Draw(model, window_icon_size, source.yaw, source.roll);
    if (!window_icon.empty()) {
        WindowSetIcon(window_icon.data(), window_icon_size);
    }
#ifdef _WIN32
    InstallWindowsIcon(data_root, source, model);
#endif
}
