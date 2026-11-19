// SPDX-License-Identifier: MIT

// Shared fixture of the override-layer tests (issue #536): the two-card source, the computed-value
// reader and the session `Fixture`. Included by scene_canvas_override_tests.cpp and
// scene_canvas_override_commit_tests.cpp.

#pragma once

#include "scene_canvas_test_support.hpp"
#include "scene_override.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace canvas_test;

// Two `.card` nodes (one with an inline colour, to prove an override beats the inline origin) and a
// button that changes state, which rebuilds the tree.
constexpr std::string_view kCards = R"tsx(import "./Preview.css";

export function Cards(): Node {
  const [count, setCount] = useState<int>(0);
  return (
    <View>
      <View className="card" style={{ color: "red" }}>
        <Text>One</Text>
      </View>
      <View className="card">
        <Text>Two</Text>
      </View>
      <Button onClick={() => setCount(count + 1)}>Add</Button>
    </View>
  );
}
)tsx";

constexpr std::string_view kCardsCss = ".card { color: blue; padding-left: 2px; }\n";

cli::CanvasSessionSource cards_source() {
    cli::CanvasSessionSource source = source_for(kCards, "ui/Cards.tsx");
    source.stylesheet = kCardsCss;
    source.stylesheet_file = "ui/Preview.css";
    return source;
}

void collect_class(const ui::Node& node, std::string_view class_name,
                   std::vector<const ui::Node*>& out) {
    if (node.class_name() == class_name) {
        out.push_back(&node);
    }
    for (const auto& child : node.children()) {
        collect_class(*child, class_name, out);
    }
}

std::vector<const ui::Node*> cards_of(const cli::CanvasSceneSession& session) {
    std::vector<const ui::Node*> cards;
    collect_class(*session.root(), "card", cards);
    return cards;
}

[[maybe_unused]] std::string computed(const cli::CanvasSceneSession& session,
                                      std::uint64_t automation_id, std::string_view property) {
    ui::style::SceneStyleTree tree(*session.root());
    for (const ui::Node* node : cards_of(session)) {
        if (node->automation_id() == automation_id) {
            const ui::style::ComputedStyle style = tree.compute(session.sheet(), *node);
            const std::string* value = style.find(property);
            return value != nullptr ? *value : std::string();
        }
    }
    return {};
}

[[maybe_unused]] bool has_code(const cli::OverrideOutcome& outcome, std::string_view code) {
    for (const cli::OverrideDiagnostic& diagnostic : outcome.diagnostics) {
        if (diagnostic.code == code) {
            return true;
        }
    }
    return false;
}

// Starts the two-card source and returns the two cards' automation ids.
struct Fixture {
    cli::CanvasSceneSession session;
    std::uint64_t first = 0;
    std::uint64_t second = 0;

    bool start() {
        const std::string error = session.start(cards_source());
        expect(error.empty(), "the cards source starts: " + error);
        const std::vector<const ui::Node*> cards = cards_of(session);
        expect(cards.size() == 2, "the source has two .card nodes");
        if (!error.empty() || cards.size() != 2) {
            return false;
        }
        first = cards[0]->automation_id();
        second = cards[1]->automation_id();
        return true;
    }
};

} // namespace
