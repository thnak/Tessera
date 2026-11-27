// SPDX-License-Identifier: MIT

#pragma once

#include <cstdint>

namespace tessera::ui::pending {

// What a suspending read needs to know about one resource at one instant
// (docs/specs/pending-ui-v1.md section 3.2): whether a fetch is in flight, whether data is
// published, whether the last fetch failed, and the epoch a completion must still match. Plain
// words, no key and no value, so reading and copying it never allocates.
struct ResourceState {
    std::uint64_t epoch = 0;
    bool loading = false;
    bool has_data = false;
    bool failed = false;

    // A suspending read: a fetch is in flight and there is nothing to show yet.
    [[nodiscard]] constexpr bool suspends() const noexcept {
        return loading && !has_data;
    }
    // Refreshing: a fetch is in flight and the previous data is still shown.
    [[nodiscard]] constexpr bool refreshing() const noexcept {
        return loading && has_data;
    }
};

// The narrow seam between a Suspense boundary and whatever owns the resource. It is a non-owning
// (object, function) pair rather than a virtual interface, so it costs no allocation, no RTTI and
// no vtable in a generated component, and an unbound view reads as an idle resource (nothing
// suspends) instead of crashing.
//
// Two implementations ship: the deterministic `FakeResource` (evaluator, tests, the Studio mock
// worker) and `StoreResource` over the data store. Any type with
// `ResourceState state() const noexcept` can be viewed; the viewed object must outlive the view.
class ResourceView {
public:
    constexpr ResourceView() noexcept = default;

    template<typename Source>
    [[nodiscard]] static constexpr ResourceView of(const Source& source) noexcept {
        ResourceView view;
        view.self_ = &source;
        view.read_ = [](const void* self) noexcept {
            return static_cast<const Source*>(self)->state();
        };
        return view;
    }

    [[nodiscard]] constexpr bool bound() const noexcept {
        return read_ != nullptr;
    }

    [[nodiscard]] ResourceState read() const noexcept {
        return read_ != nullptr ? read_(self_) : ResourceState{};
    }

private:
    const void* self_ = nullptr;
    ResourceState (*read_)(const void*) noexcept = nullptr;
};

} // namespace tessera::ui::pending
