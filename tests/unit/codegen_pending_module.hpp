// SPDX-License-Identifier: MIT

// Hand-built IR for the pending-UI fixture (ADR-067 spike #661). Shared by the emission test (which
// owns the stored golden through `--write`), the runtime test that compiles it, and the
// differential test that plays one script on the evaluator and on the generated component.
//
// The component is the smallest screen that exercises every rule the ADR asks for:
//
//   const [query, setQuery] = useState("");
//   const [isPending, startTransition] = useTransition();
//   <Suspense fallback={<Text>Loading</Text>}>
//     <View><Text>{query}</Text></View>
//   </Suspense>
//   <Button onClick={startTransition(() => setQuery("next"))}>Search</Button>
//
// so the boundary has one suspending read, the setter carries the transition mark, and the fallback
// and the content are distinguishable by their text payload.

#pragma once

#include "tessera/compiler/ir/expr.hpp"
#include "tessera/compiler/ir/module.hpp"

#include <cstdint>
#include <string>
#include <utility>

namespace tessera::test::codegen_pending {

namespace ir = tessera::compiler::ir;

inline ir::SourceSpan span(std::uint32_t offset, std::uint32_t line, std::uint32_t column) {
    return ir::SourceSpan{"ui/Feed.tsx", offset, 10, line, column, line, column + 10};
}

inline ir::Node leaf(ir::NodeKind kind, std::string id, std::string ui_id, std::uint32_t offset) {
    ir::Node node;
    node.kind = kind;
    node.id = std::move(id);
    node.ui_id = std::move(ui_id);
    node.source = span(offset, 1, 1);
    return node;
}

inline ir::IrModule make_pending_module() {
    ir::IrModule module;
    module.name = "pending";

    ir::Component feed;
    feed.id = "comp::Feed";
    feed.component_id = "81";
    feed.name = "Feed";
    feed.source = span(0, 1, 1);
    feed.state.push_back({"state::query", "query", "string", ir::make_literal(std::string(""))});

    // The root View holds the boundary and the button that starts the transition.
    ir::Node root = leaf(ir::NodeKind::View, "node::1", "801", 20);

    // <Suspense fallback={<Text>Loading</Text>}><View><Text>{query}</Text></View></Suspense>
    ir::Node boundary = leaf(ir::NodeKind::Suspense, "node::2", "802", 30);
    ir::Node content = leaf(ir::NodeKind::View, "node::3", "803", 40);
    ir::Node content_text = leaf(ir::NodeKind::Text, "node::4", "804", 50);
    {
        std::vector<ir::TemplatePart> parts;
        parts.push_back(ir::text_part("Query: "));
        parts.push_back(ir::expr_part(ir::make_ref("state::query")));
        content_text.text = ir::make_template(std::move(parts));
    }
    content.children.push_back(std::move(content_text));
    boundary.children.push_back(std::move(content));
    ir::Node fallback = leaf(ir::NodeKind::Text, "node::5", "805", 60);
    fallback.text = ir::make_literal(std::string("Loading"));
    boundary.fallback = std::make_unique<ir::Node>(std::move(fallback));
    root.children.push_back(std::move(boundary));

    // <Button onClick={handler::search}>Search</Button>: the marked setter. An inline handler, not
    // a `bind::`, because the point of the fixture is the *generated* body: the transition mark is
    // emitted into it, and the differential test drives that same body.
    ir::Node button = leaf(ir::NodeKind::Button, "node::6", "806", 70);
    button.events.emplace("onClick", "handler::search");
    ir::Node button_text = leaf(ir::NodeKind::Text, "node::7", "807", 80);
    button_text.text = ir::make_literal(std::string("Search"));
    button.children.push_back(std::move(button_text));
    root.children.push_back(std::move(button));

    feed.root = std::move(root);

    // The transition-marked setter: `startTransition(() => setQuery("next"))`. Registered before
    // the component is moved into the module, because the event target resolves against it.
    ir::Handler search;
    search.id = "handler::search";
    search.source = span(90, 6, 20);
    search.params.push_back({"event", "ClickEvent"});
    ir::Action set_query;
    set_query.kind = ir::ActionKind::Set;
    set_query.target = "state::query";
    set_query.value = ir::make_literal(std::string("next"));
    set_query.transition = true;
    search.actions.push_back(std::move(set_query));
    feed.handlers.push_back(std::move(search));

    module.components.push_back(std::move(feed));
    return module;
}

} // namespace tessera::test::codegen_pending
