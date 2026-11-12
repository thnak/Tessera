// SPDX-License-Identifier: MIT
//
// Sample IR fixture for the codegen tests: a valid module using every construct the static codegen
// emits.

#pragma once

#include "tessera/compiler/codegen/codegen.hpp"
#include "tessera/compiler/ir/validate.hpp"

#include <cstdint>
#include <string>
#include <utility>

namespace tessera::test::codegen_sample {

namespace ir = tessera::compiler::ir;

inline ir::SourceSpan span(std::uint32_t offset, std::uint32_t length, std::uint32_t line,
                           std::uint32_t column) {
    return ir::SourceSpan{"ui/Counter.tsx", offset, length, line, column, line, column + length};
}

inline ir::ExprPtr integer(std::int64_t value) {
    return ir::make_literal(value);
}

inline ir::ExprPtr text(std::string value) {
    return ir::make_literal(std::move(value));
}

inline ir::Node leaf(ir::NodeKind kind, std::string id, std::string ui_id) {
    ir::Node node;
    node.kind = kind;
    node.id = std::move(id);
    node.ui_id = std::move(ui_id);
    node.source = span(10, 5, 1, 1);
    return node;
}

// The fixture is a valid IR module that uses every construct this codegen slice emits: state, refs,
// animated values, props, a static intrinsic tree, text, class/id/role/aria/style metadata, an
// inline handler, a native event binding, and an effect with its cleanup (region stubs only).
inline ir::IrModule make_sample_module() {
    ir::IrModule module;
    module.name = "demo";

    ir::Component sample;
    sample.id = "comp::Sample";
    sample.component_id = "1";
    sample.name = "Sample";
    sample.source = span(0, 100, 1, 1);
    sample.props.push_back({"title", "string", nullptr});
    sample.state.push_back({"state::count", "count", "int", integer(0)});
    sample.state.push_back({"state::name", "name", "string", ir::make_literal(std::string(""))});
    sample.refs.push_back({"ref::box", "box", "node::2"});
    sample.animated_values.push_back({"anim::opacity", "opacity", "float", ir::make_literal(1.0)});

    ir::Handler handler;
    handler.id = "handler::1";
    handler.source = span(20, 10, 2, 3);
    handler.params.push_back({"event", "ClickEvent"});
    ir::Action set;
    set.kind = ir::ActionKind::Set;
    set.target = "state::count";
    set.value = integer(1);
    handler.actions.push_back(std::move(set));
    ir::Action fade;
    fade.kind = ir::ActionKind::AnimSet;
    fade.target = "anim::opacity";
    fade.value = ir::make_literal(0.5);
    fade.duration_ms = 300.0;
    fade.easing = "ease-in-out";
    handler.actions.push_back(std::move(fade));
    sample.handlers.push_back(std::move(handler));

    ir::Handler change;
    change.id = "handler::2";
    change.source = span(25, 10, 3, 3);
    change.params.push_back({"e", "TextChangeEvent"});
    ir::Action set_name;
    set_name.kind = ir::ActionKind::Set;
    set_name.target = "state::name";
    set_name.value = ir::make_ref("e.value");
    change.actions.push_back(std::move(set_name));
    ir::Action reset;
    reset.kind = ir::ActionKind::AnimSet;
    reset.target = "anim::opacity";
    reset.value = ir::make_literal(1.0);
    reset.animate = false;
    change.actions.push_back(std::move(reset));
    sample.handlers.push_back(std::move(change));

    ir::Handler spring;
    spring.id = "handler::3";
    spring.source = span(28, 10, 3, 3);
    spring.params.push_back({"event", "ClickEvent"});
    ir::Action bounce;
    bounce.kind = ir::ActionKind::AnimSet;
    bounce.target = "anim::opacity";
    bounce.value = ir::make_literal(0.25);
    bounce.spring = ir::SpringTiming{300.0, 12.0, 1.0};
    bounce.on_complete = "handler::4";
    spring.actions.push_back(std::move(bounce));
    sample.handlers.push_back(std::move(spring));

    ir::Handler done;
    done.id = "handler::4";
    done.source = span(31, 10, 3, 3);
    ir::Action finish;
    finish.kind = ir::ActionKind::Set;
    finish.target = "state::count";
    finish.value = integer(7);
    done.actions.push_back(std::move(finish));
    sample.handlers.push_back(std::move(done));

    sample.effects.push_back(
        {"effect::sync", {"state::count"}, "bind::sync", "bind::sync_cleanup", span(30, 8, 3, 3)});

    ir::Node root = leaf(ir::NodeKind::View, "node::1", "1");
    {
        std::vector<ir::TemplatePart> parts;
        parts.push_back(ir::text_part("root-"));
        parts.push_back(ir::expr_part(ir::make_ref("state::count")));
        root.props.emplace("id", ir::make_template(std::move(parts)));
    }
    {
        std::vector<ir::TemplatePart> parts;
        parts.push_back(ir::text_part("box "));
        parts.push_back(ir::expr_part(ir::make_ref("state::count")));
        root.props.emplace("className", ir::make_template(std::move(parts)));
    }
    root.props.emplace("role", ir::make_conditional(
                                   ir::make_binary("==", ir::make_ref("state::count"), integer(0)),
                                   text("generic"), text("status")));
    {
        std::vector<ir::TemplatePart> parts;
        parts.push_back(ir::text_part("label "));
        parts.push_back(ir::expr_part(ir::make_ref("state::count")));
        root.props.emplace("aria-label", ir::make_template(std::move(parts)));
    }
    root.automation_id = "1";
    root.style.emplace("width", text("10px"));
    root.style.emplace("opacity", ir::make_ref("anim::opacity"));

    ir::Node button = leaf(ir::NodeKind::Button, "node::2", "2");
    button.props.emplace("label", text("Go"));
    button.events.emplace("onClick", "bind::on_click");
    ir::Node label = leaf(ir::NodeKind::Text, "node::3", "3");
    std::vector<ir::TemplatePart> parts;
    parts.push_back(ir::text_part("Count: "));
    parts.push_back(ir::expr_part(ir::make_ref("state::count")));
    label.text = ir::make_template(std::move(parts));
    button.children.push_back(std::move(label));
    root.children.push_back(std::move(button));

    // A second Button exercises a declared handler record (typed payload adapter).
    ir::Node handler_button = leaf(ir::NodeKind::Button, "node::4", "4");
    handler_button.events.emplace("onClick", "handler::1");
    root.children.push_back(std::move(handler_button));

    // Controlled text field: value from state, onChange is an inline setter copying the payload.
    ir::Node input = leaf(ir::NodeKind::TextInput, "node::5", "5");
    input.props.emplace("value", ir::make_ref("state::name"));
    input.events.emplace("onChange", "handler::2");
    root.children.push_back(std::move(input));
    sample.root = std::move(root);
    module.components.push_back(std::move(sample));

    for (const auto& [id, kind, region] :
         {std::tuple<std::string, ir::BindingKind, std::string>{"bind::on_click",
                                                                ir::BindingKind::Event, "on_click"},
          std::tuple<std::string, ir::BindingKind, std::string>{"bind::sync",
                                                                ir::BindingKind::Effect, "sync"},
          std::tuple<std::string, ir::BindingKind, std::string>{
              "bind::sync_cleanup", ir::BindingKind::Effect, "sync_cleanup"}}) {
        ir::Binding binding;
        binding.id = id;
        binding.component = "comp::Sample";
        binding.kind = kind;
        binding.source = span(40, 10, 4, 3);
        binding.region = region;
        if (kind == ir::BindingKind::Event) {
            binding.params.push_back({"event", "ClickEvent"});
        }
        module.bindings.push_back(std::move(binding));
    }
    return module;
}

} // namespace tessera::test::codegen_sample
