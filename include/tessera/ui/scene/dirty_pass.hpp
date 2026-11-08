// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/ui/dirty.hpp"
#include "tessera/ui/scene/node.hpp"

#include <cstddef>
#include <vector>

namespace tessera::ui {

struct DirtyNode {
    Node* node = nullptr;
    Dirty flags = Dirty::None;
};

class DirtyPass {
public:
    explicit DirtyPass(std::size_t max_nodes = 1024);

    void collect(Node& root, Dirty domain);
    void clear(Node& root, Dirty domain);
    void reset();

    [[nodiscard]] const std::vector<DirtyNode>& nodes() const {
        return nodes_;
    }
    [[nodiscard]] bool empty() const {
        return nodes_.empty();
    }
    [[nodiscard]] bool overflowed() const {
        return overflowed_;
    }

private:
    void visit(Node& node, Dirty domain);
    void clear_visit(Node& node, Dirty domain);

    std::vector<DirtyNode> nodes_;
    std::size_t max_nodes_ = 0;
    bool overflowed_ = false;
};

} // namespace tessera::ui
