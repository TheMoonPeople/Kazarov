#include <SDL3/SDL.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

extern "C" {
std::uint32_t romfsMountSelf(const char *name);
std::uint32_t romfsUnmount(const char *name);
}

#include "platform/window.hpp"
#include "switch_main.hpp"

namespace {

/** Last configured operation-mode output width. */
int output_width = 0;
/** Last configured operation-mode output height. */
int output_height = 0;

/**
 * Selects the internal resolution independently from the operation mode's presentation size.
 */
float InternalRenderScale(int height, float logical_height) {
    static const float override_scale = [] {
        if (const char *setting = std::getenv("KZ_SWITCH_RENDER_SCALE")) {
            char       *end = nullptr;
            const float scale = std::strtof(setting, &end);
            if (end != setting && *end == '\0' && scale >= 0.5f && scale <= 4.0f) {
                return scale;
            }
            std::fprintf(stderr, "switch: KZ_SWITCH_RENDER_SCALE must be between 0.5 and 4\n");
        }
        return 0.0f;
    }();
    return override_scale > 0.0f ? override_scale : static_cast<float>(height) / logical_height;
}

} // namespace

int SwitchMain(int argc, const char **argv, SwitchRunFunction run) {
    setenv("KZ_LIST_ARENA", "1", 0);
    setenv("KZ_ALPHA_OPAQUE", "1", 0);
    setenv("KZ_FOG_NEAR_SAFE", "1", 0);
    setenv("KZ_DEPTH_PLANES", "1", 0);
    setenv("KZ_FOG_NEAR_RECIPROCAL", "1", 0);
    setenv("KZ_INLINE_MESH_DISPATCH", "1", 0);
    const std::uint32_t romfs_result = romfsMountSelf("romfs");

    std::vector<std::string> words;
    std::ifstream            args_file;
    if (romfs_result == 0) {
        args_file.open("romfs:/args.txt");
    }
    if (!args_file.is_open()) {
        args_file.open("args.txt");
    }
    if (args_file) {
        std::string word;
        while (args_file >> word) {
            words.push_back(std::move(word));
        }
    }
    std::vector<const char *> input;
    if (argc > 0 && argv != nullptr) {
        input.assign(argv, argv + argc);
    }
    if (input.empty()) {
        input.push_back(KZ_SWITCH_APPLICATION_NAME);
    }
    for (const auto &word : words) {
        input.push_back(word.c_str());
    }
    std::vector<const char *> args{input.front()};
    for (std::size_t index = 1; index < input.size(); ++index) {
        if (std::string_view(input[index]) == "--env" && index + 1 < input.size()) {
            const std::string setting = input[++index];
            const std::size_t equals = setting.find('=');
            if (equals != std::string::npos && equals > 0) {
                setenv(setting.substr(0, equals).c_str(), setting.c_str() + equals + 1, 1);
            }
            continue;
        }
        args.push_back(input[index]);
    }
    args.push_back(nullptr);
    const int result = run(static_cast<int>(args.size()) - 1, args.data());
    if (romfs_result == 0) {
        static_cast<void>(romfsUnmount("romfs"));
    }
    return result;
}

void SwitchConfigureRenderer(gfx::RendererConfig &config) {
    int width = 0;
    int height = 0;
    SwitchDisplaySize(width, height);
    config.render_scale = InternalRenderScale(height, config.logical_height);
    output_width = width;
    output_height = height;
    int window_width = 0;
    int window_height = 0;
    if (!SDL_GetWindowSizeInPixels(WindowHandle(), &window_width, &window_height)) {
        std::fprintf(stderr, "switch: cannot query SDL output size: %s\n", SDL_GetError());
    }
    std::fprintf(stderr, "switch: mode target=%dx%d, SDL=%dx%d, render scale=%.2f\n", width, height,
                 window_width, window_height, static_cast<double>(config.render_scale));
    SwitchLogClocks();
}

void SwitchUpdateDisplay(float logical_height) {
    if (gfx::InFrame() || logical_height <= 0.0f) {
        return;
    }
    int width = 0;
    int height = 0;
    SwitchDisplaySize(width, height);
    const float scale = InternalRenderScale(height, logical_height);
    const bool  output_changed = width != output_width || height != output_height;
    if (!output_changed && scale == gfx::RenderScale()) {
        return;
    }
    if (output_changed) {
        if (!SDL_SetWindowSize(WindowHandle(), width, height)) {
            std::fprintf(stderr, "switch: cannot set SDL output size to %dx%d: %s\n", width, height, SDL_GetError());
            return;
        }
        output_width = width;
        output_height = height;
        gfx::RendererResize();
    }
    gfx::SetRenderScale(scale);
    std::fprintf(stderr, "switch: output target=%dx%d, render scale=%.2f\n", width, height, static_cast<double>(scale));
}
