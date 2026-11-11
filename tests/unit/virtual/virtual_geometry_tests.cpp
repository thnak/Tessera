// SPDX-License-Identifier: MIT

#include "tessera/ui/virtual/item_geometry.hpp"

#include <array>
#include <cmath>
#include <iostream>

namespace {

using tessera::ui::virtualization::ItemGeometry;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "virtual_geometry: " << message << "\n";
    }
    return condition;
}

bool near(float value, float expected, float tolerance = 1.0e-4f) {
    return std::fabs(value - expected) <= tolerance;
}

ItemGeometry make_geometry(std::size_t count, std::size_t capacity = 64) {
    ItemGeometry geometry(ItemGeometry::Config{capacity, 32.0f, 4.0f});
    if (!geometry.set_count(count)) {
        std::cerr << "virtual_geometry: setup set_count failed\n";
    }
    return geometry;
}

bool check_offset(const ItemGeometry& geometry, std::size_t index, float expected,
                  const char* message) {
    return expect(near(geometry.item_offset(index), expected), message);
}

} // namespace

int main() {
    // 10 unmeasured items: 32 px estimate + 4 px gap -> stride 36, total 356.
    ItemGeometry geometry = make_geometry(10);
    if (!expect(near(geometry.total_extent(), 356.0f), "estimated total extent") ||
        !expect(near(geometry.estimate_main(), 32.0f), "default 32 px estimate") ||
        !expect(geometry.measured_count() == 0, "nothing measured initially") ||
        !expect(!geometry.is_measured(3), "item reports unmeasured") ||
        !expect(near(geometry.item_main_size(3), 32.0f), "unmeasured item uses the estimate") ||
        !expect(near(geometry.item_main_size(10), 0.0f), "out-of-range size is zero") ||
        !check_offset(geometry, 0, 0.0f, "first offset") ||
        !check_offset(geometry, 1, 36.0f, "second offset") ||
        !check_offset(geometry, 10, 356.0f, "offset past the end is the total")) {
        return 1;
    }

    if (!expect(geometry.index_at_offset(-10.0f) == 0, "negative offset clamps to first") ||
        !expect(geometry.index_at_offset(0.0f) == 0, "zero offset is the first item") ||
        !expect(geometry.index_at_offset(35.0f) == 0, "gap belongs to the previous item") ||
        !expect(geometry.index_at_offset(36.0f) == 1, "exact item start") ||
        !expect(geometry.index_at_offset(100.0f) == 2, "mid item") ||
        !expect(geometry.index_at_offset(1.0e6f) == 9, "past the end clamps to last")) {
        return 2;
    }

    // Forward and backward scroll windows.
    ItemGeometry::Range range = geometry.visible_range(0.0f, 100.0f);
    if (!expect(range.first == 0 && range.last == 3, "window at top") ||
        !expect(!range.empty() && range.count() == 3, "window count")) {
        return 3;
    }
    range = geometry.visible_range(36.0f, 100.0f);
    if (!expect(range.first == 1 && range.last == 4, "window after one stride")) {
        return 4;
    }
    range = geometry.visible_range(72.0f, 100.0f);
    if (!expect(range.first == 2 && range.last == 5, "window scrolled forward")) {
        return 5;
    }
    range = geometry.visible_range(144.0f, 100.0f);
    if (!expect(range.first == 4 && range.last == 7, "window further forward")) {
        return 6;
    }
    range = geometry.visible_range(72.0f, 100.0f);
    if (!expect(range.first == 2 && range.last == 5, "window scrolled backward")) {
        return 7;
    }
    if (!expect(geometry.visible_range(-50.0f, 100.0f).first == 0, "negative scroll clamps") ||
        !expect(geometry.visible_range(356.0f, 100.0f).empty(),
                "scroll at total extent shows nothing") ||
        !expect(geometry.visible_range(0.0f, 0.0f).empty(), "zero viewport shows nothing")) {
        return 8;
    }

    // Monotonicity under a scripted scroll: both ends never move backwards while scrolling down,
    // and every reported window stays inside [0, count).
    std::size_t previous_first = 0;
    std::size_t previous_last = 0;
    for (float scroll = 0.0f; scroll <= 356.0f; scroll += 36.0f) {
        const ItemGeometry::Range window = geometry.visible_range(scroll, 100.0f);
        if (!expect(window.last <= geometry.item_count(), "window inside the collection") ||
            !expect(window.first >= previous_first && window.last >= previous_last,
                    "window advances monotonically") ||
            !expect(window.first <= window.last, "window is ordered")) {
            return 9;
        }
        previous_first = window.first;
        previous_last = window.last;
    }

    // Estimate then measure: the delta moves the suffix, the total and the suffix offset.
    geometry = make_geometry(10);
    ItemGeometry::MeasureChange change = geometry.refine_measure(4, 80.0f);
    if (!expect(change.changed && change.became_measured, "estimate becomes measured") ||
        !expect(!change.became_estimate, "not a reset") ||
        !expect(near(change.old_main, 32.0f) && near(change.new_main, 80.0f), "old/new size") ||
        !expect(near(change.delta, 48.0f), "measure delta") ||
        !expect(near(geometry.total_extent(), 404.0f), "total after measure") ||
        !expect(near(change.suffix_offset, geometry.item_offset(5)), "reported suffix offset") ||
        !expect(geometry.measured_count() == 1 && geometry.is_measured(4),
                "measured bookkeeping") ||
        !check_offset(geometry, 5, 228.0f, "suffix shifted by the measure delta")) {
        return 10;
    }
    change = geometry.refine_measure(4, 80.0f);
    if (!expect(!change.changed, "identical refine is a no-op") ||
        !expect(near(change.delta, 0.0f), "no-op delta") ||
        !expect(near(change.suffix_offset, 228.0f), "no-op still reports the suffix")) {
        return 11;
    }
    // A real measure that equals the estimate still counts as measured without a total change.
    change = geometry.refine_measure(0, 32.0f);
    if (!expect(change.changed && change.became_measured, "equal measure becomes measured") ||
        !expect(near(change.delta, 0.0f), "equal measure has no delta") ||
        !expect(near(geometry.total_extent(), 404.0f), "equal measure keeps the total") ||
        !expect(geometry.measured_count() == 2, "equal measure counts")) {
        return 12;
    }
    change = geometry.reset_measure(4);
    if (!expect(change.changed && change.became_estimate, "reset restores the estimate") ||
        !expect(near(change.delta, -48.0f), "reset delta") ||
        !expect(near(geometry.total_extent(), 356.0f), "total after reset") ||
        !expect(geometry.measured_count() == 1, "reset bookkeeping")) {
        return 13;
    }
    if (!expect(!geometry.reset_measure(4).changed, "second reset is a no-op")) {
        return 14;
    }

    // Mid-list refine: measured items before the refined key keep their offset and size, measured
    // items after it move by exactly the delta, and nothing is re-measured or rebuilt.
    geometry = make_geometry(10);
    ItemGeometry::MeasureChange first_measure = geometry.refine_measure(1, 64.0f);
    ItemGeometry::MeasureChange second_measure = geometry.refine_measure(7, 16.0f);
    if (!expect(near(first_measure.delta, 32.0f) && near(second_measure.delta, -16.0f),
                "setup measures applied")) {
        return 14;
    }
    std::array<float, 11> before{};
    for (std::size_t index = 0; index <= geometry.item_count(); ++index) {
        before[index] = geometry.item_offset(index);
    }
    const float total_before = geometry.total_extent();
    const float size_before_1 = geometry.item_main_size(1);
    change = geometry.refine_measure(3, 96.0f);
    if (!expect(near(change.delta, 64.0f), "mid-list delta") ||
        !expect(near(change.suffix_offset, geometry.item_offset(4)), "mid-list suffix offset")) {
        return 15;
    }
    for (std::size_t index = 0; index <= 3; ++index) {
        if (!expect(near(geometry.item_offset(index), before[index]), "prefix offset unchanged")) {
            return 16;
        }
    }
    for (std::size_t index = 4; index <= 10; ++index) {
        if (!expect(near(geometry.item_offset(index), before[index] + 64.0f),
                    "suffix offset shifted by the delta")) {
            return 17;
        }
    }
    if (!expect(near(geometry.item_main_size(1), size_before_1), "measured item keeps its size") ||
        !expect(near(geometry.item_main_size(7), 16.0f), "later measured item keeps its size") ||
        !expect(near(geometry.total_extent(), total_before + 64.0f), "total grows by the delta") ||
        !expect(geometry.measured_count() == 3, "refine counts once")) {
        return 18;
    }

    // Visible range with a measured item larger than its estimate.
    geometry = make_geometry(6);
    if (!expect(near(geometry.refine_measure(0, 200.0f).delta, 168.0f), "large measure applied")) {
        return 19;
    }
    range = geometry.visible_range(100.0f, 100.0f);
    if (!expect(range.first == 0 && range.last == 1, "measured item owns the window") ||
        !expect(near(geometry.item_offset(1), 204.0f), "post-measure offset")) {
        return 19;
    }
    range = geometry.visible_range(204.0f, 100.0f);
    if (!expect(range.first == 1 && range.last == 4, "window after a measured item")) {
        return 20;
    }

    // Data-source insert/remove keeps measurements attached to their items, not their old index.
    geometry = make_geometry(5);
    if (!expect(near(geometry.refine_measure(2, 64.0f).delta, 32.0f), "seed measurement")) {
        return 21;
    }
    if (!expect(geometry.insert(1, 2), "insert two rows") ||
        !expect(geometry.item_count() == 7, "count after insert") ||
        !expect(geometry.is_measured(4), "measurement moved with its item") ||
        !expect(near(geometry.item_main_size(4), 64.0f), "moved measurement size") ||
        !expect(!geometry.is_measured(1) && !geometry.is_measured(2), "inserted rows estimated") ||
        !expect(near(geometry.item_main_size(2), 32.0f), "inserted row size") ||
        !expect(near(geometry.total_extent(), 280.0f), "total after insert")) {
        return 21;
    }
    if (!expect(geometry.remove(1, 3), "remove three rows") ||
        !expect(geometry.item_count() == 4, "count after remove") ||
        !expect(geometry.is_measured(1) && near(geometry.item_main_size(1), 64.0f),
                "measurement survived the removal") ||
        !expect(near(geometry.total_extent(), 172.0f), "total after remove")) {
        return 22;
    }

    // Fixed capacity: overflowing mutations fail without changing the tables.
    ItemGeometry small(ItemGeometry::Config{4, 32.0f, 0.0f});
    if (!expect(!small.set_count(5), "count above capacity rejected") ||
        !expect(small.item_count() == 0, "rejected count changes nothing") ||
        !expect(small.set_count(4), "count at capacity accepted") ||
        !expect(!small.insert(0, 1), "insert above capacity rejected") ||
        !expect(small.insert(4, 0), "empty insert is a no-op") ||
        !expect(!small.insert(5, 1), "insert past the end rejected") ||
        !expect(!small.remove(4, 1), "remove past the end rejected") ||
        !expect(small.remove(0, 10), "remove clamps to the current count") ||
        !expect(small.item_count() == 0, "remove emptied the geometry")) {
        return 23;
    }

    ItemGeometry empty(ItemGeometry::Config{0, 32.0f, 4.0f});
    if (!expect(empty.total_extent() == 0.0f, "empty total") ||
        !expect(empty.visible_range(0.0f, 100.0f).empty(), "empty window") ||
        !expect(empty.index_at_offset(10.0f) == 0, "empty index lookup") ||
        !expect(!empty.refine_measure(0, 10.0f).changed, "empty refine is a no-op")) {
        return 24;
    }

    // Two identical scripted runs produce identical geometry (no clock/RNG/allocator influence).
    std::array<float, 8> first_run{};
    std::array<float, 8> second_run{};
    for (std::array<float, 8>* run : {&first_run, &second_run}) {
        ItemGeometry scripted = make_geometry(8);
        expect(near(scripted.refine_measure(3, 48.0f).delta, 16.0f), "script measure one");
        expect(near(scripted.refine_measure(6, 12.0f).delta, -20.0f), "script measure two");
        expect(scripted.insert(2, 1), "script insert");
        expect(near(scripted.refine_measure(0, 64.0f).delta, 32.0f), "script measure three");
        for (std::size_t index = 0; index < run->size(); ++index) {
            (*run)[index] = scripted.item_offset(index);
        }
    }
    if (!expect(first_run == second_run, "geometry is deterministic across runs")) {
        return 25;
    }

    // Geometry re-seed (`reseed`, virtualization-v1.md section 10): a new estimate/gap keeps every
    // measurement, moves only the unmeasured entries, and an unchanged pair reports false.
    ItemGeometry reseeded = make_geometry(4);
    if (!expect(near(reseeded.refine_measure(1, 64.0f).delta, 32.0f), "reseed: seed measurement") ||
        !expect(near(reseeded.total_extent(), 172.0f), "reseed: measured total") ||
        !expect(reseeded.reseed(40.0f, 8.0f), "reseed: a changed estimate/gap reseeds") ||
        !expect(near(reseeded.estimate_main(), 40.0f), "reseed: the estimate was adopted") ||
        !expect(near(reseeded.gap(), 8.0f), "reseed: the gap was adopted") ||
        !expect(reseeded.is_measured(1), "reseed: the measured entry stayed measured") ||
        !expect(near(reseeded.item_main_size(1), 64.0f), "reseed: the measure kept its size") ||
        !expect(near(reseeded.item_main_size(0), 40.0f), "reseed: unmeasured took the estimate") ||
        !expect(near(reseeded.item_offset(1), 48.0f), "reseed: offsets use the new stride") ||
        !expect(near(reseeded.item_offset(2), 120.0f),
                "reseed: the measured entry keeps its slot") ||
        !expect(near(reseeded.total_extent(), 208.0f), "reseed: total follows the new entries") ||
        !expect(!reseeded.reseed(40.0f, 8.0f), "reseed: an unchanged pair reports false") ||
        !expect(reseeded.measured_count() == 1, "reseed: nothing was dropped")) {
        return 26;
    }
    reseeded.clear_measurements();
    if (!expect(reseeded.measured_count() == 0, "reseed: clear_measurements drops the rest") ||
        !expect(near(reseeded.item_main_size(1), 40.0f), "reseed: cleared entry is the estimate") ||
        !expect(near(reseeded.total_extent(), 184.0f), "reseed: total after the clear")) {
        return 27;
    }

    return 0;
}
