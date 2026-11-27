// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/compiler/ir/action.hpp"
#include "tessera/compiler/ir/component.hpp"
#include "tessera/ui/pending/fake_resource.hpp"
#include "tessera/ui/pending/resource_view.hpp"
#include "tessera/ui/pending/suspense.hpp"
#include "tessera/ui/pending/transition.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

namespace tessera::compiler::eval {

// The pending-UI model of one evaluation session (ADR-067 spike #661): a fake resource, the
// Suspense boundary over it, and the transition mark a setter-call handler raises.
//
// The evaluator is the reference implementation of what generated C++ must do
// (docs/specs/ir-v1.md section 4.1), so this session is the evaluator's half of the differential
// test: it runs the *same* `tessera::ui::pending` state machine the generated component runs, and
// the test compares the two after every step. Nothing here is a second implementation of the rule;
// it is the wiring around the one implementation.
//
// Allocation-free: the boundary, the mark and the resource are fixed-size members, and every
// operation is a plain assignment.
class PendingSession {
public:
    // The boundary covers `count` suspending reads (clamped to SuspenseBoundary::kMaxReads).
    void reset(std::size_t count) noexcept;

    // The fake resource: a fetch starts, a completion lands or is dropped by its epoch.
    [[nodiscard]] std::uint64_t begin_fetch() noexcept;
    [[nodiscard]] bool resolve(std::uint64_t epoch, int value) noexcept;
    [[nodiscard]] bool fail(std::uint64_t epoch) noexcept;

    // Publishes the resource's current state to read `slot` and re-evaluates the boundary. This is
    // what a `useResource` subscription does at a frame boundary: it never fetches in render.
    void sync_read(std::size_t slot) noexcept;

    // A transition-marked setter call: the mark starts under the resource's current epoch, so a
    // completion for an older epoch cannot clear it.
    void begin_transition() noexcept;
    // The same for the marked set of `target`. In resource mode the mark waits for the next epoch
    // of the resource whose key reads `target` (the host's fetch for the new key bumps it by one);
    // without `configure` it is the legacy call above.
    void begin_transition(std::string_view target) noexcept;

    // --- Resource mode (#656, docs/specs/pending-ui-v1.md section 10)
    // ----------------------------- The evaluator's half of a resource-driven component: the
    // session reads the component's resources through `ResourceView`s the test (or the Studio mock
    // worker) binds to resource doubles, feeds one boundary slot per suspending read in the order
    // codegen numbers them, and runs the same transition rule. `component` must outlive the
    // session.
    void configure(const tessera::compiler::ir::Component& component);
    void bind_resource(std::size_t index, const tessera::ui::pending::ResourceView& view);
    // Reads every bound resource once, publishes the boundary slots, and observes the transition:
    // what a subscription does at a frame boundary. Never fetches.
    void sync() noexcept;
    [[nodiscard]] bool configured() const noexcept {
        return component_ != nullptr;
    }
    [[nodiscard]] std::size_t boundary_count() const noexcept {
        return boundaries_.size();
    }
    [[nodiscard]] const tessera::ui::pending::SuspenseBoundary&
    boundary_at(std::size_t index) const {
        return boundaries_[index];
    }
    [[nodiscard]] tessera::ui::pending::ResourceState
    resource_state(std::size_t index) const noexcept;
    // The `res::<name>.status` value of the resource with this id, when the session knows it.
    [[nodiscard]] std::optional<std::string_view> status_of(std::string_view resource_id) const;
    // The resource's state after a marked change; false when the epoch was stale.
    [[nodiscard]] bool observe_transition() noexcept;

    [[nodiscard]] const tessera::ui::pending::SuspenseBoundary& boundary() const noexcept {
        return boundary_;
    }
    [[nodiscard]] const tessera::ui::pending::TransitionMark& transition() const noexcept {
        return transition_;
    }
    [[nodiscard]] const tessera::ui::pending::FakeResource& resource() const noexcept {
        return resource_;
    }
    // `isPending`: a transition is in flight (ADR-067 item 4).
    [[nodiscard]] bool is_pending() const noexcept {
        return transition_.pending();
    }
    // The subtree the boundary shows.
    [[nodiscard]] tessera::ui::pending::SuspensePhase phase() const noexcept {
        return boundary_.phase();
    }

private:
    const tessera::compiler::ir::Component* component_ = nullptr;
    std::vector<tessera::ui::pending::ResourceView> views_;
    std::vector<std::vector<std::size_t>> slots_; // per boundary: the resource of each read slot
    std::vector<tessera::ui::pending::SuspenseBoundary> boundaries_;
    std::size_t watched_ = static_cast<std::size_t>(-1); // the resource the mark follows
    tessera::ui::pending::FakeResource resource_;
    tessera::ui::pending::SuspenseBoundary boundary_;
    tessera::ui::pending::TransitionMark transition_;
};

} // namespace tessera::compiler::eval
