// SPDX-License-Identifier: MIT

#include "tessera/ui/scene/dirty_pass.hpp"

#include <iostream>
#include <memory>
#include <stdexcept>

namespace {

using tessera::ui::Dirty;
using tessera::ui::DirtyPass;
using tessera::ui::has_any;
using tessera::ui::Node;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "dirty_pass: " << message << "\n";
    }
    return condition;
}

} // namespace

int main() {
    Node root(1);
    Node* first = &root.add_child(std::make_unique<Node>(2));
    Node* leaf = &first->add_child(std::make_unique<Node>(3));
    Node* sibling = &root.add_child(std::make_unique<Node>(4));

    root.clear_dirty(Dirty::All);
    first->clear_dirty(Dirty::All);
    leaf->clear_dirty(Dirty::All);
    sibling->clear_dirty(Dirty::All);

    leaf->mark_dirty(Dirty::Paint);
    sibling->mark_dirty(Dirty::Render);

    DirtyPass pass;
    pass.collect(root, Dirty::Paint | Dirty::Render);
    if (!expect(pass.nodes().size() == 2, "collect two dirty nodes") ||
        !expect(pass.nodes()[0].node == leaf, "parent-first order: leaf") ||
        !expect(pass.nodes()[0].flags == Dirty::Paint, "leaf paint flags") ||
        !expect(pass.nodes()[1].node == sibling, "parent-first order: sibling") ||
        !expect(pass.nodes()[1].flags == Dirty::Render, "sibling render flags") ||
        !expect(!pass.overflowed(), "no overflow")) {
        return 1;
    }

    pass.collect(root, Dirty::Paint);
    if (!expect(pass.nodes().size() == 1, "domain filter") ||
        !expect(pass.nodes()[0].node == leaf, "only paint node")) {
        return 2;
    }

    pass.collect(root, Dirty::Layout);
    if (!expect(pass.empty(), "clean domain prunes")) {
        return 3;
    }

    pass.clear(root, Dirty::Paint);
    if (!expect(leaf->dirty() == Dirty::None, "clear leaf paint") ||
        !expect(sibling->dirty() == Dirty::Render, "clear leaves other domain") ||
        !expect(!has_any(root.subtree_dirty(), Dirty::Paint), "root paint aggregate cleared") ||
        !expect(has_any(root.subtree_dirty(), Dirty::Render), "root render aggregate remains")) {
        return 4;
    }

    pass.collect(root, Dirty::Paint | Dirty::Render);
    if (!expect(pass.nodes().size() == 1, "only render remains") ||
        !expect(pass.nodes()[0].node == sibling, "remaining render node")) {
        return 5;
    }

    DirtyPass limited(1);
    leaf->mark_dirty(Dirty::Paint);
    limited.collect(root, Dirty::Paint | Dirty::Render);
    if (!expect(limited.nodes().size() == 1, "node cap") ||
        !expect(limited.overflowed(), "overflow reported")) {
        return 6;
    }

    limited.reset();
    if (!expect(limited.empty(), "reset empties") ||
        !expect(!limited.overflowed(), "reset overflow")) {
        return 7;
    }

    bool threw = false;
    try {
        DirtyPass invalid(0);
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    if (!expect(threw, "zero max_nodes rejected")) {
        return 8;
    }

    std::cout << "dirty_pass: ok\n";
    return 0;
}
