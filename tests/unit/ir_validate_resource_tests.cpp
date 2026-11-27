// SPDX-License-Identifier: MIT

// IR validation of `resources` (ADR-067 item 1, docs/specs/pending-ui-v1.md sections 3.4, 4.2, 9).

#include "ir_validate_util.hpp"
#include "tessera/compiler/ir/suspense_reads.hpp"

#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace ir_test;

constexpr const char* kNoBoundary = "TESSERA_SUSPENSE_NO_BOUNDARY;";
constexpr const char* kTooMany = "TESSERA_SUSPENSE_TOO_MANY_READS;";

void expect_resource(const std::string& what, const std::function<void(ir::IrModule&)>& change,
                     const std::string& expected) {
    ir::IrModule module = make_resource_module();
    change(module);
    check_eq(codes_of(module), expected, what);
}

ir::Component& feed(ir::IrModule& module) {
    return module.components[0];
}
ir::Node& root(ir::IrModule& module) {
    return feed(module).root;
}
ir::Node& boundary(ir::IrModule& module) {
    return root(module).children[1];
}
ir::Node& status_text(ir::IrModule& module) {
    return root(module).children[0];
}

ir::ExprPtr data_text(const std::string& ref) {
    std::vector<ir::TemplatePart> parts;
    parts.push_back(ir::expr_part(ir::make_ref(ref)));
    return ir::make_template(std::move(parts));
}

ir::Resource extra_resource(const std::string& name, bool suspend) {
    ir::Resource resource;
    resource.id = "res::" + name;
    resource.service = "svc::api";
    resource.method = "load";
    resource.suspend = suspend;
    resource.key = std::move(*ir::make_ref("state::query"));
    resource.source = ir::SourceSpan{"ui/Feed.tsx", 100, 10, 20, 3, 20, 13};
    return resource;
}

void test_valid_and_references() {
    check_eq(codes_of(make_resource_module()), "", "the resource fixture is a valid IR module");
    expect_resource(
        "a passive resource reads data outside any boundary",
        [](auto& m) { status_text(m).text = data_text("res::tags.data"); }, "");
    expect_resource(
        "status and error of a suspending resource are readable outside the boundary",
        [](auto& m) {
            status_text(m).text = data_text("res::feed.status");
            root(m).children[0].props.emplace("error", ir::make_ref("res::feed.error"));
        },
        "");
    expect_resource(
        "a bare resource id is a record, not a value",
        [](auto& m) { status_text(m).text = data_text("res::tags"); }, kUnresolved);
    expect_resource(
        "only status, data and error are members",
        [](auto& m) { status_text(m).text = data_text("res::tags.value"); }, kUnresolved);
    expect_resource(
        "an undeclared resource does not resolve",
        [](auto& m) { status_text(m).text = data_text("res::nope.status"); }, kUnresolved);
    expect_resource(
        "a resource id is unique among the component's ids",
        [](auto& m) { feed(m).resources.push_back(extra_resource("tags", false)); }, kDuplicate);
}

void test_entry_rules() {
    expect_resource(
        "the service must be declared by the component",
        [](auto& m) { feed(m).resources[0].service = "svc::other"; }, kUnresolved);
    expect_resource(
        "a resource names a service method", [](auto& m) { feed(m).resources[1].method.clear(); },
        kMalformed);
    expect_resource(
        "a scalar key reading a prop is accepted",
        [](auto& m) {
            feed(m).props.push_back({"page", "int", nullptr, false});
            feed(m).resources[1].key = std::move(*ir::make_ref("prop::page"));
        },
        "");
    expect_resource(
        "a key may not be a list",
        [](auto& m) {
            feed(m).state.push_back({"state::ids", "ids", "list<int>", ir::make_array({})});
            feed(m).resources[1].key = std::move(*ir::make_ref("state::ids"));
        },
        kUnsupported);
    expect_resource(
        "a key may not read another resource",
        [](auto& m) { feed(m).resources[1].key = std::move(*ir::make_ref("res::feed.status")); },
        kUnsupported);
    expect_resource(
        "a record key holds scalars only",
        [](auto& m) {
            std::vector<ir::RecordField> inner;
            inner.push_back(ir::RecordField{"a", ir::make_ref("state::query")});
            std::vector<ir::RecordField> outer;
            outer.push_back(ir::RecordField{"nested", ir::make_record("", std::move(inner))});
            feed(m).resources[0].key = std::move(*ir::make_record("", std::move(outer)));
        },
        kUnsupported);
    expect_resource(
        "a record key does not name a declared record",
        [](auto& m) { feed(m).resources[0].key.type = "Size"; }, kUnsupported);
    expect_resource(
        "a record key field resolves",
        [](auto& m) { feed(m).resources[0].key.fields[0].value = ir::make_ref("state::nope"); },
        kUnresolved);
    expect_resource(
        "a record key has at least one field",
        [](auto& m) { feed(m).resources[0].key.fields.clear(); }, kMalformed);
}

void test_boundary_coverage() {
    expect_resource(
        "a suspending read outside every boundary has no boundary",
        [](auto& m) { status_text(m).text = data_text("res::feed.data"); }, kNoBoundary);
    expect_resource(
        "a suspending read in the fallback is outside the boundary",
        [](auto& m) { boundary(m).fallback->text = data_text("res::feed.data"); }, kNoBoundary);
    expect_resource(
        "a status read inside the boundary ties the resource to it",
        [](auto& m) { boundary(m).children[0].children[0].text = data_text("res::feed.status"); },
        "");
    expect_resource(
        "a suspending resource that no boundary reads has no boundary",
        [](auto& m) {
            boundary(m).children[0].children[0].text = ir::make_literal(std::string("static"));
        },
        kNoBoundary);
    expect_resource(
        "a suspending resource without any Suspense has no boundary",
        [](auto& m) { root(m).children.pop_back(); }, kNoBoundary);
    expect_resource(
        "a passive resource needs no boundary",
        [](auto& m) {
            boundary(m).children[0].children[0].text = data_text("res::tags.data");
            feed(m).resources[0].suspend = false;
        },
        "");

    // The nearest boundary covers the read: an inner boundary takes it from the outer one.
    {
        ir::IrModule module = make_resource_module();
        ir::Node inner;
        inner.kind = ir::NodeKind::Suspense;
        inner.id = "node::7";
        inner.ui_id = "707";
        inner.source = boundary(module).source;
        inner.children.push_back(std::move(boundary(module).children[0]));
        ir::Node inner_fallback;
        inner_fallback.kind = ir::NodeKind::Text;
        inner_fallback.id = "node::8";
        inner_fallback.ui_id = "708";
        inner_fallback.source = boundary(module).source;
        inner_fallback.text = ir::make_literal(std::string("Inner"));
        inner.fallback = std::make_unique<ir::Node>(std::move(inner_fallback));
        boundary(module).children.clear();
        boundary(module).children.push_back(std::move(inner));
        check_eq(codes_of(module), "", "nested boundaries validate");
        const ir::SuspenseReads reads = ir::collect_suspense_reads(feed(module));
        check(reads.boundaries.size() == 2, "two boundaries are collected");
        check(reads.boundaries[0].resources.empty(), "the outer boundary does not cover the read");
        check(reads.boundaries[1].resources == std::vector<std::size_t>{0},
              "the nearest (inner) boundary covers the read");
        check(reads.uncovered.empty(), "nothing is uncovered");
    }
}

void test_read_limit() {
    const auto with_reads = [](ir::IrModule& m, int count) {
        std::vector<ir::TemplatePart> parts;
        for (int i = 0; i < count; ++i) {
            const std::string name = "r" + std::to_string(i);
            ir::Resource resource = extra_resource(name, true);
            resource.source = feed(m).source;
            feed(m).resources.push_back(std::move(resource));
            parts.push_back(ir::expr_part(ir::make_ref("res::" + name + ".data")));
        }
        boundary(m).children[0].children[0].text = ir::make_template(std::move(parts));
    };
    // The fixture's own `res::feed` is no longer read, so it is uncovered; the count is what is
    // under test, so the suspending flag of that one is dropped.
    expect_resource(
        "eight suspending reads in one boundary are accepted",
        [&](auto& m) {
            feed(m).resources[0].suspend = false;
            with_reads(m, 8);
        },
        "");
    expect_resource(
        "nine suspending reads in one boundary are rejected once",
        [&](auto& m) {
            feed(m).resources[0].suspend = false;
            with_reads(m, 9);
        },
        kTooMany);
}

} // namespace

int main() {
    test_valid_and_references();
    test_entry_rules();
    test_boundary_coverage();
    test_read_limit();
    return finish("ir validate resource tests");
}
