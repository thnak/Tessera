// SPDX-License-Identifier: MIT

#include "tessera/appfw/service_context.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <utility>

namespace tessera::appfw {

namespace {

ServiceLookupAudit g_audit{};
int g_frame_depth = 0;

} // namespace

ServiceLookupAudit service_lookup_audit() noexcept {
    return g_audit;
}

void reset_service_lookup_audit() noexcept {
    g_audit = ServiceLookupAudit{};
}

FrameScope::FrameScope() noexcept {
    ++g_frame_depth;
}

FrameScope::~FrameScope() {
    --g_frame_depth;
}

bool FrameScope::active() noexcept {
    return g_frame_depth > 0;
}

ServiceContext::ServiceContext(std::size_t capacity, const ServiceContext* parent)
    : entries_(capacity == 0 ? nullptr : std::make_unique<Entry[]>(capacity)),
      capacity_(capacity),
      parent_(parent) {}

std::expected<void, ServiceError> ServiceContext::add(ServiceId id, const void* tag,
                                                      void* service) {
    if (frozen_) {
        return std::unexpected(ServiceError{ServiceErrorCode::Frozen, id, {}});
    }
    for (std::size_t index = 0; index < size_; ++index) {
        if (entries_[index].id != id.value) {
            continue;
        }
        // A second registration under the same id is ambiguous even when the types match: the
        // composition root must decide which object owns the contract (ADR-050 section 3 item 1).
        return std::unexpected(ServiceError{ServiceErrorCode::DuplicateId, id, {}});
    }
    if (size_ >= capacity_) {
        return std::unexpected(ServiceError{ServiceErrorCode::CapacityExceeded, id, {}});
    }
    entries_[size_++] = Entry{id.value, tag, service};
    return {};
}

std::expected<void, ServiceError> ServiceContext::freeze() {
    if (frozen_) {
        return {};
    }
    std::sort(entries_.get(), entries_.get() + static_cast<std::ptrdiff_t>(size_),
              [](const Entry& left, const Entry& right) { return left.id < right.id; });
    frozen_ = true;
    return {};
}

void* ServiceContext::lookup(ServiceId id, const void* tag) const noexcept {
    if (FrameScope::active()) {
        ++g_audit.in_frame;
    }
    for (const ServiceContext* scope = this; scope != nullptr; scope = scope->parent_) {
        if (!scope->frozen_) {
            for (std::size_t index = 0; index < scope->size_; ++index) {
                const Entry& entry = scope->entries_[index];
                if (entry.id == id.value && entry.tag == tag) {
                    return entry.service;
                }
            }
            continue;
        }
        const Entry* const first = scope->entries_.get();
        const Entry* const last = first + scope->size_;
        const Entry* const found =
            std::lower_bound(first, last, id.value,
                             [](const Entry& entry, std::uint64_t key) { return entry.id < key; });
        if (found != last && found->id == id.value && found->tag == tag) {
            return found->service;
        }
    }
    ++g_audit.missing;
    return nullptr;
}

bool ServiceContext::contains(ServiceId id) const noexcept {
    for (const ServiceContext* scope = this; scope != nullptr; scope = scope->parent_) {
        for (std::size_t index = 0; index < scope->size_; ++index) {
            if (scope->entries_[index].id == id.value) {
                return true;
            }
        }
    }
    return false;
}

void ServiceContext::detail_report_missing_service(ServiceId id) noexcept {
    ++g_audit.missing;
    // A required service that is absent is a startup-validation bug (ADR-050 section 3 item 6), so
    // the process stops with a named error instead of handing out a dangling reference. `find<T>()`
    // is the nullable form for every optional dependency.
    std::fprintf(stderr, "TESSERA_APPFW_SERVICE_MISSING: service id %llu is not registered\n",
                 static_cast<unsigned long long>(id.value));
    std::abort();
}

} // namespace tessera::appfw
