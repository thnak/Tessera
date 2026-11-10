// SPDX-License-Identifier: MIT

#include "tessera/ui/virtual/item_state_store.hpp"

#include <array>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <span>
#include <utility>

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

    // Keyed focus (spec section 7): one key at a time, mirrored on the entry, never evicted.
    ItemStateStore focus_store(4);
    if (!expect(!focus_store.has_focus(), "no focus by default") ||
        !expect(!focus_store.clear_focus(), "clearing absent focus reports false") ||
        !expect(!focus_store.is_focused(VirtualItemId{1}), "unknown key is not focused")) {
        return 25;
    }
    if (!expect(focus_store.set_focus(VirtualItemId{2}), "focus a new key") ||
        !expect(focus_store.has_focus() && focus_store.focused_key() == VirtualItemId{2},
                "focused key is reported") ||
        !expect(focus_store.is_focused(VirtualItemId{2}), "focused key matches") ||
        !expect(focus_store.peek(VirtualItemId{2})->focused, "entry mirrors the focus") ||
        !expect(focus_store.size() == 1, "focus inserted a stub entry")) {
        return 26;
    }
    if (!expect(focus_store.set_focus(VirtualItemId{3}), "move focus") ||
        !expect(!focus_store.is_focused(VirtualItemId{2}) &&
                    !focus_store.peek(VirtualItemId{2})->focused,
                "previous entry loses the focus mirror") ||
        !expect(focus_store.is_focused(VirtualItemId{3}), "new key is focused") ||
        !expect(focus_store.set_focus(VirtualItemId{3}), "re-focusing the same key is a no-op") ||
        !expect(focus_store.size() == 2, "re-focusing does not insert")) {
        return 27;
    }
    if (!expect(focus_store.clear_focus(), "clear focus") ||
        !expect(!focus_store.has_focus() && !focus_store.is_focused(VirtualItemId{3}),
                "focus cleared") ||
        !expect(!focus_store.peek(VirtualItemId{3})->focused, "mirror cleared")) {
        return 28;
    }

    // The focused entry is pinned: a new insert evicts the unpinned LRU entry instead of focus.
    ItemStateStore pinned(2);
    pinned.put(VirtualItemId{1}, ItemState{0, 11, 0.0f, 0.0f});
    pinned.put(VirtualItemId{2}, ItemState{0, 22, 0.0f, 0.0f});
    if (!expect(pinned.set_focus(VirtualItemId{1}), "focus key 1") ||
        !expect(pinned.put(VirtualItemId{3}, ItemState{0, 33, 0.0f, 0.0f}), "insert key 3") ||
        !expect(pinned.contains(VirtualItemId{1}) && pinned.is_focused(VirtualItemId{1}),
                "focus survives eviction") ||
        !expect(pinned.peek(VirtualItemId{1})->payload == 11, "focused state is intact") ||
        !expect(!pinned.contains(VirtualItemId{2}), "the unpinned LRU entry was evicted")) {
        return 29;
    }
    // With capacity one and a pinned entry there is nothing to evict, so the insert is refused
    // instead of silently dropping focus.
    ItemStateStore pinned_one(1);
    if (!expect(pinned_one.set_focus(VirtualItemId{7}), "focus the only entry") ||
        !expect(!pinned_one.put(VirtualItemId{8}, ItemState{}), "insert is refused") ||
        !expect(pinned_one.stats().refusals == 1, "refusal is counted") ||
        !expect(pinned_one.size() == 1 && pinned_one.is_focused(VirtualItemId{7}),
                "focus and size are unchanged by the refusal") ||
        !expect(!pinned_one.set_selected(VirtualItemId{9}, true),
                "selection insert is refused as well") ||
        !expect(pinned_one.clear_focus(), "focus can be released") ||
        !expect(pinned_one.put(VirtualItemId{8}, ItemState{}), "insert succeeds after releasing") ||
        !expect(pinned_one.size() == 1 && pinned_one.contains(VirtualItemId{8}),
                "released entry is evicted")) {
        return 30;
    }

    // Selection: set semantics, idempotent, bounded by capacity, pinned against eviction.
    ItemStateStore selection(4);
    if (!expect(!selection.is_selected(VirtualItemId{1}), "nothing is selected by default") ||
        !expect(selection.selected_count() == 0, "empty selection count") ||
        !expect(selection.set_selected(VirtualItemId{3}, true), "select key 3") ||
        !expect(selection.set_selected(VirtualItemId{1}, true), "select key 1") ||
        !expect(selection.selected_count() == 2, "two selected") ||
        !expect(selection.set_selected(VirtualItemId{3}, true), "selecting twice is idempotent") ||
        !expect(selection.selected_count() == 2, "idempotent select keeps the count") ||
        !expect(selection.is_selected(VirtualItemId{3}), "selected flag mirrors") ||
        !expect(!selection.set_selected(VirtualItemId{42}, false), "clearing absent selection") ||
        !expect(!selection.toggle_selected(VirtualItemId{1}), "toggle turns key 1 off") ||
        !expect(selection.selected_count() == 1, "toggle updates the count") ||
        !expect(selection.toggle_selected(VirtualItemId{1}), "toggle turns key 1 back on") ||
        !expect(selection.selected_count() == 2, "toggle restores the count")) {
        return 31;
    }
    const auto selected_keys = [&selection]() {
        std::array<VirtualItemId, 8> buffer{};
        const std::size_t written = selection.copy_selected(buffer);
        return std::pair<std::size_t, std::array<VirtualItemId, 8>>{written, buffer};
    };
    const auto [written, buffer] = selected_keys();
    if (!expect(written == 2, "copy_selected writes every selected key") ||
        !expect(buffer[0] == VirtualItemId{3} && buffer[1] == VirtualItemId{1},
                "copy_selected follows entry order") ||
        !expect(selection.set_selected(VirtualItemId{9}, true),
                "selecting with spare capacity succeeds")) {
        return 32;
    }
    // Two selected entries at capacity pin every slot, so a third selection is refused.
    ItemStateStore full_selection(2);
    full_selection.set_selected(VirtualItemId{1}, true);
    full_selection.set_selected(VirtualItemId{2}, true);
    if (!expect(!full_selection.set_selected(VirtualItemId{3}, true),
                "selection is capacity-bounded") ||
        !expect(full_selection.selected_count() == 2 &&
                    !full_selection.is_selected(VirtualItemId{3}),
                "refused selection changes nothing") ||
        !expect(!full_selection.put(VirtualItemId{4}, ItemState{}),
                "payload insert is refused while every entry is selected") ||
        !expect(full_selection.stats().refusals == 2, "both refusals are counted") ||
        !expect(full_selection.set_selected(VirtualItemId{2}, false), "deselect key 2") ||
        !expect(full_selection.put(VirtualItemId{4}, ItemState{0, 44, 0.0f, 0.0f}),
                "insert succeeds after deselecting") ||
        !expect(full_selection.is_selected(VirtualItemId{1}) &&
                    !full_selection.contains(VirtualItemId{2}),
                "the deselected entry was the eviction victim") ||
        !expect(full_selection.peek(VirtualItemId{4})->payload == 44, "inserted payload") ||
        !expect(full_selection.clear_selection() && full_selection.selected_count() == 0,
                "clear_selection drops every flag") ||
        !expect(!full_selection.clear_selection(), "clearing an empty selection reports false")) {
        return 33;
    }

    // put() copies payload/measure but never moves store-owned focus/selection; erase() keeps the
    // focus/selection bookkeeping consistent; retain_if() prunes by key and drops focus with it.
    ItemStateStore bookkeeping(8);
    bookkeeping.set_focus(VirtualItemId{5});
    bookkeeping.set_selected(VirtualItemId{6}, true);
    if (!expect(!bookkeeping.put(VirtualItemId{5}, ItemState{1, 55, 4.0f, 8.0f}),
                "put overwrites the focused entry") ||
        !expect(bookkeeping.is_focused(VirtualItemId{5}), "put preserved focus") ||
        !expect(bookkeeping.peek(VirtualItemId{5})->payload == 55, "put stored the payload") ||
        !expect(!bookkeeping.put(VirtualItemId{6}, ItemState{2, 66, 0.0f, 0.0f}),
                "put overwrites the selected entry") ||
        !expect(bookkeeping.is_selected(VirtualItemId{6}) && bookkeeping.selected_count() == 1,
                "put preserved selection") ||
        !expect(bookkeeping.erase(VirtualItemId{6}) && bookkeeping.selected_count() == 0,
                "erase drops the selection count") ||
        !expect(bookkeeping.erase(VirtualItemId{5}) && !bookkeeping.has_focus(),
                "erase drops focus") ||
        !expect(bookkeeping.set_focus(VirtualItemId{1}) &&
                    bookkeeping.set_selected(VirtualItemId{2}, true) &&
                    bookkeeping.set_selected(VirtualItemId{3}, true),
                "seed entries for pruning")) {
        return 34;
    }
    bookkeeping.retain_if([](VirtualItemId key) { return key.value != 2; });
    if (!expect(bookkeeping.has_focus() && bookkeeping.focused_key() == VirtualItemId{1},
                "retained focus survives pruning") ||
        !expect(bookkeeping.selected_count() == 1 && bookkeeping.is_selected(VirtualItemId{3}),
                "retained selection survives pruning") ||
        !expect(!bookkeeping.contains(VirtualItemId{2}), "pruned key is gone")) {
        return 35;
    }
    bookkeeping.retain_if([](VirtualItemId key) { return key.value != 1; });
    if (!expect(!bookkeeping.has_focus(), "pruning the focused key clears focus") ||
        !expect(bookkeeping.focused_key() == VirtualItemId{}, "focused key reset")) {
        return 36;
    }
    bookkeeping.clear();
    if (!expect(!bookkeeping.has_focus() && bookkeeping.selected_count() == 0,
                "clear resets focus and selection") ||
        !expect(bookkeeping.set_selected(VirtualItemId{4}, true), "select after clear") ||
        !expect(bookkeeping.selected_count() == 1 && bookkeeping.is_selected(VirtualItemId{4}),
                "selection works after clear")) {
        return 37;
    }

    return 0;
}
