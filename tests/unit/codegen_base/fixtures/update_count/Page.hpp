// SPDX-License-Identifier: MIT
//
// Developer-owned half of the `Page` fixture for the update fan-out measurement (#440): the caller
// that owns one static `Row` and a keyed list of per-item `Row` instances. Never regenerated.

#pragma once

#include "Page.g.hpp"

namespace demo {

class Page final : public gen::PageBase<Page> {
public:
private:
    friend struct gen::PageBase<Page>;
};

} // namespace demo
