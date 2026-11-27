// SPDX-License-Identifier: MIT

// #656, the differential test for the resource-driven component: the IR evaluator's PendingSession
// and the generated `ResourceFeed` read the same resource doubles through ResourceViews and must
// agree after every step of one fixed script (fallback, restore, a second slot, a refresh that
// keeps the subtree, a superseded epoch, a transition that waits for the key's fetch, a failed
// refresh).
//
// As in the fake-resource parity test, both sides run `tessera::ui::pending`; what is compared is
// the wiring: which resource feeds which boundary slot, which resource the mark follows, and the
// epoch a marked setter waits for.

#include "ResourceFeed.hpp"
#include "codegen_pending_module.hpp"
#include "tessera/compiler/eval/evaluator.hpp"
#include "tessera/compiler/eval/pending.hpp"
#include "tessera/compiler/ir/validate.hpp"
#include "tessera/ui/pending/fake_resource.hpp"
#include "tessera/ui/pending/resource_view.hpp"
#include "tessera/ui/scene/node.hpp"
#include "tessera/ui/scene/paint_data.hpp"

#include <cstdint>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace eval = tessera::compiler::eval;
namespace ir = tessera::compiler::ir;
namespace ui = tessera::ui;
namespace pend = tessera::ui::pending;

int failures = 0;

void check(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << "\n";
        ++failures;
    }
}

struct Observation {
    bool fallback_live = false;
    bool is_pending = false;
    std::string feed_status;
    std::string tags_status;
    std::uint64_t feed_epoch = 0;
    std::uint64_t tags_epoch = 0;
};

std::string describe(const Observation& o) {
    return "fallback=" + std::to_string(o.fallback_live) +
           " pending=" + std::to_string(o.is_pending) + " feed=" + o.feed_status + "@" +
           std::to_string(o.feed_epoch) + " tags=" + o.tags_status + "@" +
           std::to_string(o.tags_epoch);
}

Observation observe(const eval::PendingSession& session) {
    Observation o;
    o.fallback_live = session.boundary_at(0).fallback_live();
    o.is_pending = session.is_pending();
    o.feed_status = std::string(*session.status_of("res::feed"));
    o.tags_status = std::string(*session.status_of("res::tags"));
    o.feed_epoch = session.resource_state(0).epoch;
    o.tags_epoch = session.resource_state(1).epoch;
    return o;
}

Observation observe(const pending::ResourceFeed& feed) {
    Observation o;
    o.fallback_live = feed.pending_fallback_live();
    o.is_pending = feed.pending_is_pending();
    o.feed_status = feed.res_feed_status();
    o.tags_status = feed.res_tags_status();
    o.feed_epoch = feed.res_feed_state().epoch;
    o.tags_epoch = feed.res_tags_state().epoch;
    return o;
}

enum class Kind : std::uint8_t {
    FeedFetch,
    FeedResolve,
    FeedFail,
    TagsFetch,
    TagsResolve,
    Sync,
    Marked
};

struct Step {
    Kind kind = Kind::Sync;
    std::uint64_t epoch = 0;
    std::string label;
};

std::vector<Step> make_script() {
    return {
        {.kind = Kind::Sync, .label = "idle: content"},
        {.kind = Kind::FeedFetch, .label = "feed fetch with no data: fallback"},
        {.kind = Kind::Sync, .label = "sync the fallback"},
        {.kind = Kind::FeedResolve, .epoch = 1, .label = "the feed lands: content restored"},
        {.kind = Kind::Sync, .label = "sync the restore"},
        {.kind = Kind::TagsFetch, .label = "tags fetch with no data: the second slot suspends"},
        {.kind = Kind::Sync, .label = "sync the second slot"},
        {.kind = Kind::TagsResolve, .epoch = 1, .label = "the tags land"},
        {.kind = Kind::Sync, .label = "sync both with data"},
        {.kind = Kind::FeedFetch, .label = "a key change: refreshing keeps the subtree"},
        {.kind = Kind::Sync, .label = "sync the refresh"},
        {.kind = Kind::FeedResolve, .epoch = 2, .label = "the refresh lands"},
        {.kind = Kind::Sync, .label = "sync the refreshed value"},
        {.kind = Kind::FeedFetch, .label = "fetch A"},
        {.kind = Kind::FeedFetch, .label = "fetch B supersedes A"},
        {.kind = Kind::FeedResolve, .epoch = 3, .label = "the superseded epoch is dropped"},
        {.kind = Kind::FeedResolve, .epoch = 4, .label = "the current epoch lands"},
        {.kind = Kind::Sync, .label = "sync after the drop"},
        {.kind = Kind::Marked, .label = "a transition-marked setter"},
        {.kind = Kind::Sync, .label = "sync: no fetch yet, the mark waits"},
        {.kind = Kind::FeedFetch, .label = "the host's fetch for the new key"},
        {.kind = Kind::Sync, .label = "sync: isPending while the fetch runs"},
        {.kind = Kind::FeedResolve, .epoch = 5, .label = "the key's fetch lands"},
        {.kind = Kind::Sync, .label = "sync: isPending clears"},
        {.kind = Kind::FeedFetch, .label = "another refresh"},
        {.kind = Kind::FeedFail, .epoch = 6, .label = "a failed refresh keeps the data"},
        {.kind = Kind::Sync, .label = "sync the failure"},
    };
}

void apply_to_doubles(const Step& step, pend::FakeResource& feed, pend::FakeResource& tags) {
    switch (step.kind) {
    case Kind::FeedFetch:
        (void)feed.begin_fetch();
        break;
    case Kind::FeedResolve:
        (void)feed.resolve(step.epoch, 1);
        break;
    case Kind::FeedFail:
        (void)feed.fail(step.epoch);
        break;
    case Kind::TagsFetch:
        (void)tags.begin_fetch();
        break;
    case Kind::TagsResolve:
        (void)tags.resolve(step.epoch, 1);
        break;
    case Kind::Sync:
    case Kind::Marked:
        break;
    }
}

} // namespace

int main() {
    const ir::IrModule module = tessera::test::codegen_pending::make_pending_resource_module();
    std::string codes;
    for (const ir::IrDiagnostic& diagnostic : ir::validate(module)) {
        codes += diagnostic.code + ":" + diagnostic.message + "; ";
    }
    check(codes.empty(), "the differential fixture is a valid IR module");
    const ir::Component& component = module.components[0];

    // Both sides read the same doubles, so any disagreement is a wiring difference.
    pend::FakeResource feed_double;
    pend::FakeResource tags_double;

    eval::PendingSession session;
    session.configure(component);
    session.bind_resource(0, pend::ResourceView::of(feed_double));
    session.bind_resource(1, pend::ResourceView::of(tags_double));
    eval::Evaluator evaluator;
    evaluator.set_pending_session(&session);
    eval::Scope scope;
    scope.emplace("state::query", eval::Value::of_string(""));
    check(session.boundary_count() == 1 && session.boundary_at(0).read_count() == 2,
          "the evaluator numbers one boundary with two read slots");

    pending::ResourceFeed feed;
    ui::Node root(1000);
    feed.build(root);
    feed.bind_res_feed(pend::ResourceView::of(feed_double));
    feed.bind_res_tags(pend::ResourceView::of(tags_double));

    const std::vector<Step> script = make_script();
    int index = 0;
    for (const Step& step : script) {
        apply_to_doubles(step, feed_double, tags_double);
        if (step.kind == Kind::Marked) {
            const auto ran = evaluator.run(component.handlers[0].actions, scope);
            check(ran.has_value(), "the evaluator runs the marked action list");
            feed.run_marked_set();
        }
        session.sync();
        feed.pending_sync();
        const Observation preview = observe(session);
        const Observation native = observe(feed);
        if (describe(preview) != describe(native)) {
            std::cerr << "FAIL: step " << index << " (" << step.label
                      << "): the evaluator and the generated code disagree\n  evaluator: "
                      << describe(preview) << "\n  generated: " << describe(native) << "\n";
            ++failures;
        }
        ++index;
    }

    // The script must have exercised both phases and the transition, or a pair of no-ops passes.
    check(index == static_cast<int>(script.size()), "every script step ran");
    check(feed.query() == "next", "the generated marked setter assigned its value");
    check(scope.at("state::query").as_string() != nullptr &&
              *scope.at("state::query").as_string() == "next",
          "the evaluator's marked setter assigned its value");
    check(observe(session).feed_status == "error" && observe(feed).feed_status == "error",
          "the script ends with a failed refresh on both sides");
    check(!observe(session).fallback_live && !observe(feed).fallback_live,
          "the failed refresh keeps the content live on both sides");
    check(!observe(session).is_pending && !observe(feed).is_pending,
          "the transition settled on both sides");

    // The evaluator reads a status through the pending session when the host bound one.
    const ir::ExprPtr status = ir::make_ref("res::feed.status");
    const auto value = evaluator.evaluate(*status, scope);
    check(value.has_value() && value->as_string() != nullptr && *value->as_string() == "error",
          "the evaluator reads res::feed.status from the session");

    if (failures != 0) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "pending resource parity: ok (" << index << " steps)\n";
    return 0;
}
