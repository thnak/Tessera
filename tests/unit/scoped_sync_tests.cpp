// SPDX-License-Identifier: MIT
//
// The scoped scene sync (issue #735, slice B): when the layout-unchanged gate proves that only the
// restyled subtrees changed, the sync visits those subtrees and the ancestors that refold
// `glass_subtree`, instead of walking the whole tree.
//
// The claim that needs proving is not "it is faster" but "it applies exactly what the full walk
// would". So every case here runs both walks over the same tree and compares the scene state they
// produce: z-index, transform, paint outsets, glass sigma, filter reach and both subtree flags on
// every node. A scoped walk that skipped a node it should have visited shows up as a node whose
// state differs.

#include "tessera/ui/paint/scene_style_sync.hpp"
#include "tessera/ui/scene/node.hpp"
#include "tessera/ui/style/scene_style.hpp"
#include "tessera/ui/style/sync_memo.hpp"

#include <cmath>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace {

using tessera::rendering::gpu::GlassLimits;
using tessera::ui::Node;
using tessera::ui::paint::SceneStyleProvider;
using tessera::ui::paint::SceneStyleSyncResult;
using tessera::ui::paint::sync_scene_style;
using tessera::ui::paint::sync_scene_style_scoped;
using tessera::ui::style::compile_stylesheet;
using tessera::ui::style::SceneStyleTree;
using tessera::ui::style::SyncMemo;

bool expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "scoped_sync: " << message << "\n";
    }
    return condition;
}

constexpr const char* kSheet = R"(
.card { width: 100px; height: 40px; background-color: #1e293b; }
.card.hot { background-color: #334155; }
.card.raised { z-index: 5; transform: translate(4px, 6px); }
.card.shadowed { box-shadow: 0 2px 8px #000000; }
.card.glass { backdrop-filter: blur(6px); }
.card.filtered { filter: blur(3px); }
)";

struct Scene {
    std::unique_ptr<Node> root = std::make_unique<Node>(1);
    std::vector<Node*> cards;
    std::vector<Node*> labels;

    explicit Scene(int card_count) {
        std::uint64_t id = 2;
        for (int i = 0; i < card_count; ++i) {
            Node& card = root->add_child(std::make_unique<Node>(id++));
            card.set_element_kind("View");
            card.set_class_name("card");
            cards.push_back(&card);
            Node& label = card.add_child(std::make_unique<Node>(id++));
            label.set_element_kind("Text");
            labels.push_back(&label);
        }
    }
};

// The scene state the sync is responsible for, flattened in tree order.
struct NodeState {
    std::uint64_t id = 0;
    int z_index = 0;
    float transform[9] = {};
    float outsets[5] = {};
    float glass_sigma = 0.0f;
    bool glass_subtree = false;
    float filter_reach = 0.0f;
    bool filter_subtree = false;
};

void collect_state(const Node& node, std::vector<NodeState>& out) {
    NodeState state;
    state.id = node.id();
    state.z_index = node.z_index();
    for (int i = 0; i < 9; ++i) {
        state.transform[i] = node.transform().m[i];
    }
    const auto& outsets = node.paint_outsets();
    state.outsets[0] = outsets.left;
    state.outsets[1] = outsets.top;
    state.outsets[2] = outsets.right;
    state.outsets[3] = outsets.bottom;
    state.outsets[4] = outsets.blurred ? 1.0f : 0.0f;
    state.glass_sigma = node.glass_blur_sigma();
    state.glass_subtree = node.glass_subtree();
    state.filter_reach = node.filter_reach();
    state.filter_subtree = node.filter_subtree();
    out.push_back(state);
    for (const auto& child : node.children()) {
        collect_state(*child, out);
    }
}

bool same_state(const NodeState& a, const NodeState& b) {
    if (a.id != b.id || a.z_index != b.z_index || a.glass_subtree != b.glass_subtree ||
        a.filter_subtree != b.filter_subtree || a.filter_reach != b.filter_reach) {
        return false;
    }
    if (std::fabs(a.glass_sigma - b.glass_sigma) > 0.001f) {
        return false;
    }
    for (int i = 0; i < 9; ++i) {
        if (std::fabs(a.transform[i] - b.transform[i]) > 0.001f) {
            return false;
        }
    }
    for (int i = 0; i < 5; ++i) {
        if (std::fabs(a.outsets[i] - b.outsets[i]) > 0.001f) {
            return false;
        }
    }
    return true;
}

// Runs both walks over two identical trees and compares the resulting scene state. `restyled` names
// the nodes whose subtree the caller restyled; the scoped walk is given exactly those.
bool compare_walks(const char* label, Scene& full, Scene& scoped,
                   const std::vector<const Node*>& restyled_full,
                   const std::vector<const Node*>& restyled_scoped) {
    const auto sheet = compile_stylesheet(kSheet);
    SceneStyleTree full_tree(*full.root);
    SceneStyleTree scoped_tree(*scoped.root);
    SceneStyleProvider full_provider(sheet, full_tree);
    SceneStyleProvider scoped_provider(sheet, scoped_tree);

    full_tree.begin_memo();
    scoped_tree.begin_memo();
    for (const Node* node : restyled_full) {
        full_tree.restyle_subtree(*node);
    }
    for (const Node* node : restyled_scoped) {
        scoped_tree.restyle_subtree(*node);
    }

    SyncMemo full_memo;
    SyncMemo scoped_memo;
    const SceneStyleSyncResult full_result =
        sync_scene_style(*full.root, full_provider, GlassLimits{}, &full_memo);
    const SceneStyleSyncResult scoped_result = sync_scene_style_scoped(
        scoped_provider, scoped_tree.restyled_roots(), GlassLimits{}, &scoped_memo);

    std::vector<NodeState> full_state;
    std::vector<NodeState> scoped_state;
    collect_state(*full.root, full_state);
    collect_state(*scoped.root, scoped_state);
    if (!expect(full_state.size() == scoped_state.size(),
                std::string(label) + ": both walks see the same tree")) {
        return false;
    }
    for (std::size_t i = 0; i < full_state.size(); ++i) {
        if (!same_state(full_state[i], scoped_state[i])) {
            std::cerr << "scoped_sync: " << label << ": node " << full_state[i].id << " differs (z "
                      << full_state[i].z_index << " vs " << scoped_state[i].z_index
                      << ", glass_subtree " << full_state[i].glass_subtree << " vs "
                      << scoped_state[i].glass_subtree << ")\n";
            return false;
        }
    }
    // The scoped walk must visit strictly fewer nodes than the full one, or it is not scoped at
    // all and this test proves nothing.
    if (!expect(scoped_result.nodes < full_result.nodes,
                std::string(label) + ": the scoped walk visits fewer nodes than the full walk")) {
        std::cerr << "  full=" << full_result.nodes << " scoped=" << scoped_result.nodes << "\n";
        return false;
    }
    full_tree.end_memo();
    scoped_tree.end_memo();
    return true;
}

} // namespace

int main() {
    // 1. A paint-only class change on one card: the scoped walk visits that card's subtree and the
    //    ancestors above it, and the scene ends in the same state as the full walk.
    {
        Scene full(6);
        Scene scoped(6);
        full.cards[2]->set_class_name("card hot");
        scoped.cards[2]->set_class_name("card hot");
        if (!compare_walks("paint-only class change", full, scoped, {full.cards[2]},
                           {scoped.cards[2]})) {
            return 1;
        }
    }

    // 2. A change that moves scene-consumed properties (z-index, transform, shadow): the scoped
    //    walk must apply all of them, not just the paint ones.
    {
        Scene full(6);
        Scene scoped(6);
        full.cards[1]->set_class_name("card raised shadowed");
        scoped.cards[1]->set_class_name("card raised shadowed");
        if (!compare_walks("z-index/transform/shadow change", full, scoped, {full.cards[1]},
                           {scoped.cards[1]})) {
            return 2;
        }
    }

    // 3. Glass: a card that gains `backdrop-filter` must set its own sigma AND refold
    //    `glass_subtree` on every ancestor, which is the one thing the scoped walk cannot skip.
    {
        Scene full(6);
        Scene scoped(6);
        full.cards[4]->set_class_name("card glass");
        scoped.cards[4]->set_class_name("card glass");
        if (!compare_walks("glass gain refolds ancestors", full, scoped, {full.cards[4]},
                           {scoped.cards[4]})) {
            return 3;
        }
    }

    // 4. Glass removal: the ancestor refold must clear the flag again, not just set it.
    {
        Scene full(6);
        Scene scoped(6);
        full.cards[3]->set_class_name("card glass");
        scoped.cards[3]->set_class_name("card glass");
        const auto sheet = compile_stylesheet(kSheet);
        {
            // Prime both trees with the glass card present.
            SceneStyleTree full_tree(*full.root);
            SceneStyleTree scoped_tree(*scoped.root);
            SceneStyleProvider full_provider(sheet, full_tree);
            SceneStyleProvider scoped_provider(sheet, scoped_tree);
            full_tree.begin_memo();
            scoped_tree.begin_memo();
            SyncMemo full_memo;
            SyncMemo scoped_memo;
            (void)sync_scene_style(*full.root, full_provider, GlassLimits{}, &full_memo);
            (void)sync_scene_style_scoped(scoped_provider, scoped_tree.restyled_roots(),
                                          GlassLimits{}, &scoped_memo);
            full_tree.end_memo();
            scoped_tree.end_memo();
        }
        full.cards[3]->set_class_name("card");
        scoped.cards[3]->set_class_name("card");
        if (!compare_walks("glass removal clears ancestors", full, scoped, {full.cards[3]},
                           {scoped.cards[3]})) {
            return 4;
        }
    }

    // 3b. A layer filter has its own field and subtree flag; the scoped refold must keep both
    //     equal to the full walk, and it must not leak into the glass state.
    {
        Scene full(6);
        Scene scoped(6);
        full.cards[2]->set_class_name("card filtered");
        scoped.cards[2]->set_class_name("card filtered");
        if (!compare_walks("filter gain refolds ancestors", full, scoped, {full.cards[2]},
                           {scoped.cards[2]})) {
            return 31;
        }
        if (!scoped.cards[2]->filter_subtree() || scoped.cards[2]->glass_subtree() ||
            !scoped.root->filter_subtree() || scoped.root->glass_subtree()) {
            std::cerr << "filter gain sets only the filter flags\n";
            return 32;
        }
    }

    // 5. Two restyled subtrees at once: both are visited, and the shared ancestors refold once.
    {
        Scene full(8);
        Scene scoped(8);
        full.cards[1]->set_class_name("card raised");
        scoped.cards[1]->set_class_name("card raised");
        full.cards[6]->set_class_name("card glass");
        scoped.cards[6]->set_class_name("card glass");
        if (!compare_walks("two restyled subtrees", full, scoped, {full.cards[1], full.cards[6]},
                           {scoped.cards[1], scoped.cards[6]})) {
            return 5;
        }
    }

    // 6. An empty restyle visits nothing and changes nothing.
    {
        Scene full(4);
        Scene scoped(4);
        const auto sheet = compile_stylesheet(kSheet);
        SceneStyleTree scoped_tree(*scoped.root);
        SceneStyleProvider scoped_provider(sheet, scoped_tree);
        scoped_tree.begin_memo();
        const SceneStyleSyncResult result =
            sync_scene_style_scoped(scoped_provider, {}, GlassLimits{}, nullptr);
        scoped_tree.end_memo();
        if (!expect(result.nodes == 0, "an empty restyle visits no node")) {
            return 6;
        }
    }

    std::cout << "scoped_sync: ok\n";
    return 0;
}
