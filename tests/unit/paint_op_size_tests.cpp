// SPDX-License-Identifier: MIT
//
// The paint cache's per-op size budget (issue #731). `PaintOp` was 2376 bytes because every draw
// payload sat inline, and the cache charges `ops.capacity() * sizeof(PaintOp)` against a 2 MiB
// budget, so the flat shape held 104 of the configured 256 entries. The payload is now a
// `std::variant` and the three gradient shapes live in `PaintRecord::gradients`.
//
// These are `static_assert`s, not runtime checks: a size regression is a build failure, the same
// rule `node.hpp` applies to `kNodeSizeBudget`. The numbers are also pinned as `enforce` rows in
// tests/golden/perf/memory_budget.json, which catches a change on a lane that does not compile this
// test.

#include "tessera/ui/paint/paint_cache.hpp"

#include <cstddef>
#include <iostream>
#include <vector>

namespace paint = tessera::ui::paint;

namespace {

// The op must stay small enough that a record of a few ops fits the cache budget many times over.
// 128 leaves headroom for one more payload field without a redesign; the measured value is 104.
constexpr std::size_t kPaintOpSizeBudget = 128;
// The record carries the op vector, the gradient side table, the clip and the counts.
constexpr std::size_t kPaintRecordSizeBudget = 192;
// The budget is for a 24-byte `std::vector`; MSVC's Debug stdlib makes it 32 (issue #900), and the
// record holds two (`ops`, `gradients`), so the limit widens by exactly that and by no more. A
// real growth still fails: it moves the record without moving a vector.
constexpr std::size_t kShippingVectorBytes = 24;
constexpr std::size_t kPaintRecordVectors = 2;
constexpr std::size_t kPaintRecordSizeLimit =
    kPaintRecordSizeBudget +
    kPaintRecordVectors * (sizeof(std::vector<int>) - kShippingVectorBytes);

// A gradient payload is deliberately large and deliberately out of line: it is the whole reason the
// op is small. If this ever shrinks below the op budget the side table has stopped earning its
// keep.
static_assert(sizeof(paint::GradientPayload) > sizeof(paint::PaintOp),
              "the gradient side table only pays for itself while a gradient is bigger than an op");

static_assert(sizeof(paint::PaintOp) <= kPaintOpSizeBudget,
              "sizeof(PaintOp) regressed past its budget (issue #731)");
static_assert(sizeof(paint::PaintRecord) <= kPaintRecordSizeLimit,
              "sizeof(PaintRecord) regressed past its budget (issue #731)");

// Every kind must have a payload alternative that fits the op, so a new kind cannot silently
// reintroduce an inline payload.
static_assert(sizeof(paint::FillRectOp) <= kPaintOpSizeBudget, "FillRectOp outgrew the op budget");
static_assert(sizeof(paint::DrawTextOp) <= kPaintOpSizeBudget, "DrawTextOp outgrew the op budget");
static_assert(sizeof(paint::DrawAssetOp) <= kPaintOpSizeBudget,
              "DrawAssetOp outgrew the op budget");
static_assert(sizeof(paint::FillBoxShadowOp) <= kPaintOpSizeBudget,
              "FillBoxShadowOp outgrew the op budget");

} // namespace

int main() {
    bool ok = true;
    if (sizeof(paint::PaintOp) > kPaintOpSizeBudget) {
        std::cerr << "paint_op_size: sizeof(PaintOp)=" << sizeof(paint::PaintOp)
                  << " budget=" << kPaintOpSizeBudget << "\n";
        ok = false;
    }
    if (sizeof(paint::PaintRecord) > kPaintRecordSizeLimit) {
        std::cerr << "paint_op_size: sizeof(PaintRecord)=" << sizeof(paint::PaintRecord)
                  << " limit=" << kPaintRecordSizeLimit << "\n";
        ok = false;
    }
    std::cout << "paint_op_size: sizeof(PaintOp)=" << sizeof(paint::PaintOp)
              << " sizeof(PaintRecord)=" << sizeof(paint::PaintRecord)
              << " sizeof(GradientPayload)=" << sizeof(paint::GradientPayload) << "\n";
    return ok ? 0 : 1;
}
