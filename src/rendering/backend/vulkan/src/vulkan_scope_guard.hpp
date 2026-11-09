// SPDX-License-Identifier: MIT

#pragma once

#include <utility>

namespace tessera::rendering::vulkan {

// Runs `cleanup` on scope exit unless dismissed. create_* functions make several Vulkan objects in
// a row and any step can throw; each object gets a guard until ownership moves to its wrapper, so
// a failure part-way releases exactly what already exists.
template<typename Cleanup> class ScopeGuard {
public:
    explicit ScopeGuard(Cleanup cleanup)
        : cleanup_(std::move(cleanup)) {}

    ~ScopeGuard() {
        if (active_) {
            cleanup_();
        }
    }

    ScopeGuard(const ScopeGuard&) = delete;
    ScopeGuard& operator=(const ScopeGuard&) = delete;
    ScopeGuard(ScopeGuard&&) = delete;
    ScopeGuard& operator=(ScopeGuard&&) = delete;

    void dismiss() {
        active_ = false;
    }

private:
    Cleanup cleanup_;
    bool active_ = true;
};

} // namespace tessera::rendering::vulkan
