// SPDX-License-Identifier: MIT

// Key -> index resolution over a read-only data source (`docs/specs/virtualization-v1.md`
// sections 3 and 6): stable keys survive inserts/removes/moves, duplicates and capacity overflow
// invalidate the whole mapping, and the probe order is deterministic.

#include "tessera/ui/virtual/key_index.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <utility>
#include <vector>

namespace {

using tessera::ui::virtualization::KeyIndex;
using tessera::ui::virtualization::KeyIndexError;
using tessera::ui::virtualization::MeasureConstraints;
using tessera::ui::virtualization::VirtualDataSource;
using tessera::ui::virtualization::VirtualItemId;
using tessera::ui::virtualization::VirtualItemMeasure;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "virtual_key_index: " << message << "\n";
    }
    return condition;
}

VirtualItemId key(std::uint64_t value) {
    return VirtualItemId{value};
}

// Keys are the item model's own stable ids, deliberately not equal to the index.
class KeyListSource final : public VirtualDataSource {
public:
    explicit KeyListSource(std::vector<std::uint64_t> values)
        : values_(std::move(values)) {}

    [[nodiscard]] std::size_t item_count() const override {
        return values_.size();
    }
    [[nodiscard]] VirtualItemId item_key(std::size_t index) const override {
        return VirtualItemId{values_[index]};
    }
    [[nodiscard]] VirtualItemMeasure measure(std::size_t,
                                             const MeasureConstraints&) const override {
        return VirtualItemMeasure{32.0f, 100.0f};
    }
    void build(std::size_t, tessera::ui::Node&) override {}

private:
    std::vector<std::uint64_t> values_;
};

} // namespace

int main() {
    // Resolution for a source whose keys are unrelated to the index.
    KeyListSource source({100, 200, 300, 400});
    KeyIndex index(8);
    if (!expect(index.rebuild(source).has_value(), "rebuild from the data source") ||
        !expect(index.size() == 4 && index.valid(), "index reports its size") ||
        !expect(index.find(key(100)) == 0 && index.find(key(400)) == 3,
                "key resolves to its index") ||
        !expect(index.key_at(2) == key(300), "index -> key") ||
        !expect(!index.find(key(999)).has_value(), "unknown key misses") ||
        !expect(index.key_at(9) == VirtualItemId{}, "out-of-range key is empty")) {
        return 1;
    }

    // A data update that inserts a row before the anchor: the key keeps resolving, only the
    // index moves.
    KeyListSource inserted({50, 100, 200, 300, 400});
    if (!expect(index.rebuild(inserted).has_value(), "rebuild after insert") ||
        !expect(index.find(key(100)) == 1, "inserted key shifts later indices") ||
        !expect(index.key_at(1) == key(100), "index -> key after insert")) {
        return 2;
    }
    KeyListSource removed({100, 300, 400});
    if (!expect(index.rebuild(removed).has_value(), "rebuild after remove") ||
        !expect(index.find(key(200)) == std::nullopt, "removed key disappears") ||
        !expect(index.find(key(300)) == 1, "surviving keys move up")) {
        return 3;
    }

    // Duplicate keys are errors: the index is invalidated, never partially valid, and the first
    // duplicate in index order is reported.
    KeyListSource duplicate({7, 8, 9, 8});
    KeyIndex dup_index(8);
    const auto dup_result = dup_index.rebuild(duplicate);
    if (!expect(!dup_result.has_value(), "duplicate keys fail") ||
        !expect(dup_result.error().kind == KeyIndexError::Kind::DuplicateKey, "duplicate kind") ||
        !expect(dup_result.error().key == key(8) && dup_result.error().index == 3,
                "first duplicate is reported") ||
        !expect(dup_index.size() == 0 && !dup_index.valid(), "failed rebuild clears the index") ||
        !expect(!dup_index.find(key(7)).has_value(), "no lookup survives a failed rebuild")) {
        return 4;
    }

    // Capacity overflow is an error and names the attempted count.
    KeyIndex small(3);
    const auto overflow = small.rebuild(KeyListSource({1, 2, 3, 4}));
    if (!expect(!overflow.has_value(), "capacity overflow fails") ||
        !expect(overflow.error().kind == KeyIndexError::Kind::CapacityExceeded, "capacity kind") ||
        !expect(overflow.error().index == 4, "overflow reports the source count") ||
        !expect(!small.valid(), "overflow invalidates the index")) {
        return 5;
    }

    // Zero-capacity index is usable only for an empty collection.
    KeyIndex empty(0);
    if (!expect(empty.rebuild(KeyListSource({})).has_value(), "empty source fits an empty index") ||
        !expect(empty.capacity() == 0 && empty.size() == 0, "empty index reports zero") ||
        !expect(!empty.rebuild(KeyListSource({1})).has_value(), "one key overflows capacity 0")) {
        return 6;
    }

    // The span overload is the same mapping without a source; reordering the array is how a move
    // arrives at the adapter.
    const std::array<VirtualItemId, 5> keys{key(5), key(1), key(9), key(3), key(7)};
    KeyIndex span_index(5);
    if (!expect(span_index.rebuild(keys).has_value(), "rebuild from a key span") ||
        !expect(span_index.find(key(9)) == 2, "span key resolves") ||
        !expect(span_index.key_at(4) == key(7), "span order is preserved")) {
        return 7;
    }

    // Determinism: the same key set resolves identically on a fresh index, including keys that
    // collide modulo small bucket counts.
    KeyIndex first(6);
    KeyIndex second(6);
    const std::array<VirtualItemId, 6> crowded{key(1), key(65), key(129),
                                               key(2), key(66), key(130)};
    if (!expect(first.rebuild(crowded).has_value(), "crowded set fits") ||
        !expect(second.rebuild(crowded).has_value(), "crowded set fits again") ||
        !expect(first.size() == second.size(), "same size")) {
        return 8;
    }
    for (std::size_t i = 0; i < crowded.size(); ++i) {
        if (!expect(first.find(crowded[i]) == i && second.find(crowded[i]) == i,
                    "colliding keys resolve to their own index")) {
            return 9;
        }
    }

    // A stale mapping is never served after a failed rebuild.
    KeyIndex stale(4);
    if (!expect(stale.rebuild(KeyListSource({11, 22})).has_value(), "stale setup") ||
        !expect(!stale.rebuild(KeyListSource({11, 11})).has_value(), "stale duplicate fails") ||
        !expect(!stale.find(key(11)).has_value() && !stale.find(key(22)).has_value(),
                "stale lookups miss after invalidation")) {
        return 10;
    }
    return 0;
}
