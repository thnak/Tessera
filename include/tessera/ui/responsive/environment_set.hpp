// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/ui/responsive/size_class.hpp"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace tessera::ui::responsive {

// One component's environment-value consumer: the generated `set_environment_<name>()` the host
// calls when a value changes. A plain function pointer plus a context, so the set allocates nothing
// per push and the generated base needs no virtual surface (ADR-024: no JS runtime, no RTTI).
struct EnvironmentConsumer {
    using Fn = void (*)(void* context, std::string_view value);

    // The provider this consumer reads (`breakpoint`, `theme`), so a push for one provider never
    // reaches a consumer of another.
    std::string_view provider;
    Fn set = nullptr;
    void* context = nullptr;
};

// The per-window set of environment consumers (docs/specs/jsx-subset-v1.md section 7.1, issue
// #692). Generated components register at mount and unregister at unmount; the host pushes a new
// value after every size or theme change.
//
// **Why a side table and not a `Node` field.** An environment value is a component's, not a node's,
// and a component is not a scene node: the generated base owns the field and the setter. The set is
// the host's, so it dies with the window, exactly like `LayoutObserverSet`.
//
// UI thread only, like everything it is called from.
class EnvironmentSet {
public:
    EnvironmentSet() = default;
    EnvironmentSet(const EnvironmentSet&) = delete;
    EnvironmentSet& operator=(const EnvironmentSet&) = delete;

    void reserve(std::size_t consumers);

    // Registers `consumer` (idempotent on the same `context` + `provider`). The consumer must
    // outlive its registration: a generated component detaches in `unmount()`.
    void attach(const EnvironmentConsumer& consumer);
    void detach(const EnvironmentConsumer& consumer);

    // Pushes `value` to every consumer of `provider`, in attach (mount) order. A consumer whose
    // generated setter is equality-gated turns an unchanged value into a no-op, so a resize that
    // stays inside one class costs one string comparison per consumer and no re-render.
    std::size_t push(std::string_view provider, std::string_view value);

    [[nodiscard]] std::size_t size() const {
        return consumers_.size();
    }
    [[nodiscard]] std::size_t size_of(std::string_view provider) const;

private:
    std::vector<EnvironmentConsumer> consumers_; // attach order
};

} // namespace tessera::ui::responsive
