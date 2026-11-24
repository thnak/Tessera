// SPDX-License-Identifier: MIT
//
// The pull-side accessibility provider (issue #189): the Android shape, testable on every lane.
//
// What a device lane cannot see cheaply is what these prove: that a virtual id survives a rebuild
// that reordered the tree, that a stale id is answered with false instead of a wrong node, that a
// text field's typed content is its value and not its label, and that a flooding client drops its
// own requests instead of growing the queue.

#include "tessera/ui/accessibility/snapshot_provider.hpp"

#include <array>
#include <iostream>
#include <string>
#include <vector>

namespace {

namespace a11y = tessera::ui::a11y;
using a11y::A11yNode;
using a11y::A11ySnapshot;
using a11y::ProviderAnswer;
using a11y::SnapshotProvider;

int failures = 0;

void check(bool condition, const char* name) {
    if (!condition) {
        std::cerr << "FAIL " << name << "\n";
        ++failures;
    }
}

void set_text(std::array<char, a11y::kMaxTextBytes>& field, const char* text) {
    for (std::size_t i = 0; text[i] != '\0' && i < field.size(); ++i) {
        field[i] = text[i];
    }
}

// One node with an id, a role and an optional name.
A11yNode make_node(tessera::ui::NodeId id, a11y::Role role, const char* name = "") {
    A11yNode node;
    node.id = id;
    node.role = role;
    if (name[0] != '\0') {
        set_text(node.name, name);
    }
    return node;
}

// A three-node tree: a root with two children.
A11ySnapshot three_nodes() {
    A11ySnapshot snapshot;
    snapshot.nodes.resize(3);
    snapshot.nodes[0] = make_node(100, a11y::Role::Generic, "root");
    snapshot.nodes[0].children[0] = 1;
    snapshot.nodes[0].children[1] = 2;
    snapshot.nodes[0].child_count = 2;
    snapshot.nodes[1] = make_node(101, a11y::Role::Button, "Save");
    snapshot.nodes[2] = make_node(102, a11y::Role::TextField, "Email");
    return snapshot;
}

} // namespace

int main() {
    // --- An empty provider answers honestly ----------------------------------------------------
    {
        SnapshotProvider provider;
        check(!provider.has_snapshot(), "a fresh provider has no snapshot");
        check(provider.count() == 0, "and no nodes");
        ProviderAnswer answer;
        check(!provider.describe(1, answer), "describing an unknown id is false, not a guess");
        std::vector<int> out(4);
        check(provider.children_of(a11y::kHostVirtualId, out) == 0, "the host has no children yet");
    }

    // --- Publishing makes the tree addressable from 1 -----------------------------------------
    {
        SnapshotProvider provider;
        provider.publish(three_nodes());
        check(provider.has_snapshot(), "a published provider has a snapshot");
        check(provider.count() == 3, "and counts every node");
        check(provider.publishes() == 1, "and counts the publish");
        // The host view is 0 and nodes start at 1, so no node can be confused with the host.
        ProviderAnswer answer;
        check(provider.describe(1, answer), "virtual id 1 is addressable");
        check(answer.node_id == 100, "and is the root");
        check(provider.describe(2, answer), "virtual id 2 is addressable");
        check(answer.node_id == 101, "and is the first child");
        check(!provider.describe(4, answer), "an id past the tree is unknown");
        check(!provider.describe(a11y::kHostVirtualId, answer),
              "the host itself is not a snapshot node");
    }

    // --- Children ------------------------------------------------------------------------------
    {
        SnapshotProvider provider;
        provider.publish(three_nodes());
        std::vector<int> roots(8);
        const std::size_t root_count = provider.children_of(a11y::kHostVirtualId, roots);
        check(root_count == 1, "the host has exactly the one root");
        check(roots[0] == 1, "and it is the root's virtual id");
        std::vector<int> children(8);
        const std::size_t child_count = provider.children_of(1, children);
        check(child_count == 2, "the root has two children");
        check(children[0] == 2 && children[1] == 3, "in child order");
        check(provider.children_of(2, children) == 0, "a leaf has no children");
    }

    // --- A virtual id survives a rebuild that reordered the tree ------------------------------
    // This is the whole point of the slot table: a screen reader focused on virtual view 3 must
    // still find the same control after the tree is rebuilt in a different order.
    {
        SnapshotProvider provider;
        provider.publish(three_nodes());
        const tessera::ui::NodeId before = provider.node_of(3);
        check(before == 102, "virtual id 3 is the text field");

        // Rebuild with the children in the opposite order, and the root last in pre-order.
        A11ySnapshot reordered;
        reordered.nodes.resize(3);
        reordered.nodes[0] = make_node(100, a11y::Role::Generic, "root");
        reordered.nodes[0].children[0] = 2;
        reordered.nodes[0].children[1] = 1;
        reordered.nodes[0].child_count = 2;
        reordered.nodes[1] = make_node(102, a11y::Role::TextField, "Email");
        reordered.nodes[2] = make_node(101, a11y::Role::Button, "Save");
        provider.publish(reordered);

        check(provider.node_of(3) == 102, "the text field kept its virtual id across the rebuild");
        check(provider.node_of(2) == 101, "and so did the button");
        check(provider.count() == 3, "the count is unchanged");
        check(provider.publishes() == 2, "two publishes");
    }

    // --- A node that left the tree frees its slot ---------------------------------------------
    {
        SnapshotProvider provider;
        provider.publish(three_nodes());
        A11ySnapshot smaller;
        smaller.nodes.resize(1);
        smaller.nodes[0] = make_node(100, a11y::Role::Generic, "root");
        provider.publish(smaller);
        check(provider.count() == 1, "the tree shrank");
        check(provider.node_of(2) == 0, "a slot that is gone answers 0");
        ProviderAnswer answer;
        check(!provider.describe(2, answer), "and describes as unknown");
        // A stale id from the old tree is a normal race, never an error: an action for it is
        // refused rather than applied to whatever took its place.
        check(!provider.post_action(2, a11y::Action::Press), "and cannot be acted on");
    }

    // --- A text field reports its content as the VALUE ---------------------------------------
    {
        SnapshotProvider provider;
        A11ySnapshot snapshot;
        snapshot.nodes.resize(1);
        A11yNode field = make_node(200, a11y::Role::TextField, "Email");
        set_text(field.value, "me@example.com");
        snapshot.nodes[0] = field;
        provider.publish(snapshot);

        ProviderAnswer answer;
        check(provider.describe(1, answer), "the field is addressable");
        check(answer.editable, "a text field is editable");
        check(answer.text == "Email", "its name is the label");
        check(answer.description == "me@example.com", "and its content is the value");
    }

    // --- Disabled is inverted, checkable comes from the role ----------------------------------
    {
        SnapshotProvider provider;
        A11ySnapshot snapshot;
        snapshot.nodes.resize(2);
        snapshot.nodes[0] = make_node(300, a11y::Role::Button, "Disabled");
        snapshot.nodes[0].states = a11y::State::Disabled;
        // A TextView with a stale Checked bit must not announce as a checkbox.
        snapshot.nodes[1] = make_node(301, a11y::Role::Text, "Text");
        snapshot.nodes[1].states = a11y::State::Checked;
        provider.publish(snapshot);

        ProviderAnswer answer;
        check(provider.describe(1, answer), "the button is addressable");
        check(!answer.enabled, "a Disabled node is not enabled");
        check(provider.describe(2, answer), "the text is addressable");
        check(!answer.checkable, "text is not checkable even with a Checked bit");
        check(!answer.checked, "and does not report checked");
    }

    // --- Focus ---------------------------------------------------------------------------------
    {
        SnapshotProvider provider;
        A11ySnapshot snapshot = three_nodes();
        check(provider.focused_virtual_id() == a11y::kHostVirtualId,
              "nothing is focused in an empty provider");
        provider.publish(snapshot);
        check(provider.focused_virtual_id() == a11y::kHostVirtualId, "and nothing is focused here");
        snapshot.nodes[1].states = a11y::State::Focused;
        provider.publish(snapshot);
        check(provider.focused_virtual_id() == 2, "the focused node's virtual id is reported");
    }

    // --- Actions are queued, drained, and bounded ---------------------------------------------
    {
        SnapshotProvider provider;
        provider.publish(three_nodes());
        check(provider.post_action(1, a11y::Action::Press), "an action on a live node is queued");
        check(provider.post_action(1, a11y::Action::Focus), "and so is a second");
        check(!provider.post_action(1, a11y::Action::None), "an empty action is refused");

        std::vector<a11y::ProviderAction> batch(8);
        const std::size_t taken = provider.take_actions(batch);
        check(taken == 2, "both actions drain");
        check(batch[0].node == 100, "the first carries its scene node");
        check(batch[0].action == a11y::Action::Press, "and its action");
        check(batch[1].action == a11y::Action::Focus, "the second kept its order");
        check(provider.take_actions(batch) == 0, "a second drain finds nothing");
    }
    {
        SnapshotProvider provider;
        provider.publish(three_nodes());
        // Fill past the cap: the provider must drop rather than grow.
        std::size_t accepted = 0;
        for (std::size_t i = 0; i < a11y::kMaxProviderActions + 5; ++i) {
            if (provider.post_action(1, a11y::Action::Press)) {
                ++accepted;
            }
        }
        check(accepted == a11y::kMaxProviderActions, "the queue is bounded at its cap");
        check(provider.dropped_actions() == 5, "and every refused request is counted");
        std::vector<a11y::ProviderAction> batch(a11y::kMaxProviderActions);
        check(provider.take_actions(batch) == a11y::kMaxProviderActions, "the cap's worth drains");
    }

    // --- A partial drain keeps the rest -------------------------------------------------------
    {
        SnapshotProvider provider;
        provider.publish(three_nodes());
        for (int i = 0; i < 4; ++i) {
            (void)provider.post_action(1, a11y::Action::Press);
        }
        std::vector<a11y::ProviderAction> small(2);
        check(provider.take_actions(small) == 2, "a small buffer takes two");
        std::vector<a11y::ProviderAction> rest(8);
        check(provider.take_actions(rest) == 2, "the other two are still queued");
    }

    // --- clear() drops everything --------------------------------------------------------------
    {
        SnapshotProvider provider;
        provider.publish(three_nodes());
        (void)provider.post_action(1, a11y::Action::Press);
        provider.clear();
        check(!provider.has_snapshot(), "clear drops the snapshot");
        check(provider.count() == 0, "and the nodes");
        std::vector<a11y::ProviderAction> batch(4);
        check(provider.take_actions(batch) == 0, "and the queued actions");
        // Safe twice.
        provider.clear();
    }

    // --- A snapshot past the cap is truncated, not overrun -----------------------------------
    {
        SnapshotProvider provider;
        A11ySnapshot big;
        big.nodes.resize(a11y::kMaxNodes + 10);
        for (std::size_t i = 0; i < big.nodes.size(); ++i) {
            big.nodes[i] = make_node(1000 + i, a11y::Role::Generic);
        }
        provider.publish(big);
        check(provider.count() == a11y::kMaxNodes, "a snapshot past the cap is truncated");
    }

    // --- A SetValue request carries its text, and only it can ------------------------------
    {
        SnapshotProvider provider;
        A11ySnapshot snapshot;
        snapshot.nodes.resize(1);
        snapshot.nodes[0] = make_node(300, a11y::Role::TextField, "Email");
        provider.publish(snapshot);

        check(provider.post_action(1, a11y::Action::SetValue, "typed by a screen reader"),
              "a SetValue request is accepted");
        std::array<a11y::ProviderAction, a11y::kMaxProviderActions> drained{};
        const std::size_t taken = provider.take_actions(drained);
        check(taken == 1, "and drains as one action");
        check(taken == 1 && drained[0].action == a11y::Action::SetValue,
              "with the action it was queued as");
        check(taken == 1 && drained[0].text_of() == "typed by a screen reader",
              "and the payload the platform sent");

        // The payload is what made Android's SetText work at all; a request without one would be
        // honoured as "replace the text with nothing", so an empty payload is legitimate and the
        // distinction a caller must make is the action, not the string.
        check(provider.post_action(1, a11y::Action::SetValue, ""),
              "an empty SetValue payload is a real request (clear the field)");

        // A payload on an action that does not carry one is refused rather than silently ignored:
        // answering true would tell the platform the request succeeded.
        check(!provider.post_action(1, a11y::Action::Press, "not a press payload"),
              "a payload on Press is refused");
        check(provider.post_action(1, a11y::Action::Press), "and Press without one is accepted");

        // A payload at or over the bound is refused, not truncated: a screen reader holding a
        // trimmed field text would disagree with the engine's buffer.
        std::string oversized(a11y::kMaxProviderActionTextBytes, 'x');
        check(!provider.post_action(1, a11y::Action::SetValue, oversized),
              "an oversized payload is refused, never truncated");
        // Drain what the refusals above left behind, so this reads the entry it just queued rather
        // than the oldest one still waiting.
        (void)provider.take_actions(drained);
        std::string at_limit(a11y::kMaxProviderActionTextBytes - 1, 'y');
        check(provider.post_action(1, a11y::Action::SetValue, at_limit),
              "a payload one byte under the bound is accepted");
        const std::size_t after = provider.take_actions(drained);
        check(after == 1 && drained[0].text_of() == at_limit,
              "and keeps every byte it was given");
    }

    if (failures != 0) {
        std::cerr << "snapshot_provider: " << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "snapshot_provider: ok\n";
    return 0;
}
