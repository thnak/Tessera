// SPDX-License-Identifier: MIT

// File loader for the shader manifest. The byte cap is enforced before reading so an oversized
// file never reaches the decoder (docs/specs/shader-manifest-v1.md section 9).

#include "shader_manifest_internal.hpp"

#include <fstream>
#include <ios>
#include <string>
#include <utility>

namespace tessera::schema {

std::expected<Manifest, ValidationError> load_manifest_file(std::string_view path) {
    std::ifstream file(std::string(path), std::ios::binary | std::ios::ate);
    if (!file) {
        return internal::fail(ValidationErrorCode::FileUnreadable, "$",
                              "cannot open the manifest file");
    }
    const std::streamoff end = file.tellg();
    if (end < 0) {
        return internal::fail(ValidationErrorCode::FileUnreadable, "$",
                              "cannot determine the manifest size");
    }
    if (std::cmp_greater(end, kMaxManifestBytes)) {
        return internal::fail(ValidationErrorCode::FileTooLarge, "$",
                              "manifest exceeds the byte limit");
    }
    file.seekg(0, std::ios::beg);
    std::string text(static_cast<std::size_t>(end), '\0');
    if (!text.empty() && !file.read(text.data(), static_cast<std::streamsize>(text.size()))) {
        return internal::fail(ValidationErrorCode::FileUnreadable, "$",
                              "cannot read the manifest file");
    }
    return parse_manifest(text);
}

} // namespace tessera::schema
