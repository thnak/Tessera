// SPDX-License-Identifier: MIT
//
// `RealizedWindow`: the realized-table contract that `VirtualRange` used to carry inline
// (docs/architecture/god-type-audit-v1.md section 3.3). These assertions are about the table itself
// - capacity, packed order, the swap-into-the-hole release, the index span and placement - so a
// future change to the window cannot silently alter the release/reuse determinism that
// `VirtualRange`'s documented behavior depends on.

#include "tessera/ui/scene/node.hpp"
#include "tessera/ui/virtual/item_geometry.hpp"
#include "tessera/ui/virtual/node_recycler.hpp"
#include "tessera/ui/virtual/realized_window.hpp"

#include <iostream>
#include <memory>

namespace {

using tessera::ui::Node;
using tessera::ui::virtualization::ItemGeometry;
using tessera::ui::virtualization::NodeRecycler;
using tessera::ui::virtualization::RealizedWindow;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "realized_window: " << message << "\n";
    }
    return condition;
}

NodeRecycler::Lease lease_for(Node& node) {
    NodeRecycler::Lease lease;
    lease.node = &node;
    return lease;
}

// The table is a packed set whose order is build order; a release swaps the tail into the hole, so
// the packed property (and therefore the deterministic reuse order) survives any removal pattern.
bool packed_table_contract() {
    NodeRecycler recycler(4);
    RealizedWindow window(recycler.capacity());
    if (!expect(window.count() == 0 && window.empty(), "a new window is empty") ||
        !expect(window.capacity() == recycler.capacity(), "capacity follows the pool")) {
        return false;
    }

    Node nodes[4] = {Node(1), Node(2), Node(3), Node(4)};
    for (std::size_t i = 0; i < 4; ++i) {
        window.append(i * 10, lease_for(nodes[i]), 5.0f);
    }
    if (!expect(window.count() == 4, "four entries appended") ||
        !expect(window.node_for(20) == &nodes[2], "index 20 maps to its node")) {
        return false;
    }

    // Removing from the middle swaps entry 3 into slot 1, so the table stays packed and index_at
    // reports the moved entry at its new position.
    RealizedWindow::Item removed;
    window.take_at(1, removed);
    if (!expect(removed.index == 10, "take_at returns the removed entry") ||
        !expect(window.count() == 3, "count drops by one") ||
        !expect(window.index_at(1) == 30, "the tail entry moved into the hole") ||
        !expect(window.index_at(3) == std::nullopt, "past the end is empty") ||
        !expect(window.node_for(10) == nullptr, "the removed index is gone")) {
        return false;
    }
    // The span is the inclusive min/max of the live indices, not the table positions.
    const ItemGeometry::Range span = window.index_span();
    return expect(span.first == 0 && span.last == 31, "span covers the live indices") &&
           expect(window.contains(0) && !window.contains(10), "contains follows the table");
}

// Placement is the window's job: main-axis offset comes from the geometry, the cross axis from the
// entry, and a horizontal collection swaps which axis is main.
bool placement_contract() {
    const ItemGeometry::Config config{.capacity = 4, .estimate_main = 10.0f, .gap = 0.0f};
    ItemGeometry geometry(config);
    (void)geometry.set_count(4);

    NodeRecycler recycler(2);
    RealizedWindow window(recycler.capacity());
    Node first(1);
    Node second(2);
    window.append(0, lease_for(first), 7.0f);
    window.append(2, lease_for(second), 9.0f);
    window.place_all(geometry, 5.0f, false);

    // Vertical: y is the main axis, so item 2 sits at 2*10 - 5 = 15.
    if (!expect(first.bounds().y == -5.0f, "first item offset by the scroll") ||
        !expect(second.bounds().y == 15.0f, "second item at its main offset") ||
        !expect(second.bounds().x == 0.0f && second.bounds().width == 9.0f,
                "cross axis carries the measured size")) {
        return false;
    }
    // Horizontal: x and y swap roles.
    window.place_all(geometry, 0.0f, true);
    return expect(second.bounds().x == 20.0f, "horizontal main axis is x") &&
           expect(second.bounds().height == 9.0f, "horizontal cross axis is the measured size");
}

bool empty_span() {
    NodeRecycler recycler(2);
    RealizedWindow window(recycler.capacity());
    const ItemGeometry::Range span = window.index_span();
    return expect(span.empty(), "an empty window reports an empty span");
}

} // namespace

int main() {
    if (!packed_table_contract()) {
        return 1;
    }
    if (!placement_contract()) {
        return 2;
    }
    if (!empty_span()) {
        return 3;
    }
    std::cout << "realized_window: ok\n";
    return 0;
}
