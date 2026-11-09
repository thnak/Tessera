// SPDX-License-Identifier: MIT

#include "tessera/ui/scene/node.hpp"

#include <iostream>
#include <memory>

namespace {

using tessera::ui::Dirty;
using tessera::ui::has_flag;
using tessera::ui::Node;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "scene_dirty: " << message << "\n";
    }
    return condition;
}

bool clean(const Node& node) {
    return node.dirty() == Dirty::Clean && node.subtree_dirty() == Dirty::Clean;
}

} // namespace

int main() {
    Node root(1);
    Node* child = &root.add_child(std::make_unique<Node>(2));
    Node* grandchild = &child->add_child(std::make_unique<Node>(3));

    if (!expect(has_flag(root.dirty(), Dirty::Layout), "add_child dirties parent layout") ||
        !expect(has_flag(root.dirty(), Dirty::Paint), "add_child dirties parent paint") ||
        !expect(has_flag(root.dirty(), Dirty::Render), "add_child dirties parent render") ||
        !expect(has_flag(child->dirty(), Dirty::Style), "new child is style-dirty") ||
        !expect(has_flag(root.subtree_dirty(), Dirty::Style),
                "new child style reaches root subtree")) {
        return 1;
    }

    root.clear_dirty(Dirty::All);
    child->clear_dirty(Dirty::All);
    grandchild->clear_dirty(Dirty::All);
    if (!expect(clean(root) && clean(*child) && clean(*grandchild), "clear all dirties")) {
        return 2;
    }

    grandchild->mark_dirty(Dirty::Paint);
    if (!expect(has_flag(grandchild->dirty(), Dirty::Paint), "leaf mark") ||
        !expect(has_flag(child->subtree_dirty(), Dirty::Paint), "parent subtree mark") ||
        !expect(has_flag(root.subtree_dirty(), Dirty::Paint), "root subtree mark") ||
        !expect(child->dirty() == Dirty::Clean, "parent own flags unchanged") ||
        !expect(root.dirty() == Dirty::Clean, "root own flags unchanged")) {
        return 3;
    }

    grandchild->clear_dirty(Dirty::Paint);
    if (!expect(!has_flag(child->subtree_dirty(), Dirty::Paint), "clear leaf clears parent") ||
        !expect(!has_flag(root.subtree_dirty(), Dirty::Paint), "clear leaf clears root")) {
        return 4;
    }

    root.mark_dirty(Dirty::Layout | Dirty::Render);
    if (!expect(has_flag(root.dirty(), Dirty::Layout), "root own layout") ||
        !expect(has_flag(root.subtree_dirty(), Dirty::Render), "root subtree render") ||
        !expect(child->dirty() == Dirty::Clean, "mark root does not dirty child own flags")) {
        return 5;
    }

    root.set_bounds({1, 2, 3, 4});
    if (!expect(has_flag(root.dirty(), Dirty::Paint), "bounds dirties paint") ||
        !expect(has_flag(root.dirty(), Dirty::Render), "bounds dirties render")) {
        return 6;
    }

    root.clear_dirty(Dirty::All);
    root.set_bounds({1, 2, 3, 4});
    if (!expect(root.dirty() == Dirty::Clean, "same bounds does not dirty")) {
        return 7;
    }

    root.set_transform(tessera::rendering::rhi::Mat3::translation(4, 5));
    if (!expect(has_flag(root.dirty(), Dirty::Paint), "transform dirties paint")) {
        return 8;
    }

    root.clear_dirty(Dirty::All);
    root.set_transform(tessera::rendering::rhi::Mat3::translation(4, 5));
    if (!expect(root.dirty() == Dirty::Clean, "same transform does not dirty")) {
        return 9;
    }

    root.set_visible(false);
    if (!expect(has_flag(root.dirty(), Dirty::Layout), "visibility dirties layout") ||
        !expect(has_flag(root.dirty(), Dirty::Paint), "visibility dirties paint") ||
        !expect(has_flag(root.dirty(), Dirty::Render), "visibility dirties render")) {
        return 10;
    }

    root.clear_dirty(Dirty::All);
    root.set_visible(false);
    if (!expect(root.dirty() == Dirty::Clean, "same visibility does not dirty")) {
        return 11;
    }

    std::cout << "scene_dirty: ok\n";
    return 0;
}
