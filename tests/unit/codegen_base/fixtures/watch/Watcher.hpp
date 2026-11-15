// SPDX-License-Identifier: MIT
//
// Developer-owned class of the size-change (`onSizeChange`) fixture (ADR-040): the component that
// observes its own size and counts observations. Never regenerated.

#pragma once

#include "Watcher.g.hpp"

namespace watch {

class Watcher final : public gen::WatcherBase<Watcher> {
public:
private:
    friend struct gen::WatcherBase<Watcher>;
};

} // namespace watch
