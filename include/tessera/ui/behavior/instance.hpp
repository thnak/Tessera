// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/ui/behavior/behavior.hpp"
#include "tessera/ui/behavior/registry.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

// The behavior instance: the one wiring class both consumers of the IR share (ADR-064 item 7;
// docs/specs/behavior-hooks-v1.md section 9, docs/design/behavior-hooks.md section 6).
//
// The generated base and the design-time preview must not implement a behavior twice, so neither
// owns the lifecycle rules. Each owns one `Instance` per behavior and calls the same four methods:
//
//   attach(targets, services, args)  build the engine behavior from the registry and attach it
//   update(args)                     forward an update only when the hashed options changed
//   detach()                         detach and release the engine behavior
//   outputs()                        the current outputs, for the generated accessors
//
// **Why the hash lives here and not in each consumer.** The spec's rule is "call `update` only when
// the hash of the evaluated value options changed" (section 9). A generated base and a preview
// session that each hashed their own way would drift, and the drift would show up as a behavior
// that updates in one consumer and not the other. Hashing once, here, makes that impossible.
//
// **Allocation.** `attach` allocates the engine behavior through the registry factory (one
// allocation, the slot the generated base owns). `update` and `detach` allocate nothing: the hash
// is a value, and the args are a borrowed view. This is the contract
// `behavior_allocation_tests.cpp` pins.
namespace tessera::ui::behavior {

// A stable hash of the evaluated value options, in manifest order. Two calls with the same values
// hash equal, which is what makes `update` a no-op when nothing changed. A callback arg hashes its
// handler id, so re-binding a callback to a different handler is a change.
[[nodiscard]] std::uint64_t hash_args(std::span<const ArgValue> values);

class Instance {
public:
    Instance() = default;
    ~Instance();

    Instance(const Instance&) = delete;
    Instance& operator=(const Instance&) = delete;
    Instance(Instance&&) noexcept = default;
    Instance& operator=(Instance&&) noexcept = default;

    // Builds the engine behavior named `behavior` and attaches it to `targets`. Returns false when
    // the registry has no such behavior (a build-tool error: the manifest test catches it before a
    // user ever runs the app) or when a target is not attached yet, in which case nothing is built
    // and a later `attach` may still succeed.
    //
    // `behavior` is the IR's `Behavior::behavior` field, the engine registry name.
    bool attach(std::string_view behavior, const Targets& targets, const Services& services,
                const ArgsView& args);

    // The same for a behavior that takes no target (`useHotkeys`, whose scope is the window, and
    // the registered `transition`/`deferred_value` engines a host drives by hand). `attach` refuses
    // an empty target set on purpose: a behavior that declares targets and got none (a conditional
    // ref that did not resolve) must stay detached. Only a behavior whose manifest entry has no
    // targets calls this one, so the two cases cannot be confused. Returns false when the registry
    // has no such behavior.
    bool attach_untargeted(std::string_view behavior, const Services& services,
                           const ArgsView& args);

    // Forwards to the engine behavior only when the hash of `args` differs from the last attach or
    // update. Allocates nothing. A no-op when not attached.
    void update(const ArgsView& args);

    // Recomputes the outputs from the scene's current interaction state, WITHOUT consulting the
    // options hash. This is the second, separate trigger, and the distinction is not cosmetic:
    //
    //   `update`  the component's own state changed, so the evaluated options may have changed.
    //   `refresh` the SCENE moved on (a pointer moved, a press began, focus moved) while the
    //             component's options did not change at all.
    //
    // A behavior whose outputs are a function of interaction state — `press_feedback` is the first
    // — has no options, so its hash never changes and `update` would skip it forever. The caller
    // therefore calls `refresh` after a dispatch, which is what makes the outputs track the
    // pointer. A behavior with no interaction-state outputs simply recomputes the same values.
    void refresh(const ArgsView& args);

    // Forwards the frame's time to the engine behavior, for one whose outputs expire. Allocates
    // nothing and is a no-op when not attached.
    void tick(double now_s);

    // Detaches the engine behavior and releases it. Idempotent, and safe before the first attach.
    void detach();

    [[nodiscard]] bool attached() const noexcept {
        return behavior_ != nullptr;
    }

    // The current outputs, or an empty span when not attached. A consumer reads its manifest
    // defaults in that case rather than assuming a value.
    [[nodiscard]] std::span<const OutputValue> outputs() const;

    // The hash of the options the engine behavior currently holds. Exposed so a test can assert
    // that an unchanged update really was skipped rather than merely being cheap.
    [[nodiscard]] std::uint64_t args_hash() const noexcept {
        return args_hash_;
    }

private:
    std::unique_ptr<Behavior> behavior_;
    std::uint64_t args_hash_ = 0;
    bool has_hash_ = false;
};

} // namespace tessera::ui::behavior
