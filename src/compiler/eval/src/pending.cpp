// SPDX-License-Identifier: MIT

#include "tessera/compiler/eval/pending.hpp"
#include "tessera/compiler/ir/suspense_reads.hpp"

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

void PendingSession::begin_transition(std::string_view target) noexcept {
    if (component_ == nullptr) {
        begin_transition();
        return;
    }
    const std::optional<std::size_t> keyed =
        tessera::compiler::ir::resource_keyed_by(*component_, target);
    if (!keyed || *keyed >= views_.size()) {
        return; // codegen rejects a marked set that writes no resource key; the preview ignores it
    }
    watched_ = *keyed;
    transition_.begin(views_[*keyed].read().epoch + 1);
}

void PendingSession::configure(const tessera::compiler::ir::Component& component) {
    component_ = &component;
    views_.assign(component.resources.size(), tessera::ui::pending::ResourceView{});
    const tessera::compiler::ir::SuspenseReads reads =
        tessera::compiler::ir::collect_suspense_reads(component);
    slots_.clear();
    boundaries_.assign(reads.boundaries.size(), tessera::ui::pending::SuspenseBoundary{});
    for (std::size_t boundary = 0; boundary < reads.boundaries.size(); ++boundary) {
        slots_.push_back(reads.boundaries[boundary].resources);
        boundaries_[boundary].reset_reads(slots_.back().size());
    }
    watched_ = static_cast<std::size_t>(-1);
    transition_.clear();
}

void PendingSession::bind_resource(std::size_t index,
                                   const tessera::ui::pending::ResourceView& view) {
    if (index < views_.size()) {
        views_[index] = view;
    }
}

void PendingSession::sync() noexcept {
    for (std::size_t boundary = 0; boundary < boundaries_.size(); ++boundary) {
        for (std::size_t slot = 0; slot < slots_[boundary].size(); ++slot) {
            const tessera::ui::pending::ResourceState state = views_[slots_[boundary][slot]].read();
            (void)boundaries_[boundary].set_read(slot, state.loading, state.has_data, state.epoch);
        }
    }
    if (transition_.started() && watched_ < views_.size()) {
        const tessera::ui::pending::ResourceState state = views_[watched_].read();
        (void)transition_.observe(state.loading, state.epoch);
    }
}

tessera::ui::pending::ResourceState
PendingSession::resource_state(std::size_t index) const noexcept {
    return index < views_.size() ? views_[index].read() : tessera::ui::pending::ResourceState{};
}

std::optional<std::string_view> PendingSession::status_of(std::string_view resource_id) const {
    if (component_ == nullptr) {
        return std::nullopt;
    }
    for (std::size_t index = 0; index < component_->resources.size(); ++index) {
        if (component_->resources[index].id == resource_id) {
            return std::string_view(views_[index].read().status());
        }
    }
    return std::nullopt;
}

bool PendingSession::observe_transition() noexcept {
    return transition_.observe(resource_.loading(), resource_.epoch());
}

} // namespace tessera::compiler::eval
