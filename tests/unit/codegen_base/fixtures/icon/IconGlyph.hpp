// SPDX-License-Identifier: MIT
//
// Developer-owned class of the library Icon fixture (issue #530, ADR-040): `IconGlyph` has no
// `bind::` region, so the class only names itself. Never regenerated.

#pragma once

#include "IconGlyph.g.hpp"

namespace icon_demo {

class IconGlyph final : public gen::IconGlyphBase<IconGlyph> {
public:
private:
    friend struct gen::IconGlyphBase<IconGlyph>;
};

} // namespace icon_demo
