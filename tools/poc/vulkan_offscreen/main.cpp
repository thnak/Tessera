// SPDX-License-Identifier: MIT

#include <tessera/rendering/vulkan/offscreen_renderer.hpp>

#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <string>

int main(int argc, char** argv) {
    tessera::rendering::vulkan::OffscreenConfig config;

#ifdef NDEBUG
    config.enable_validation = false;
#else
    config.enable_validation = true;
#endif

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        try {
            if (arg == "--out" && i + 1 < argc) {
                config.output_path = argv[++i];
            } else if (arg == "--width" && i + 1 < argc) {
                config.width = static_cast<std::uint32_t>(std::stoul(argv[++i]));
            } else if (arg == "--height" && i + 1 < argc) {
                config.height = static_cast<std::uint32_t>(std::stoul(argv[++i]));
            } else if (arg == "--device" && i + 1 < argc) {
                config.device_index = std::stoi(argv[++i]);
            } else if (arg == "--validation") {
                config.enable_validation = true;
            } else if (arg == "--no-validation") {
                config.enable_validation = false;
            } else if (arg == "--help" || arg == "-h") {
                std::cout
                    << "usage: vulkan_offscreen_poc [--out <file.png>] [--width N] [--height N]\n"
                    << "                           [--device N] [--validation|--no-validation]\n";
                return 0;
            } else {
                std::cerr << "unknown argument: " << arg << "\n";
                return 2;
            }
        } catch (const std::exception& error) {
            std::cerr << "invalid argument '" << arg << "': " << error.what() << "\n";
            return 2;
        }
    }

    try {
        tessera::rendering::vulkan::OffscreenRenderer renderer(config);
        renderer.render_to_png();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "error: " << error.what() << "\n";
        return 1;
    }
}
