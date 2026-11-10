// SPDX-License-Identifier: MIT

#include "tessera/ui/virtual/item_geometry.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <iostream>
#include <vector>

namespace {

using tessera::ui::virtualization::ItemGeometry;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "virtual_geometry_scan: " << message << "\n";
    }
    return condition;
}

bool near(float value, float expected, float tolerance = 1.0e-4f) {
    return std::fabs(value - expected) <= tolerance;
}

// Linear-scan oracle for the Fenwick searches. It mirrors the documented contract, including the
// trailing-gap handling of `item_offset(count)`.
struct LinearModel {
    std::vector<float> sizes;
    float gap = 0.0f;

    [[nodiscard]] float raw_offset(std::size_t index) const {
        float sum = 0.0f;
        for (std::size_t item = 0; item < index && item < sizes.size(); ++item) {
            sum += sizes[item] + gap;
        }
        return sum;
    }

    [[nodiscard]] float offset(std::size_t index) const {
        const float raw = raw_offset(index);
        return index == sizes.size() && !sizes.empty() ? raw - gap : raw;
    }

    [[nodiscard]] float total() const {
        return offset(sizes.size());
    }
};

bool check_against_linear(const ItemGeometry& geometry, const LinearModel& model) {
    const std::size_t count = model.sizes.size();
    const float total = model.total();
    if (!expect(geometry.item_count() == count, "item count matches the model") ||
        !expect(near(geometry.total_extent(), total), "total extent matches the linear scan")) {
        return false;
    }
    for (std::size_t index = 0; index <= count; ++index) {
        if (!expect(near(geometry.item_offset(index), model.offset(index)),
                    "item offset matches the linear scan")) {
            return false;
        }
    }

    for (float probe = -20.0f; probe <= total + 40.0f; probe += 6.5f) {
        std::size_t expected_index = 0;
        if (count == 0 || probe <= 0.0f) {
            expected_index = 0;
        } else if (probe >= total) {
            expected_index = count - 1;
        } else {
            std::size_t last = 0;
            while (last < count && model.offset(last) <= probe) {
                ++last;
            }
            expected_index = last - 1;
        }
        if (!expect(geometry.index_at_offset(probe) == expected_index,
                    "index_at_offset matches the linear scan")) {
            return false;
        }
    }

    for (float scroll = -20.0f; scroll <= total + 20.0f; scroll += 11.0f) {
        for (const float viewport : {0.0f, 1.0f, 40.0f, 100.0f, 250.0f, 1000.0f}) {
            ItemGeometry::Range expected{0, 0};
            if (count != 0 && viewport > 0.0f && total > 0.0f) {
                const float start = std::max(scroll, 0.0f);
                if (start >= total) {
                    expected = ItemGeometry::Range{count, count};
                } else {
                    std::size_t first = 0;
                    if (start > 0.0f) {
                        std::size_t next = 0;
                        while (next < count && model.offset(next) <= start) {
                            ++next;
                        }
                        first = next - 1;
                    }
                    std::size_t last = 0;
                    while (last < count && model.offset(last) < start + viewport) {
                        ++last;
                    }
                    expected = ItemGeometry::Range{first, std::max(first, last)};
                }
            }
            const ItemGeometry::Range actual = geometry.visible_range(scroll, viewport);
            if (!expect(actual.first == expected.first && actual.last == expected.last,
                        "visible range matches the linear scan")) {
                return false;
            }
        }
    }
    return true;
}

bool run_case(float gap, const std::vector<float>& measured) {
    ItemGeometry geometry(ItemGeometry::Config{measured.size() + 4, 32.0f, gap});
    if (!geometry.set_count(measured.size())) {
        return expect(false, "setup set_count");
    }
    LinearModel model;
    model.gap = gap;
    model.sizes.assign(measured.size(), 32.0f);
    if (!check_against_linear(geometry, model)) {
        return false;
    }
    // Measure out of order so each refine is followed by a suffix-shift verification.
    for (std::size_t step = 0; step < measured.size(); ++step) {
        const std::size_t index = (step * 7 + 3) % measured.size();
        if (measured[index] == model.sizes[index]) {
            continue;
        }
        const ItemGeometry::MeasureChange change = geometry.refine_measure(index, measured[index]);
        if (!expect(change.changed && near(change.delta, measured[index] - model.sizes[index]),
                    "refine reports the delta")) {
            return false;
        }
        model.sizes[index] = measured[index];
        if (!check_against_linear(geometry, model)) {
            return false;
        }
    }
    return true;
}

} // namespace

int main() {
    // Uniform estimates, with and without a gap.
    if (!run_case(0.0f, std::vector<float>(8, 32.0f)) ||
        !run_case(4.0f, std::vector<float>(10, 32.0f))) {
        return 1;
    }

    // Mixed measured sizes, including a collapsed zero-size row and a very tall row.
    if (!run_case(4.0f, {0.0f, 8.0f, 32.0f, 48.0f, 96.0f, 200.0f, 32.0f, 32.0f, 12.0f, 64.0f})) {
        return 2;
    }

    // Fractional gap and shuffled sizes.
    if (!run_case(3.5f, {12.0f, 32.0f, 7.0f, 64.0f, 32.0f, 0.0f, 120.0f, 16.0f, 32.0f, 40.0f, 24.0f,
                         32.0f})) {
        return 3;
    }

    // Degenerate sizes: one zero-size item, then all zero-size items with no gap.
    if (!run_case(4.0f, {0.0f}) || !run_case(0.0f, {0.0f, 0.0f, 0.0f})) {
        return 4;
    }

    return 0;
}
