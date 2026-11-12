// SPDX-License-Identifier: MIT

// Build-time shader manifest check (docs/specs/shader-manifest-v1.md sections 5 and 9).
//
// The Vulkan and TESSERA_SHADER_CROSS glslc rules run this executable before the compiler, so a
// malformed manifest fails the build with a stable error token and JSON field path instead of
// producing shader output. The tool links only tessera_schema: no GPU, backend or OS headers.

#include "tessera/schema/manifest.hpp"

#include <iostream>
#include <string_view>

namespace {

constexpr int kUsageError = 2;

} // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: tessera_shader_manifest_check <manifest.json>\n";
        return kUsageError;
    }
    const std::string_view path = argv[1];
    const auto manifest = tessera::schema::load_manifest_file(path);
    if (!manifest) {
        const auto& error = manifest.error();
        std::cerr << "shader manifest error: " << path << ": "
                  << tessera::schema::to_string(error.code) << " at " << error.path;
        if (error.offset != 0) {
            std::cerr << " (offset " << error.offset << ")";
        }
        std::cerr << ": " << error.detail << "\n";
        return 1;
    }
    std::cout << "shader manifest ok: " << path << " (" << manifest->shaders.size()
              << " shader(s))\n";
    return 0;
}
