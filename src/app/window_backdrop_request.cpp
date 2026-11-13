// SPDX-License-Identifier: MIT

#include "tessera/ui/scene/node.hpp"
#include "window_backdrop_request.hpp"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <string>
#include <string_view>

namespace tessera::app {
namespace {

std::string_view trim(std::string_view value) {
    const std::size_t first = value.find_first_not_of(" \t");
    if (first == std::string_view::npos) {
        return {};
    }
    const std::size_t last = value.find_last_not_of(" \t");
    return value.substr(first, last - first + 1);
}

// CSS keywords are ASCII case-insensitive while the platform grammar is lowercase, so the inline
// value is normalized once at startup (never on a frame path).
std::string lowercased(std::string_view value) {
    std::string text(value);
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

} // namespace

hal::WindowBackdrop window_backdrop_request(const ui::Node& root,
                                            hal::WindowBackdrop explicit_request) {
    if (explicit_request != hal::WindowBackdrop::Disabled) {
        return explicit_request;
    }
    for (const ui::Node::InlineStyleDeclaration& declaration : root.inline_style()) {
        if (declaration.name != "window-backdrop") {
            continue;
        }
        const std::string value = lowercased(trim(declaration.value));
        hal::WindowBackdrop parsed = hal::WindowBackdrop::Disabled;
        return hal::window_backdrop_from_name(value, parsed) ? parsed
                                                             : hal::WindowBackdrop::Disabled;
    }
    return hal::WindowBackdrop::Disabled;
}

} // namespace tessera::app
