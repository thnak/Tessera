// SPDX-License-Identifier: MIT
//
// Developer-owned half of the `Page` instance fixture (ADR-040): the caller that owns per-item
// `Row` instances (a keyed list), a conditional `Row` instance, and per-item presence. Never
// regenerated.

#pragma once

#include "Page.g.hpp"

namespace demo {

class Page final : public gen::PageBase<Page> {
public:
private:
    friend struct gen::PageBase<Page>;
};

} // namespace demo
