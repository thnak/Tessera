// SPDX-License-Identifier: MIT

#include "tessera/ui/virtual/item_state_store.hpp"

#include <array>
#include <cmath>
#include <cstdint>
#include <iostream>

namespace {

using tessera::ui::virtualization::ItemState;
using tessera::ui::virtualization::ItemStateStore;
using tessera::ui::virtualization::VirtualItemId;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "virtual_state: " << message << "\n";
    }
    return condition;
}

bool near(float value, float expected, float tolerance = 1.0e-4f) {
    return std::fabs(value - expected) <= tolerance;
}

// Stand-in for a realized item node. Recycling clears everything the node holds; anything read
// back afterwards must come from the keyed store, never from the previous item.
struct FakeNode {
    std::uint64_t payload = 0;
    float measured_main = 0.0f;
};

} // namespace

int main() {
    if (!expect(ItemStateStore{}.capacity() == ItemStateStore::kDefaultCapacity,
                "default capacity") ||
        !expect(ItemStateStore{0}.capacity() == ItemStateStore::kMinCapacity,
                "zero capacity clamps to one")) {
        return 1;
    }

    // Basic put/find/peek/contains and stats bookkeeping.
    ItemStateStore store(4);
    const ItemState empty;
    if (!expect(empty.revision == 0 && empty.payload == 0, "default state is empty") ||
        !expect(!empty.measured_main && !empty.measured_cross, "default measure is zero")) {
        return 2;
    }
    const VirtualItemId key{7};
    ItemState state;
    state.revision = 2;
    state.payload = 0xABCDULL;
    state.measured_main = 48.0f;
    state.measured_cross = 120.0f;
    if (!expect(!store.contains(key), "unknown key is absent") ||
        !expect(store.put(key, state), "first put inserts") ||
        !expect(store.size() == 1, "size after insert") ||
        !expect(store.peek(key) != nullptr, "peek finds the entry") ||
        !expect(store.contains(key), "contains finds the entry")) {
        return 3;
    }
    ItemState* found = store.find(key);
    if (!expect(found != nullptr, "find returns the entry") ||
        !expect(found->payload == 0xABCDULL && near(found->measured_main, 48.0f),
                "find returns the stored values") ||
        !expect(found->revision == 2 && near(found->measured_cross, 120.0f),
                "find returns the whole state")) {
        return 4;
    }
    if (!expect(!store.put(key, state), "overwrite is not an insert") ||
        !expect(store.size() == 1, "overwrite keeps one entry")) {
        return 5;
    }
    ItemStateStore::Stats stats = store.stats();
    if (!expect(stats.entries == 1 && stats.peak_entries == 1, "entries statistics") ||
        !expect(stats.inserts == 1, "inserts count only new keys") ||
        !expect(stats.hits == 2, "peek and find count as hits") ||
        !expect(stats.misses == 0 && stats.evictions == 0, "no misses or evictions yet")) {
        return 6;
    }
    if (!expect(store.find(VirtualItemId{99}) == nullptr, "missing key returns null") ||
        !expect(store.stats().misses == 1, "miss is counted")) {
        return 7;
    }

    // Item state survives a recycle: the node is discarded, then rebuilt from the store by key.
    ItemStateStore recycling(8);
    const VirtualItemId keyed{0x1001};
    ItemState keyed_state;
    keyed_state.revision = 3;
    keyed_state.payload = 0xDEADBEEFULL;
    keyed_state.measured_main = 48.0f;
    keyed_state.measured_cross = 320.0f;
    if (!expect(recycling.put(keyed, keyed_state), "seed keyed item state")) {
        return 8;
    }
    const auto build = [&recycling](VirtualItemId item) {
        FakeNode node;
        if (const ItemState* item_state = recycling.find(item)) {
            node.payload = item_state->payload;
            node.measured_main = item_state->measured_main;
        }
        return node;
    };
    FakeNode first = build(keyed);
    if (!expect(first.payload == 0xDEADBEEFULL, "first build binds keyed state") ||
        !expect(near(first.measured_main, 48.0f), "first build binds the measure")) {
        return 9;
    }
    first = FakeNode{}; // recycle: the node keeps no item state
    const FakeNode second = build(keyed);
    if (!expect(second.payload == 0xDEADBEEFULL, "state survives recycle") ||
        !expect(near(second.measured_main, 48.0f), "measure survives recycle")) {
        return 10;
    }
    // A recycled slot rebuilt for another key must not leak the previous item's state.
    const FakeNode other = build(VirtualItemId{0x2002});
    if (!expect(other.payload == 0 && near(other.measured_main, 0.0f),
                "a new key starts empty, never recycled state")) {
        return 11;
    }

    // LRU: the least recently found key is the one evicted when capacity is exceeded.
    ItemStateStore limited(3);
    for (std::uint64_t value = 1; value <= 3; ++value) {
        limited.put(VirtualItemId{value}, ItemState{0, value, 0.0f, 0.0f});
    }
    if (!expect(limited.find(VirtualItemId{1}) != nullptr, "refresh key 1")) {
        return 12;
    }
    if (!expect(limited.put(VirtualItemId{4}, ItemState{0, 4, 0.0f, 0.0f}), "insert key 4")) {
        return 13;
    }
    stats = limited.stats();
    if (!expect(!limited.contains(VirtualItemId{2}), "least recently used key evicted") ||
        !expect(limited.contains(VirtualItemId{1}) && limited.contains(VirtualItemId{3}) &&
                    limited.contains(VirtualItemId{4}),
                "remaining keys survive eviction") ||
        !expect(stats.evictions == 1 && stats.entries == 3, "eviction statistics") ||
        !expect(stats.peak_entries == 3, "peak stays at capacity")) {
        return 14;
    }

    // peek does not refresh recency, so the untouched oldest entry is evicted.
    ItemStateStore peeking(2);
    peeking.put(VirtualItemId{1}, ItemState{0, 1, 0.0f, 0.0f});
    peeking.put(VirtualItemId{2}, ItemState{0, 2, 0.0f, 0.0f});
    if (!expect(peeking.peek(VirtualItemId{1}) != nullptr, "peek finds key 1") ||
        !expect(peeking.put(VirtualItemId{3}, ItemState{0, 3, 0.0f, 0.0f}), "insert key 3")) {
        return 15;
    }
    if (!expect(!peeking.contains(VirtualItemId{1}), "peek did not refresh key 1") ||
        !expect(peeking.contains(VirtualItemId{2}) && peeking.contains(VirtualItemId{3}),
                "key 2 and 3 survive")) {
        return 16;
    }

    // find does refresh recency, so the oldest other entry is evicted instead.
    ItemStateStore touching(2);
    touching.put(VirtualItemId{1}, ItemState{0, 1, 0.0f, 0.0f});
    touching.put(VirtualItemId{2}, ItemState{0, 2, 0.0f, 0.0f});
    if (!expect(touching.find(VirtualItemId{1}) != nullptr, "touch key 1") ||
        !expect(touching.put(VirtualItemId{3}, ItemState{0, 3, 0.0f, 0.0f}), "insert key 3") ||
        !expect(!touching.contains(VirtualItemId{2}), "find refreshed key 1") ||
        !expect(touching.contains(VirtualItemId{1}) && touching.contains(VirtualItemId{3}),
                "touched key survives")) {
        return 17;
    }

    // Overwriting an existing key never evicts, even at capacity.
    ItemStateStore overwriting(2);
    overwriting.put(VirtualItemId{1}, ItemState{0, 1, 0.0f, 0.0f});
    overwriting.put(VirtualItemId{2}, ItemState{0, 2, 0.0f, 0.0f});
    if (!expect(!overwriting.put(VirtualItemId{1}, ItemState{1, 11, 0.0f, 0.0f}),
                "overwrite reports an update") ||
        !expect(overwriting.size() == 2 && overwriting.stats().evictions == 0,
                "overwrite does not evict") ||
        !expect(overwriting.peek(VirtualItemId{1})->payload == 11, "overwrite stored the value")) {
        return 18;
    }

    // Erase keeps the probe chains valid: every remaining key is still reachable.
    ItemStateStore chained(8);
    for (std::uint64_t value = 0; value < 8; ++value) {
        chained.put(VirtualItemId{value}, ItemState{0, value, 0.0f, 0.0f});
    }
    if (!expect(chained.erase(VirtualItemId{3}), "erase existing key") ||
        !expect(!chained.erase(VirtualItemId{3}), "erase missing key reports false") ||
        !expect(chained.size() == 7, "size after erase")) {
        return 19;
    }
    for (std::uint64_t value = 0; value < 8; ++value) {
        const bool present = chained.contains(VirtualItemId{value});
        if (!expect(present == (value != 3), "remaining keys reachable after erase")) {
            return 20;
        }
    }
    if (!expect(chained.put(VirtualItemId{3}, ItemState{0, 30, 0.0f, 0.0f}),
                "reinsert erased key") ||
        !expect(chained.size() == 8, "size after reinsert") ||
        !expect(chained.peek(VirtualItemId{3})->payload == 30, "reinserted value")) {
        return 21;
    }

    // clear() drops entries but keeps capacity and cumulative statistics.
    const ItemStateStore::Stats before_clear = chained.stats();
    chained.clear();
    if (!expect(chained.size() == 0 && chained.capacity() == 8, "clear keeps capacity") ||
        !expect(chained.stats().entries == 0, "clear drops entries") ||
        !expect(chained.stats().inserts == before_clear.inserts, "clear keeps counters") ||
        !expect(!chained.contains(VirtualItemId{0}), "clear removes all keys") ||
        !expect(chained.put(VirtualItemId{9}, ItemState{0, 9, 0.0f, 0.0f}),
                "store reused after clear") ||
        !expect(chained.size() == 1 && chained.contains(VirtualItemId{9}), "reuse works")) {
        return 22;
    }

    // Capacity one is the lower bound and still evicts correctly.
    ItemStateStore tiny(1);
    tiny.put(VirtualItemId{1}, ItemState{0, 1, 0.0f, 0.0f});
    if (!expect(tiny.put(VirtualItemId{2}, ItemState{0, 2, 0.0f, 0.0f}), "capacity one insert") ||
        !expect(tiny.size() == 1 && !tiny.contains(VirtualItemId{1}) &&
                    tiny.contains(VirtualItemId{2}),
                "capacity one evicts the previous key")) {
        return 23;
    }

    // The eviction order is deterministic across identical runs.
    const auto run = []() {
        ItemStateStore deterministic_run(4);
        for (std::uint64_t value = 0; value < 8; ++value) {
            deterministic_run.put(VirtualItemId{value}, ItemState{0, value, 0.0f, 0.0f});
        }
        std::array<std::uint64_t, 4> survivors{};
        std::size_t written = 0;
        for (std::uint64_t value = 0; value < 8; ++value) {
            if (const ItemState* item_state = deterministic_run.peek(VirtualItemId{value})) {
                survivors[written++] = item_state->payload;
            }
        }
        return survivors;
    };
    if (!expect(run() == run(), "eviction order is deterministic")) {
        return 24;
    }

    return 0;
}
