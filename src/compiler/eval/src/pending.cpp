// SPDX-License-Identifier: MIT

#include "tessera/compiler/eval/pending.hpp"

namespace tessera::compiler::eval {

void PendingSession::reset(std::size_t count) noexcept {
    resource_ = tessera::ui::pending::FakeResource{};
    boundary_.reset_reads(count);
    transition_.clear();
}

std::uint64_t PendingSession::begin_fetch() noexcept {
    return resource_.begin_fetch();
}

bool PendingSession::resolve(std::uint64_t epoch, int value) noexcept {
    return resource_.resolve(epoch, value);
}

bool PendingSession::fail(std::uint64_t epoch) noexcept {
    return resource_.fail(epoch);
}

void PendingSession::sync_read(std::size_t slot) noexcept {
    // The read is a subscription, not a fetch: it copies the resource's published state into the
    // boundary's slot, which is what a `useResource` observer does when the store notifies.
    (void)boundary_.set_read(slot, resource_.loading(), resource_.has_data(), resource_.epoch());
}

void PendingSession::begin_transition() noexcept {
    transition_.begin(resource_.epoch());
}

bool PendingSession::observe_transition() noexcept {
    return transition_.observe(resource_.loading(), resource_.epoch());
}

} // namespace tessera::compiler::eval
