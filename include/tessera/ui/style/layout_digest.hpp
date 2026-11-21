// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/ui/style/cascade.hpp"

#include <cstdint>

namespace tessera::ui::style {

// The layout half of one computed style, as a digest: exactly the longhands that can move a box,
// and nothing else. Computed once and cached on the style's shared entries
// (`ComputedStyle::cached_digest`), so a second call for an unchanged style is one load.
//
// It lives in the style module rather than in layout because `tessera_layout` depends on
// `tessera_style` (not the other way round), and because the digest is a property of a
// `ComputedStyle`: the layout signature folds it in, and the layout-unchanged gate compares it
// before and after a restyle (issue #735, slice A). A paint-only change leaves it identical.
//
// The property set is the one `layout_signature` documents: the longhands `apply_style`,
// `child_layout_order` and `text_style_from` read, minus the paint-only `text-overflow`. Adding a
// layout-affecting longhand to the engine means adding it here too, or a restyle that changes it
// will not be seen as a layout change.
[[nodiscard]] std::uint64_t layout_digest(const ComputedStyle& style) noexcept;

// True when `property` (a normalized longhand name) is part of the layout digest's set. Exposed so
// the signature's own tests can assert the inclusion list directly instead of inferring it.
[[nodiscard]] bool is_layout_property(std::string_view property) noexcept;

} // namespace tessera::ui::style
