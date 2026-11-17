// SPDX-License-Identifier: MIT
//
// Developer-owned half of the `Row` fixture for the update fan-out measurement (#440). It is the
// same component as `fixtures/instances/Row.hpp` plus one counter: a parent's generated `update()`
// calls `child.update()` on this type, so the shadowing `update()` below sees every
// parent-initiated call. The scheduler's own ticket calls `RowBase<Row>::update()` directly and
// therefore bypasses it, which is exactly the split the test needs (ticket runs are counted by
// `UpdateFlushStats::updated`). Never regenerated.

#pragma once

#include "Row.g.hpp"

namespace demo {

// Calls of `Row::update()` made by an owner's generated `update()`, not by the scheduler.
inline int g_row_direct_updates = 0;

class Row final : public gen::RowBase<Row> {
public:
    void update() {
        ++g_row_direct_updates;
        gen::RowBase<Row>::update();
    }

private:
    friend struct gen::RowBase<Row>;
};

} // namespace demo
