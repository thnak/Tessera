// SPDX-License-Identifier: MIT

// `Node::clear_subtree_dirty` (what `DirtyPass::clear` calls) clears a dirty domain over a subtree
// in one post-order pass. The contract under test is that it leaves every node exactly as the
// per-node `clear_dirty` walk it replaced did: the same own flags, the same subtree summaries, on
// the cleared subtree and on its ancestors. The reference below is that old walk, written against
// the public API.

#include "tessera/ui/scene/dirty_pass.hpp"
#include "tessera/ui/scene/node.hpp"

#include <cstdint>
#include <iostream>
#include <memory>
#include <vector>

namespace {

using tessera::ui::Dirty;
using tessera::ui::has_any;
using tessera::ui::Node;
using tessera::ui::NodeId;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "scene_dirty_clear: " << message << "\n";
    }
    return condition;
}

class Random {
public:
    explicit Random(std::uint64_t seed)
        : state_(seed) {}
    std::uint32_t below(std::uint32_t n) {
        state_ = state_ * 6364136223846793005ULL + 1442695040888963407ULL;
        return static_cast<std::uint32_t>(state_ >> 33) % n;
    }

private:
    std::uint64_t state_;
};

// The walk `DirtyPass::clear` used to do: clear the node, then its children, skipping subtrees that
// carry none of the domain.
void reference_clear(Node& node, Dirty domain) {
    if (!has_any(node.subtree_dirty(), domain)) {
        return;
    }
    node.clear_dirty(domain);
    for (const auto& child : node.children()) {
        reference_clear(*child, domain);
    }
}

void collect(Node& node, std::vector<Node*>& out) {
    out.push_back(&node);
    for (const auto& child : node.children()) {
        collect(*child, out);
    }
}

// Same shape from the same seed, so two trees can be mutated identically and compared by index.
std::unique_ptr<Node> make_tree(std::uint64_t seed) {
    Random random(seed);
    auto root = std::make_unique<Node>(1);
    std::vector<Node*> open{root.get()};
    NodeId next_id = 2;
    const std::uint32_t target = 60 + random.below(200);
    while (next_id < target) {
        Node* parent = open[random.below(static_cast<std::uint32_t>(open.size()))];
        // A mix of wide and deep parents: the quadratic case is a parent with many children.
        auto child = std::make_unique<Node>(next_id++);
        open.push_back(&parent->add_child(std::move(child)));
    }
    // `add_child` marks every new node dirty, so a fresh tree is uniformly dirty and every summary
    // equals its node's own flags - a state in which no summary bug can show. Start from clean and
    // let the test mark a sparse random set instead.
    reference_clear(*root, Dirty::Style | Dirty::Layout | Dirty::Paint | Dirty::Render);
    return root;
}

Dirty random_domain(Random& random) {
    Dirty flags = Dirty::Clean;
    const std::uint32_t pick = 1 + random.below(15);
    if ((pick & 1u) != 0) {
        flags |= Dirty::Style;
    }
    if ((pick & 2u) != 0) {
        flags |= Dirty::Layout;
    }
    if ((pick & 4u) != 0) {
        flags |= Dirty::Paint;
    }
    if ((pick & 8u) != 0) {
        flags |= Dirty::Render;
    }
    return flags;
}

bool same_state(Node& lhs_root, Node& rhs_root) {
    std::vector<Node*> lhs;
    std::vector<Node*> rhs;
    collect(lhs_root, lhs);
    collect(rhs_root, rhs);
    if (lhs.size() != rhs.size()) {
        return false;
    }
    for (std::size_t i = 0; i < lhs.size(); ++i) {
        if (lhs[i]->dirty() != rhs[i]->dirty() ||
            lhs[i]->subtree_dirty() != rhs[i]->subtree_dirty()) {
            return false;
        }
    }
    return true;
}

bool test_matches_the_per_node_walk() {
    for (std::uint64_t seed = 1; seed <= 200; ++seed) {
        auto fast = make_tree(seed);
        auto slow = make_tree(seed);
        std::vector<Node*> fast_nodes;
        std::vector<Node*> slow_nodes;
        collect(*fast, fast_nodes);
        collect(*slow, slow_nodes);

        Random random(seed * 31 + 7);
        // Mark a random set of nodes with random domains, identically in both trees.
        const std::uint32_t marks = 1 + random.below(static_cast<std::uint32_t>(fast_nodes.size()));
        for (std::uint32_t i = 0; i < marks; ++i) {
            const std::uint32_t at = random.below(static_cast<std::uint32_t>(fast_nodes.size()));
            const Dirty flags = random_domain(random);
            fast_nodes[at]->mark_dirty(flags);
            slow_nodes[at]->mark_dirty(flags);
        }
        // Clear a random domain from a random subtree root (not always the tree root: the
        // ancestors of the cleared root must be re-derived too).
        for (int round = 0; round < 3; ++round) {
            const std::uint32_t at = random.below(static_cast<std::uint32_t>(fast_nodes.size()));
            const Dirty domain = random_domain(random);
            fast_nodes[at]->clear_subtree_dirty(domain);
            reference_clear(*slow_nodes[at], domain);
            if (!same_state(*fast, *slow)) {
                std::cerr << "scene_dirty_clear: seed " << seed << " round " << round
                          << " left a different state\n";
                return false;
            }
        }
    }
    return true;
}

bool test_dirty_pass_clear_leaves_a_clean_tree() {
    auto root = make_tree(7);
    std::vector<Node*> nodes;
    collect(*root, nodes);
    for (Node* node : nodes) {
        node->mark_dirty(Dirty::Paint | Dirty::Render);
    }
    tessera::ui::DirtyPass pass(nodes.size() + 8);
    pass.collect(*root, Dirty::Paint);
    if (!expect(pass.nodes().size() == nodes.size(), "every node is collected before the clear")) {
        return false;
    }
    pass.clear(*root, Dirty::Paint);
    bool ok = expect(!has_any(root->subtree_dirty(), Dirty::Paint), "no Paint left in the summary");
    // Render was not in the cleared domain and must survive on every node.
    for (Node* node : nodes) {
        ok &= expect(has_any(node->dirty(), Dirty::Render), "an uncleared domain is untouched");
        ok &= expect(!has_any(node->dirty(), Dirty::Paint), "the cleared domain is gone");
    }
    return ok;
}

bool test_clearing_one_child_keeps_the_siblings_summary() {
    // The ancestor summary must stay dirty while another branch still is.
    Node root(1);
    auto left = std::make_unique<Node>(2);
    auto right = std::make_unique<Node>(3);
    Node& left_ref = root.add_child(std::move(left));
    Node& right_ref = root.add_child(std::move(right));
    left_ref.mark_dirty(Dirty::Paint);
    right_ref.mark_dirty(Dirty::Paint);
    left_ref.clear_subtree_dirty(Dirty::Paint);
    return expect(has_any(root.subtree_dirty(), Dirty::Paint),
                  "the other branch keeps the root dirty") &&
           expect(!has_any(left_ref.subtree_dirty(), Dirty::Paint), "the cleared branch is clean");
}

} // namespace

int main() {
    bool ok = true;
    ok &= test_matches_the_per_node_walk();
    ok &= test_dirty_pass_clear_leaves_a_clean_tree();
    ok &= test_clearing_one_child_keeps_the_siblings_summary();
    return ok ? 0 : 1;
}
