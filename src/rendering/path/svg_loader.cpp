// SPDX-License-Identifier: MIT

#include "tessera/rendering/path/svg_loader.hpp"

#ifdef TESSERA_HAVE_THORVG
#include "thorvg_svg_loader.hpp"
#endif

#include <span>

namespace tessera::rendering::path {

bool svg_backend_available() noexcept {
#ifdef TESSERA_HAVE_THORVG
    return true;
#else
    return false;
#endif
}

std::expected<SvgDocument, SvgError> load_svg(std::span<const std::uint8_t> bytes,
                                              SvgLimits limits) {
#ifdef TESSERA_HAVE_THORVG
    return load_svg_thorvg(bytes, limits);
#else
    static_cast<void>(bytes);
    static_cast<void>(limits);
    return std::unexpected(SvgError::backend_unavailable);
#endif
}

std::expected<SvgDocument, SvgError> load_svg(std::span<const char> bytes, SvgLimits limits) {
    const auto* data = reinterpret_cast<const std::uint8_t*>(bytes.data());
    return load_svg(std::span<const std::uint8_t>(data, bytes.size()), limits);
}

} // namespace tessera::rendering::path
