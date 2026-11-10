// SPDX-License-Identifier: MIT

#include "tessera/ui/virtual/item_geometry.hpp"
#include "tessera/ui/virtual/item_state_store.hpp"

#include <cstddef>
#include <cstdlib>
#include <iostream>
#include <new>

namespace {

std::size_t g_allocations = 0;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "virtual_allocations: " << message << "\n";
    }
    return condition;
}

} // namespace

// Count allocations so the scroll/refine/state path can be proven allocation-free after setup.
void* operator new(std::size_t size) {
    ++g_allocations;
    if (void* memory = std::malloc(size == 0 ? 1 : size)) {
        return memory;
    }
    throw std::bad_alloc();
}

void operator delete(void* memory) noexcept {
    std::free(memory);
}

void operator delete(void* memory, std::size_t) noexcept {
    std::free(memory);
}

int main() {
    using tessera::ui::virtualization::ItemGeometry;
    using tessera::ui::virtualization::ItemState;
    using tessera::ui::virtualization::ItemStateStore;
    using tessera::ui::virtualization::VirtualItemId;

    ItemGeometry geometry(ItemGeometry::Config{1024, 32.0f, 4.0f});
    ItemStateStore store(64);
    if (!expect(geometry.set_count(512), "setup set_count") ||
        !expect(store.put(VirtualItemId{0}, ItemState{}), "setup state store")) {
        return 1;
    }

    // Warm every path once so lazy CRT/iostate setup is not counted as hot-path work.
    const ItemGeometry::Range warm_window = geometry.visible_range(0.0f, 320.0f);
    const ItemGeometry::MeasureChange warm_change = geometry.refine_measure(0, 64.0f);
    const std::size_t warm_index = geometry.index_at_offset(10.0f);
    const ItemState* warm_state = store.find(VirtualItemId{0});
    if (!expect(!warm_window.empty() && warm_change.changed && warm_index == 0,
                "warm-up geometry paths") ||
        !expect(warm_state != nullptr, "warm-up state path")) {
        return 1;
    }

    const std::size_t baseline = g_allocations;
    bool ok = true;
    for (std::size_t step = 0; step < 500 && ok; ++step) {
        const float scroll = static_cast<float>((step * 37) % 16000);
        const ItemGeometry::Range window = geometry.visible_range(scroll, 320.0f);
        const std::size_t index = (step * 13) % geometry.item_count();
        const ItemGeometry::MeasureChange change =
            geometry.refine_measure(index, 24.0f + static_cast<float>(step % 40));
        store.put(VirtualItemId{step % 96}, ItemState{step, step, change.new_main, 0.0f});
        ok = ok && window.last <= geometry.item_count() && window.first <= window.last;
        ok = ok && geometry.index_at_offset(scroll) < geometry.item_count();
        ok = ok && geometry.total_extent() > 0.0f;
        ok = ok && store.find(VirtualItemId{step % 96}) != nullptr;
        ok = ok && geometry.visible_range(geometry.total_extent(), 320.0f).empty();
    }
    return expect(ok, "hot-path queries stay in range") &&
                   expect(g_allocations == baseline, "hot path performs no allocations")
               ? 0
               : 2;
}
