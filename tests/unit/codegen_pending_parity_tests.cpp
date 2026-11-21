// SPDX-License-Identifier: MIT

// ADR-067 spike #661, the differential test: the IR evaluator and generated C++ run the *same*
// script of pending-UI operations and must agree after every step.
//
// This is the evidence the ADR asks for ("the evaluator and the generated code agree"). The two
// sides are not two implementations of the rule: both run `tessera::ui::pending`, which is
// header-only for exactly this reason. What the test proves is the *wiring* - that the evaluator's
// `PendingSession` and the generated component's members are driven by the same operations in the
// same order, and that a transition-marked action starts the mark on both sides.
//
// The script is a fixed sequence (no clock, no randomness), and every step compares the whole
// observable state: the boundary phase, the live subtree, `isPending`, the epoch and the value.

#include "Feed.hpp"
#include "codegen_pending_module.hpp"
#include "tessera/compiler/eval/evaluator.hpp"
#include "tessera/compiler/eval/pending.hpp"
#include "tessera/compiler/ir/validate.hpp"
#include "tessera/ui/scene/node.hpp"

#include <cstdint>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace eval = tessera::compiler::eval;
namespace ir = tessera::compiler::ir;
namespace ui = tessera::ui;

int failures = 0;

void check(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << "\n";
        ++failures;
    }
}

// One observable state of the pending UI, from either side.
struct Observation {
    bool fallback_live = false;
    bool is_pending = false;
    std::uint64_t epoch = 0;
    int value = 0;
    bool loading = false;
    bool has_data = false;
};

std::string describe(const Observation& observation) {
    return "fallback=" + std::string(observation.fallback_live ? "1" : "0") +
           " pending=" + std::string(observation.is_pending ? "1" : "0") +
           " epoch=" + std::to_string(observation.epoch) +
           " value=" + std::to_string(observation.value) +
           " loading=" + std::string(observation.loading ? "1" : "0") +
           " data=" + std::string(observation.has_data ? "1" : "0");
}

// The evaluator's side: the pending session the evaluator's `run()` drives.
Observation observe(const eval::PendingSession& session) {
    Observation observation;
    observation.fallback_live = session.boundary().fallback_live();
    observation.is_pending = session.is_pending();
    observation.epoch = session.resource().epoch();
    observation.value = session.resource().value();
    observation.loading = session.resource().loading();
    observation.has_data = session.resource().has_data();
    return observation;
}

// The generated side: the component's own accessors.
Observation observe(const pending::Feed& feed) {
    Observation observation;
    observation.fallback_live = feed.pending_fallback_live();
    observation.is_pending = feed.pending_is_pending();
    observation.epoch = feed.pending_epoch();
    observation.value = feed.pending_value();
    observation.loading = feed.pending_loading();
    observation.has_data = feed.pending_has_data();
    return observation;
}

// One step of the script, applied to both sides.
enum class StepKind : std::uint8_t { BeginFetch, Resolve, Fail, Sync, MarkedSet, Settle };

struct Step {
    StepKind kind = StepKind::Sync;
    std::uint64_t epoch = 0;
    int value = 0;
    std::string label;
};

// The script: fallback, restore, keep-on-refresh, stale-epoch drop, a marked setter, and the
// settle. Every step is deterministic and names its own epoch, so a failure names the step.
std::vector<Step> make_script() {
    return {
        {.kind = StepKind::Sync, .label = "idle: content"},
        {.kind = StepKind::BeginFetch, .label = "fetch with no data: fallback"},
        {.kind = StepKind::Sync, .label = "sync the fallback"},
        {.kind = StepKind::Resolve,
         .epoch = 1,
         .value = 7,
         .label = "the completion lands: content restored"},
        {.kind = StepKind::Sync, .label = "sync the restore"},
        {.kind = StepKind::BeginFetch, .label = "a key change: refreshing"},
        {.kind = StepKind::Sync, .label = "sync the refresh: the old subtree stays"},
        {.kind = StepKind::Resolve, .epoch = 2, .value = 8, .label = "the refresh lands"},
        {.kind = StepKind::Sync, .label = "sync the refreshed value"},
        {.kind = StepKind::BeginFetch, .label = "fetch A"},
        {.kind = StepKind::BeginFetch, .label = "fetch B supersedes A"},
        {.kind = StepKind::Resolve,
         .epoch = 3,
         .value = 99,
         .label = "a completion for the superseded epoch is dropped"},
        {.kind = StepKind::Sync, .label = "sync after the drop"},
        {.kind = StepKind::Resolve, .epoch = 4, .value = 5, .label = "the current epoch lands"},
        {.kind = StepKind::Sync, .label = "sync the accepted completion"},
        {.kind = StepKind::MarkedSet, .label = "a transition-marked setter"},
        {.kind = StepKind::Settle, .label = "the transition's fetch lands"},
        {.kind = StepKind::Sync, .label = "sync the settled transition"},
        {.kind = StepKind::Fail, .epoch = 5, .label = "a failed refresh keeps the data"},
        {.kind = StepKind::Sync, .label = "sync the failed refresh"},
    };
}

// Applies one step to the evaluator's session.
void apply(const Step& step, eval::PendingSession& session) {
    switch (step.kind) {
    case StepKind::BeginFetch:
        (void)session.begin_fetch();
        break;
    case StepKind::Resolve:
        (void)session.resolve(step.epoch, step.value);
        break;
    case StepKind::Fail:
        (void)session.fail(step.epoch);
        break;
    case StepKind::Sync:
        session.sync_read(0);
        break;
    case StepKind::MarkedSet:
        // The evaluator's half of a transition-marked action: `Evaluator::run` calls exactly this
        // when it meets an action with `transition == true`.
        session.begin_transition();
        break;
    case StepKind::Settle:
        (void)session.observe_transition();
        break;
    }
}

// Applies one step to the generated component.
void apply(const Step& step, pending::Feed& feed) {
    switch (step.kind) {
    case StepKind::BeginFetch:
        (void)feed.pending_begin_fetch();
        break;
    case StepKind::Resolve:
        (void)feed.pending_resolve(step.epoch, step.value);
        break;
    case StepKind::Fail:
        (void)feed.pending_fail(step.epoch);
        break;
    case StepKind::Sync:
        feed.pending_sync();
        break;
    case StepKind::MarkedSet:
        // The generated half: the Button's click binding runs the marked setter.
        feed.run_marked_set();
        break;
    case StepKind::Settle:
        feed.pending_observe_transition();
        break;
    }
}

// The evaluator's side of the marked setter: the IR action list of `handler::search`, run through
// the evaluator with a pending session attached. This is the path a preview body takes.
std::expected<void, eval::EvalError>
run_marked_action(const ir::IrModule& module, eval::Evaluator& evaluator, eval::Scope& scope) {
    for (const ir::Component& component : module.components) {
        for (const ir::Handler& handler : component.handlers) {
            if (handler.id != "handler::search") {
                continue;
            }
            return evaluator.run(handler.actions, scope);
        }
    }
    return std::unexpected(
        eval::EvalError{"TESSERA_IR_EVAL_MALFORMED", "the fixture has no `handler::search` body"});
}

} // namespace

int main() {
    const ir::IrModule module = tessera::test::codegen_pending::make_pending_module();
    std::string codes;
    for (const ir::IrDiagnostic& diagnostic : ir::validate(module)) {
        codes += diagnostic.code + ":" + diagnostic.message + "; ";
    }
    check(codes.empty(), "the differential fixture is a valid IR module");

    // The evaluator's side.
    eval::PendingSession session;
    session.reset(1);
    eval::Evaluator evaluator;
    evaluator.set_pending_session(&session);
    eval::Scope scope;
    scope.emplace("state::query", eval::Value::of_string(""));

    // The generated side.
    pending::Feed feed;
    ui::Node root(1000);
    feed.build(root);

    const std::vector<Step> script = make_script();
    int step_index = 0;
    for (const Step& step : script) {
        if (step.kind == StepKind::MarkedSet) {
            // The evaluator runs the IR action list; the generated side runs the developer binding.
            // Both must start the mark, so the comparison below covers it.
            const auto ran = run_marked_action(module, evaluator, scope);
            check(ran.has_value(), "the evaluator runs the marked action list");
            apply(step, feed);
        } else {
            apply(step, session);
            apply(step, feed);
        }
        const Observation preview = observe(session);
        const Observation native = observe(feed);
        if (describe(preview) != describe(native)) {
            std::cerr << "FAIL: step " << step_index << " (" << step.label
                      << "): the evaluator and the generated code disagree\n"
                      << "  evaluator: " << describe(preview) << "\n"
                      << "  generated: " << describe(native) << "\n";
            ++failures;
        }
        ++step_index;
    }

    // The script must actually have exercised both phases, or an always-equal pair of no-ops would
    // pass. These are the ADR's named behaviours, asserted on the final state of both sides.
    check(step_index == static_cast<int>(script.size()), "every script step ran");
    check(!observe(session).fallback_live && !observe(feed).fallback_live,
          "the script ends with the content live on both sides");
    check(observe(session).has_data && observe(feed).has_data,
          "the script ends with data on both sides");
    check(scope.at("state::query").as_string() != nullptr &&
              *scope.at("state::query").as_string() == "next",
          "the evaluator's marked setter assigned its value");
    check(feed.query() == "next", "the generated marked setter assigned its value");

    if (failures != 0) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "pending parity: ok (" << step_index << " steps)\n";
    return 0;
}
