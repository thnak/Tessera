// SPDX-License-Identifier: MIT
//
// Developer-owned class of the compose fixture (never regenerated).

#pragma once

#include "WatchPage.g.hpp"

namespace compose {

class WatchPage final : public gen::WatchPageBase<WatchPage> {
public:
    void on_tail_size(const tessera::ui::SizeChangeEvent&) {}

private:
    friend struct gen::WatchPageBase<WatchPage>;
};

} // namespace compose
