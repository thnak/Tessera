// SPDX-License-Identifier: MIT
//
// `hal::NativeSurface` (ADR-020; ADR-050 section 7, roadmap AF6, issue #178).
//
// The whole file exists to make one ownership rule checkable: a `NativeSurface` borrows the opaque
// handle pair a HAL window reports and never owns it. The two mechanisms are (a) a **weak** witness
// of the owner's `SurfaceLifetime`, so a surface can never keep a window alive and reports invalid
// the moment the window is destroyed, and (b) a frozen `SurfaceDesc` plus a copied handle pair, so
// nothing here can reach back into the window to mutate or destroy it.
//
// There is deliberately no `std::function` teardown hook and no deleter: a pointer held here is a
// value, not a resource. That is what makes "destroying the surface does not destroy the window"
// true by construction rather than by convention.

#include "tessera/hal/surface.hpp"

#include <memory>
#include <utility>

namespace tessera::hal {
namespace {

constexpr SurfaceDesc kNoSurface{};

NativeWindowHandle no_handle() noexcept {
    return NativeWindowHandle{};
}

} // namespace

SurfaceLifetime::SurfaceLifetime()
    : liveness_(std::make_shared<SurfaceLiveness>()) {}

SurfaceLifetime::~SurfaceLifetime() {
    // The owner is going away: every surface borrowed from it must answer invalid rather than read
    // a handle the platform is about to release. Doing it here, not in the window's destructor
    // body, covers an owner destroyed by a `unique_ptr` reset, a stack unwind or a container clear.
    revoke();
}

void SurfaceLifetime::revoke() noexcept {
    if (liveness_ != nullptr) {
        liveness_->alive = false;
    }
}

bool SurfaceLifetime::alive() const noexcept {
    return liveness_ != nullptr && liveness_->alive;
}

std::weak_ptr<const SurfaceLiveness> SurfaceLifetime::witness() const noexcept {
    return liveness_;
}

// One immutable record per borrowed surface. Copies of a `NativeSurface` share it, so a copy is
// cheap, all copies go invalid together, and nothing is allocated after `borrow()`.
struct NativeSurface::State {
    State(std::weak_ptr<const SurfaceLiveness> owner, SurfaceDesc description,
          NativeWindowHandle handles, std::uint64_t surface_id) noexcept
        : witness(std::move(owner)),
          desc(description),
          handle(handles),
          id(surface_id) {}

    // Weak on purpose: the surface observes the owner, it never shares ownership of it.
    std::weak_ptr<const SurfaceLiveness> witness;
    SurfaceDesc desc;
    NativeWindowHandle handle{};
    std::uint64_t id = 0;
};

NativeSurface::NativeSurface(std::shared_ptr<const State> state) noexcept
    : state_(std::move(state)) {}

NativeSurface NativeSurface::borrow(const SurfaceLifetime& owner, const SurfaceDesc& desc,
                                    NativeWindowHandle handles, std::uint64_t surface_id) {
    // A caller that has no owner gets no surface. Returning an invalid surface here (instead of
    // storing a dangling borrow) keeps every later query total.
    auto state = std::make_shared<State>(owner.witness(), desc, handles, surface_id);
    return NativeSurface(std::move(state));
}

bool NativeSurface::valid() const noexcept {
    if (state_ == nullptr || !state_->desc.valid()) {
        return false;
    }
    // `lock()` is the entire ownership rule: it succeeds only while the owner is still alive. A
    // destroyed or revoked owner leaves the witness expired or marked, and both answer invalid, so
    // no path in this type can read a handle the platform already released.
    const std::shared_ptr<const SurfaceLiveness> owner = state_->witness.lock();
    return owner != nullptr && owner->alive;
}

SurfaceDesc NativeSurface::desc() const noexcept {
    return valid() ? state_->desc : kNoSurface;
}

std::uint64_t NativeSurface::id() const noexcept {
    return state_ != nullptr && valid() ? state_->id : 0;
}

NativeWindowHandle NativeSurface::native_handle() const noexcept {
    return valid() ? state_->handle : no_handle();
}

} // namespace tessera::hal
