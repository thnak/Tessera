// SPDX-License-Identifier: MIT

// The coverage tables of the shim packages as data (docs/specs/router-shim-v1.md section 3 and
// docs/specs/store-i18n-shims-v1.md sections 3 and 5). The spec tables and this one are the same
// facts; a row here without its spec row (or the reverse) is a documentation bug the shim tests
// pin by name.
#pragma once

#include <cstdint>
#include <string_view>

namespace tessera::compiler::jsx::shim {

enum class Status : std::uint8_t {
    Supported,   // lowers today
    Pending,     // in the table and designed; the lowering is a follow-up
    Unsupported, // outside the table: the hint names the supported form
};

struct Row {
    std::string_view package;
    std::string_view name;
    Status status;
    std::string_view hint;
};

// `kNavigateHook` is the one name the rewrite below keys on.
inline constexpr std::string_view kRouterPackage = "react-router-dom";
inline constexpr std::string_view kNavigateHook = "useNavigate";

[[nodiscard]] const Row* find_row(std::string_view package, std::string_view name) noexcept;
[[nodiscard]] bool is_known_package(std::string_view package) noexcept;
// The hint for a name the package's table does not list.
[[nodiscard]] std::string_view unlisted_hint(std::string_view package) noexcept;

} // namespace tessera::compiler::jsx::shim
