// SPDX-License-Identifier: MIT

// The hand-built IR of #656 (ADR-067 item 1): a component that reads a resource through a service
// and covers its suspending read with a Suspense boundary. Nothing lowers `useResource` from TSX
// yet, so this fixture is the writer golden (tests/golden/ir/resource.json) and the base of the
// resource validator tests.

#include "ir_samples.hpp"

#include <utility>
#include <vector>

namespace ir_test {
namespace {

ir::SourceSpan feed_span(std::uint32_t offset, std::uint32_t line, std::uint32_t column) {
    return ir::SourceSpan{"ui/Feed.tsx", offset, 10, line, column, line, column + 10};
}

ir::Node feed_node(ir::NodeKind kind, std::string id, std::string ui_id, std::uint32_t offset,
                   std::uint32_t line) {
    ir::Node node;
    node.kind = kind;
    node.id = std::move(id);
    node.ui_id = std::move(ui_id);
    node.source = feed_span(offset, line, 3);
    return node;
}

ir::ExprPtr text_of(std::string label, std::string ref) {
    std::vector<ir::TemplatePart> parts;
    parts.push_back(ir::text_part(std::move(label)));
    parts.push_back(ir::expr_part(ir::make_ref(std::move(ref))));
    return ir::make_template(std::move(parts));
}

} // namespace

ir::IrModule make_resource_module() {
    ir::IrModule module;
    module.name = "feed";

    ir::Component feed;
    feed.id = "comp::Feed";
    feed.component_id = "71";
    feed.name = "Feed";
    feed.source = feed_span(0, 1, 1);
    feed.state.push_back({"state::query", "query", "string", ir::make_literal(std::string(""))});

    ir::Service api;
    api.id = "svc::api";
    api.name = "api";
    api.type = "demo::Api";
    api.header = "demo/api.hpp";
    api.source = feed_span(10, 2, 3);
    feed.services.push_back(std::move(api));

    // const feed = useResource(api.loadFeed, { query }, { suspend: true });
    ir::Resource suspending;
    suspending.id = "res::feed";
    suspending.service = "svc::api";
    suspending.method = "loadFeed";
    suspending.suspend = true;
    {
        std::vector<ir::RecordField> fields;
        fields.push_back(ir::RecordField{"query", ir::make_ref("state::query")});
        suspending.key = std::move(*ir::make_record("", std::move(fields)));
    }
    suspending.source = feed_span(20, 3, 3);
    feed.resources.push_back(std::move(suspending));

    // const tags = useResource(api.loadTags, query);  (never suspends; the component reads status)
    ir::Resource passive;
    passive.id = "res::tags";
    passive.service = "svc::api";
    passive.method = "loadTags";
    passive.key = std::move(*ir::make_ref("state::query"));
    passive.source = feed_span(30, 4, 3);
    feed.resources.push_back(std::move(passive));

    ir::Node root = feed_node(ir::NodeKind::View, "node::1", "701", 40, 6);
    ir::Node status = feed_node(ir::NodeKind::Text, "node::2", "702", 50, 7);
    status.text = text_of("Tags: ", "res::tags.status");
    root.children.push_back(std::move(status));

    ir::Node boundary = feed_node(ir::NodeKind::Suspense, "node::3", "703", 60, 8);
    ir::Node content = feed_node(ir::NodeKind::View, "node::4", "704", 70, 9);
    ir::Node items = feed_node(ir::NodeKind::Text, "node::5", "705", 80, 10);
    items.text = text_of("Items: ", "res::feed.data");
    content.children.push_back(std::move(items));
    boundary.children.push_back(std::move(content));
    ir::Node fallback = feed_node(ir::NodeKind::Text, "node::6", "706", 90, 12);
    fallback.text = ir::make_literal(std::string("Loading"));
    boundary.fallback = std::make_unique<ir::Node>(std::move(fallback));
    root.children.push_back(std::move(boundary));

    feed.root = std::move(root);
    module.components.push_back(std::move(feed));
    return module;
}

} // namespace ir_test
