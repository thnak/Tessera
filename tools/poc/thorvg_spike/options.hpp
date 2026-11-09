// SPDX-License-Identifier: MIT

#pragma once

#include <cstdint>
#include <iostream>
#include <string>

namespace tessera::poc::thorvg_spike {

struct Options {
    std::string backend = "thorvg";
    std::string out = "thorvg_spike.png";
    std::string raw;
    std::string compare;
    std::uint32_t width = 512;
    std::uint32_t height = 512;
    std::uint32_t tolerance = 0;
    double max_mismatch = 0.0;
    bool help = false;
};

inline void print_usage() {
    std::cout
        << "usage: thorvg_spike [--backend thorvg|null] [--out path] [--raw path]\n"
           "                     [--compare golden.png] [--width N] [--height N]\n"
           "                     [--tolerance N] [--max-mismatch ratio]\n"
           "\n"
           "Renders the representative icon/chart path set through the PathRasterizer seam\n"
           "and writes a PNG. With --compare, exits non-zero when the image differs from the\n"
           "golden beyond the tolerance.\n";
}

// Returns false on a malformed command line; --help sets options.help and returns true.
inline bool parse_options(int argc, char** argv, Options& options) {
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            options.help = true;
            return true;
        }
        if (arg == "--backend" && i + 1 < argc) {
            options.backend = argv[++i];
        } else if (arg == "--out" && i + 1 < argc) {
            options.out = argv[++i];
        } else if (arg == "--raw" && i + 1 < argc) {
            options.raw = argv[++i];
        } else if (arg == "--compare" && i + 1 < argc) {
            options.compare = argv[++i];
        } else if (arg == "--width" && i + 1 < argc) {
            options.width = static_cast<std::uint32_t>(std::stoul(argv[++i]));
        } else if (arg == "--height" && i + 1 < argc) {
            options.height = static_cast<std::uint32_t>(std::stoul(argv[++i]));
        } else if (arg == "--tolerance" && i + 1 < argc) {
            options.tolerance = static_cast<std::uint32_t>(std::stoul(argv[++i]));
        } else if (arg == "--max-mismatch" && i + 1 < argc) {
            options.max_mismatch = std::stod(argv[++i]);
        } else {
            std::cerr << "unknown argument: " << arg << "\n";
            return false;
        }
    }
    return true;
}

} // namespace tessera::poc::thorvg_spike
