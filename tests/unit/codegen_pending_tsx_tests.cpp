// SPDX-License-Identifier: MIT

// #656 end to end (docs/specs/pending-ui-v1.md): one TSX component is compiled through the real
// front end into the generated base `Feed`, and lowered again for the IR evaluator. Both sides read
// the same resource double through a `ResourceView` and must agree after every step on the boundary
// phase, `isPending`, the resource's status and typed `.data`, and the `useDeferredValue` value. A
// second scenario starts the transition on a resource with no data yet: the boundary keeps its old
// subtree (spec section 4.3) instead of entering its fallback.

#include "Feed.hpp"
#include "codegen_pending_tsx_fixture.hpp"
#include "tessera/compiler/eval/evaluator.hpp"
#include "tessera/compiler/eval/pending.hpp"
#include "tessera/compiler/ir/validate.hpp"
#include "tessera/ui/pending/fake_resource.hpp"
#include "tessera/ui/pending/resource_view.hpp"
#include "tessera/ui/scene/node.hpp"
#include "tessera/ui/scene/paint_data.hpp"

#include <iostream>
#include <string>
#include <string_view>

namespace {

namespace eval = tessera::compiler::eval;
namespace ir = tessera::compiler::ir;
namespace ui = tessera::ui;
namespace pend = tessera::ui::pending;
namespace fixture = tessera::test::pending_tsx;

int failures = 0;

void check(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << "\n";
        ++failures;
    }
}

struct Observation {
    bool fallback = false;
    bool is_pending = false;
    std::string status;
    std::string data;
    std::string settled;
};

std::string describe(const Observation& o) {
    return "fallback=" + std::to_string(o.fallback) + " pending=" + std::to_string(o.is_pending) +
           " status=" + o.status + " data=" + o.data + " settled=" + o.settled;
}

std::string text_of_value(const eval::Evaluator& evaluator, eval::Evaluator& mutable_evaluator,
                          const eval::Scope& scope, std::string_view id) {
    (void)evaluator;
    const ir::ExprPtr ref = ir::make_ref(std::string(id));
    const auto value = mutable_evaluator.evaluate(*ref, scope);
    if (!value || value->as_string() == nullptr) {
        return "<unreadable>";
    }
    return *value->as_string();
}

Observation observe_evaluator(const eval::PendingSession& session, eval::Evaluator& evaluator,
                              const eval::Scope& scope) {
    Observation o;
    o.fallback = session.boundary_at(0).fallback_live();
    o.is_pending = session.is_pending();
    o.status = std::string(*session.status_of("res::feed"));
    o.data = text_of_value(evaluator, evaluator, scope, "res::feed.data");
    o.settled = text_of_value(evaluator, evaluator, scope, "beh::settled.value");
    return o;
}

Observation observe_generated(const pending_tsx::Feed& feed) {
    Observation o;
    o.fallback = feed.pending_fallback_live();
    o.is_pending = feed.pending_is_pending();
    o.status = feed.res_feed_status();
    o.data = std::string(feed.res_feed_state().text);
    o.settled = feed.beh_settled_value();
    return o;
}

const ui::Node* find_text(const ui::Node& node, std::string_view text) {
    const std::optional<ui::PaintData>& paint = node.paint_data();
    if (paint && paint->text == text) {
        return &node;
    }
    for (const auto& child : node.children()) {
        if (const ui::Node* found = find_text(*child, text)) {
            return found;
        }
    }
    return nullptr;
}

const ir::Handler* marked_handler(const ir::Component& component) {
    for (const ir::Handler& handler : component.handlers) {
        for (const ir::Action& action : handler.actions) {
            if (action.transition) {
                return &handler;
            }
        }
    }
    return nullptr;
}

// Both halves of one scenario: the evaluator over the lowered IR and the generated component, with
// one resource double behind both views.
struct Pair {
    explicit Pair(const ir::Component& component_in)
        : component(component_in),
          root(1000) {
        session.configure(component);
        session.bind_resource(0, pend::ResourceView::of(double_resource));
        evaluator.set_pending_session(&session);
        scope.emplace("state::mode", eval::Value::of_string("fast"));
        generated.build(root);
        generated.bind_res_feed(pend::ResourceView::of(double_resource));
    }

    void sync() {
        session.sync();
        generated.pending_sync();
        generated.update(); // the host refreshes the bindings that read external values
    }

    void compare(std::string_view label) {
        const Observation a = observe_evaluator(session, evaluator, scope);
        const Observation b = observe_generated(generated);
        if (describe(a) != describe(b)) {
            std::cerr << "FAIL: " << label << ": the evaluator and the generated code disagree\n"
                      << "  evaluator: " << describe(a) << "\n  generated: " << describe(b) << "\n";
            ++failures;
        }
    }

    void mark() {
        const ir::Handler* handler = marked_handler(component);
        check(handler != nullptr, "the TSX lowered a transition-marked handler");
        if (handler != nullptr) {
            check(evaluator.run(handler->actions, scope).has_value(), "the evaluator runs it");
        }
        generated.run_marked_set();
    }

    const ir::Component& component;
    pend::FakeResource double_resource;
    eval::PendingSession session;
    eval::Evaluator evaluator;
    eval::Scope scope;
    pending_tsx::Feed generated;
    ui::Node root;
};

void test_scenario_with_data(const ir::Component& component) {
    Pair pair(component);
    pair.sync();
    pair.compare("idle");
    check(!pair.generated.pending_fallback_live() && find_text(pair.root, "fast") != nullptr,
          "idle: the content shows and the deferred value is the argument");

    (void)pair.double_resource.begin_fetch();
    pair.sync();
    pair.compare("fetch with no data");
    check(pair.generated.pending_fallback_live() && find_text(pair.root, "Loading") != nullptr,
          "a suspending typed read with no data shows the fallback");

    check(pair.double_resource.resolve_text(1, "alpha"), "the first fetch lands");
    pair.sync();
    pair.compare("first data");
    check(!pair.generated.pending_fallback_live() && find_text(pair.root, "alpha") != nullptr,
          "the typed .data renders in the restored subtree");
    check(find_text(pair.root, "success") != nullptr, ".status renders");

    pair.mark();
    pair.sync();
    pair.compare("marked setter");
    check(pair.generated.pending_is_pending() && find_text(pair.root, "Updating") != nullptr,
          "isPending is true and its conditional renders from the marked change");
    check(find_text(pair.root, "fast") != nullptr,
          "the deferred value still shows the settled value while the transition is pending");

    (void)pair.double_resource.begin_fetch(); // the host's fetch for the new key
    pair.sync();
    pair.compare("refresh");
    check(!pair.generated.pending_fallback_live() && find_text(pair.root, "alpha") != nullptr &&
              pair.generated.pending_is_pending(),
          "a refresh keeps the subtree and isPending holds");

    check(pair.double_resource.resolve_text(2, "beta"), "the key's fetch lands");
    pair.sync();
    pair.compare("settled");
    check(!pair.generated.pending_is_pending() && find_text(pair.root, "Updating") == nullptr &&
              find_text(pair.root, "slow") != nullptr && find_text(pair.root, "beta") != nullptr,
          "isPending clears and the deferred value takes the new argument");
}

void test_hold_keeps_the_old_subtree(const ir::Component& component) {
    Pair pair(component);
    pair.sync();
    pair.compare("idle");
    check(!pair.generated.pending_fallback_live(), "idle: the content is live");

    pair.mark();
    pair.sync();
    pair.compare("marked on an idle resource");
    (void)pair.double_resource.begin_fetch(); // Loading with no data
    pair.sync();
    pair.compare("loading under a transition");
    check(pair.generated.pending_is_pending() && !pair.generated.pending_fallback_live() &&
              find_text(pair.root, "Loading") == nullptr,
          "a transition holds the old subtree: no fallback while it is pending");
    check(pair.session.boundary_at(0).held(), "the evaluator's boundary holds as well");

    check(pair.double_resource.resolve_text(1, "gamma"), "the fetch lands");
    pair.sync();
    pair.compare("settled");
    check(!pair.generated.pending_is_pending() && find_text(pair.root, "gamma") != nullptr,
          "the transition settles on the new content");

    // Without a transition the same Loading-with-no-data frame is a fallback (the spec's rule 1).
    Pair plain(component);
    plain.sync();
    (void)plain.double_resource.begin_fetch();
    plain.sync();
    plain.compare("loading without a transition");
    check(plain.generated.pending_fallback_live(), "no transition: the fallback shows");
}

} // namespace

int main() {
    std::string error;
    const auto module = fixture::compile(fixture::kSource, error);
    check(module.has_value(), "the TSX compiles: " + error);
    if (!module) {
        return 1;
    }
    std::string codes;
    for (const ir::IrDiagnostic& diagnostic : ir::validate(*module)) {
        codes += diagnostic.code + ":" + diagnostic.message + "; ";
    }
    check(codes.empty(), "the lowered module validates: " + codes);
    const ir::Component& component = module->components.at(0);
    check(component.resources.size() == 1 && component.resources[0].type == "string" &&
              component.resources[0].suspend,
          "the resource is typed from the service method and suspends");
    test_scenario_with_data(component);
    test_hold_keeps_the_old_subtree(component);
    if (failures != 0) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "pending tsx end to end: ok\n";
    return 0;
}
