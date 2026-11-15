// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/appfw/service_id.hpp"

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>

namespace tessera::appfw {

// ADR-050 section 3 item 5: resolution is legal only at the composition root, in a service's
// start(), and in a generated mount(). A lookup while a frame is being updated is a contract
// violation; AF1 measures it instead of trusting review. These counters are the diagnostic mirror
// of that rule (wiring them into the diagnostics registry belongs to a later slice).
struct ServiceLookupAudit {
    std::uint64_t in_frame = 0;
    std::uint64_t missing = 0;
};

[[nodiscard]] ServiceLookupAudit service_lookup_audit() noexcept;
void reset_service_lookup_audit() noexcept;

// Marks the region in which a service lookup is a contract violation (the update/render path, event
// handlers, effects). Nesting is counted, so a host may wrap a whole tick and an inner subsystem
// may wrap its own stage without cancelling the outer mark.
//
// AF1 deliberately does not wire this into `runtime::RuntimeLoop`: that is a layer-0 target and
// AF1 may not change it. It also could not wrap the whole tick, because a service's `start()`
// legally resolves its dependencies and a start resumes from the completion drain inside
// `RuntimeLoop::tick()`. The caller (AF3's `Application`) marks the update/render stage only.
class FrameScope {
public:
    FrameScope() noexcept;
    ~FrameScope();

    FrameScope(const FrameScope&) = delete;
    FrameScope& operator=(const FrameScope&) = delete;

    [[nodiscard]] static bool active() noexcept;
};

// Frozen, fixed-capacity service registry for one scope (ADR-050 section 3). Entries are
// `{id, type tag, pointer}`, sorted by id at `freeze()`; a lookup is a binary search with two
// integer compares, no RTTI and no allocation. A context may name a parent scope, so a window scope
// falls back to the app scope and a future route scope stays additive (open question 2's answer).
class ServiceContext {
public:
    static constexpr std::size_t kDefaultCapacity = 64;

    explicit ServiceContext(std::size_t capacity = kDefaultCapacity,
                            const ServiceContext* parent = nullptr);

    ServiceContext(const ServiceContext&) = delete;
    ServiceContext& operator=(const ServiceContext&) = delete;

    // Composition root only: registers `implementation` under `T::kServiceId`. Rejects a frozen or
    // full context, and an id that is already taken (including by a different type).
    template<ProvidedService T, std::derived_from<T> Impl>
    std::expected<void, ServiceError> provide(Impl& implementation) {
        return add(T::kServiceId, service_type_tag<T>(),
                   static_cast<void*>(static_cast<T*>(&implementation)));
    }

    // Sorts the entries so lookups are a binary search. Idempotent.
    std::expected<void, ServiceError> freeze();

    // A required service. A miss is a startup-validation bug, so it is recorded and terminates
    // instead of returning a reference to nothing; `find` is the nullable form.
    template<ProvidedService T> T& get() const {
        if (void* const service = lookup(T::kServiceId, service_type_tag<T>());
            service != nullptr) {
            return *static_cast<T*>(service);
        }
        detail_report_missing_service(T::kServiceId);
    }

    // Optional service: null when neither this scope nor a parent has the id, or when it is
    // registered under a different type.
    template<ProvidedService T> T* find() const noexcept {
        return static_cast<T*>(lookup(T::kServiceId, service_type_tag<T>()));
    }

    [[nodiscard]] bool contains(ServiceId id) const noexcept;

    [[nodiscard]] bool frozen() const noexcept {
        return frozen_;
    }
    [[nodiscard]] std::size_t size() const noexcept {
        return size_;
    }
    [[nodiscard]] std::size_t capacity() const noexcept {
        return capacity_;
    }
    [[nodiscard]] const ServiceContext* parent() const noexcept {
        return parent_;
    }

private:
    struct Entry {
        std::uint64_t id = 0;
        const void* tag = nullptr;
        void* service = nullptr;
    };

    // Walks this scope's chain. Before `freeze()` the entries are scanned linearly so a test may
    // look up early; the frozen path (the one on the hot contract) is the binary search.
    [[nodiscard]] void* lookup(ServiceId id, const void* tag) const noexcept;
    std::expected<void, ServiceError> add(ServiceId id, const void* tag, void* service);

    // Declared here because `get` is a template: records the miss and terminates.
    [[noreturn]] static void detail_report_missing_service(ServiceId id) noexcept;

    std::unique_ptr<Entry[]> entries_;
    std::size_t capacity_ = 0;
    std::size_t size_ = 0;
    bool frozen_ = false;
    const ServiceContext* parent_ = nullptr;
};

} // namespace tessera::appfw
