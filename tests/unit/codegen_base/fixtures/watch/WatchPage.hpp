// SPDX-License-Identifier: MIT
//
// Developer-owned class of the size-change (`onSizeChange`) fixture (ADR-040): the page that owns
// one observing `Watcher` instance and observes a later node through a native binding. Never
// regenerated.

#pragma once

#include "WatchPage.g.hpp"

namespace watch {

class WatchPage final : public gen::WatchPageBase<WatchPage> {
public:
    void on_tail_size(const tessera::ui::SizeChangeEvent&) {}

private:
    friend struct gen::WatchPageBase<WatchPage>;
};

} // namespace watch
