// SPDX-License-Identifier: MIT
//
// Registration, dependency ordering and the public surface of `ServiceRunner`. The transition
// sequence itself lives in `service_runner_sequence.cpp`.

#include "service_runner_impl.hpp"
#include "tessera/appfw/service_runner.hpp"

#include <utility>

namespace tessera::appfw {

detail::RunnerState::RunnerState(runtime::Clock& clock, runtime::CompletionQueue& completions,
                                 const ServiceContext* services, ServiceRunnerConfig config)
    : clock(&clock),
      completions(&completions),
      services(services),
      config(config),
      slots(std::make_unique<Slot[]>(config.max_services == 0 ? 1 : config.max_services)),
      placed(std::make_unique<bool[]>(config.max_services == 0 ? 1 : config.max_services)),
      order_ids(std::make_unique<ServiceId[]>(config.max_services == 0 ? 1 : config.max_services)),
      order_slots(
          std::make_unique<std::size_t[]>(config.max_services == 0 ? 1 : config.max_services)) {}

std::size_t detail::RunnerState::find_slot(ServiceId id) const noexcept {
    // A linear scan is the right shape here: registration and `state_of` are composition-root and
    // diagnostic calls, and the hot contract (§3 item 5) is the frozen `ServiceContext` lookup.
    for (std::size_t index = 0; index < registered; ++index) {
        if (slots[index].id == id.value) {
            return index;
        }
    }
    return kNoSlot;
}

std::expected<void, ServiceError> detail::RunnerState::add(const ServiceRegistration& registration,
                                                           Service& service) {
    if (prepared) {
        return std::unexpected(ServiceError{ServiceErrorCode::Frozen, registration.id, {}});
    }
    if (registration.depends_on.size() > ServiceRegistration::kMaxDependencies) {
        return std::unexpected(
            ServiceError{ServiceErrorCode::TooManyDependencies, registration.id, {}});
    }
    if (registered >= config.max_services) {
        return std::unexpected(
            ServiceError{ServiceErrorCode::CapacityExceeded, registration.id, {}});
    }
    if (find_slot(registration.id) != kNoSlot) {
        return std::unexpected(ServiceError{ServiceErrorCode::DuplicateId, registration.id, {}});
    }
    Slot& slot = slots[registered];
    slot.id = registration.id.value;
    slot.service = &service;
    slot.required = registration.required;
    slot.dependency_count = registration.depends_on.size();
    for (std::size_t index = 0; index < slot.dependency_count; ++index) {
        slot.dependencies[index] = registration.depends_on[index].value;
    }
    ++registered;
    return {};
}

std::expected<void, ServiceError> detail::RunnerState::prepare() {
    if (prepared) {
        return std::unexpected(ServiceError{ServiceErrorCode::AlreadyPrepared, ServiceId{}, {}});
    }
    if (phase != ServicePhase::Idle) {
        return std::unexpected(ServiceError{ServiceErrorCode::InvalidState, ServiceId{}, {}});
    }
    // Every dependency must exist before anything is ordered: startup validation, not a runtime
    // miss (ADR-050 section 3 item 6).
    for (std::size_t index = 0; index < registered; ++index) {
        Slot& slot = slots[index];
        for (std::size_t dependency = 0; dependency < slot.dependency_count; ++dependency) {
            const std::size_t resolved = find_slot(ServiceId{slot.dependencies[dependency]});
            if (resolved == kNoSlot) {
                return std::unexpected(ServiceError{ServiceErrorCode::MissingDependency,
                                                    ServiceId{slot.id},
                                                    ServiceId{slot.dependencies[dependency]}});
            }
            slot.dependency_slots[dependency] = resolved;
        }
    }

    // Kahn's algorithm with a deterministic tie-break: always take the lowest registration index
    // whose dependencies are already ordered. Two services with no ordering constraint between them
    // therefore start in registration order, which makes the sequence reproducible.
    for (std::size_t index = 0; index < registered; ++index) {
        placed[index] = false;
    }
    order_size = 0;
    while (order_size < registered) {
        std::size_t chosen = kNoSlot;
        for (std::size_t candidate = 0; candidate < registered; ++candidate) {
            if (placed[candidate]) {
                continue;
            }
            bool ready = true;
            for (std::size_t dependency = 0; dependency < slots[candidate].dependency_count;
                 ++dependency) {
                if (!placed[slots[candidate].dependency_slots[dependency]]) {
                    ready = false;
                    break;
                }
            }
            if (ready) {
                chosen = candidate;
                break;
            }
        }
        if (chosen == kNoSlot) {
            // Every remaining slot waits on another remaining slot: a cycle. Report one concrete
            // edge so the startup diagnostic names something actionable.
            for (std::size_t candidate = 0; candidate < registered; ++candidate) {
                if (placed[candidate]) {
                    continue;
                }
                for (std::size_t dependency = 0; dependency < slots[candidate].dependency_count;
                     ++dependency) {
                    const std::size_t edge = slots[candidate].dependency_slots[dependency];
                    if (!placed[edge]) {
                        return std::unexpected(ServiceError{ServiceErrorCode::DependencyCycle,
                                                            ServiceId{slots[candidate].id},
                                                            ServiceId{slots[edge].id}});
                    }
                }
            }
            return std::unexpected(
                ServiceError{ServiceErrorCode::DependencyCycle, ServiceId{}, {}});
        }
        placed[chosen] = true;
        order_slots[order_size] = chosen;
        order_ids[order_size] = ServiceId{slots[chosen].id};
        ++order_size;
    }
    prepared = true;
    return {};
}

ServiceRunner::ServiceRunner(runtime::Clock& clock, runtime::CompletionQueue& completions,
                             const ServiceContext* services, ServiceRunnerConfig config)
    : services_(services),
      impl_(std::make_unique<detail::RunnerState>(clock, completions, services, config)) {}

ServiceRunner::~ServiceRunner() = default;

std::expected<void, ServiceError> ServiceRunner::add(const ServiceRegistration& registration,
                                                     Service& service) {
    return impl_->add(registration, service);
}

std::expected<void, ServiceError> ServiceRunner::prepare() {
    return impl_->prepare();
}

bool ServiceRunner::begin_start() {
    return impl_->begin_start();
}

bool ServiceRunner::begin_stop() {
    return impl_->begin_stop();
}

void ServiceRunner::tick() {
    impl_->tick();
}

ServicePhase ServiceRunner::phase() const noexcept {
    return impl_->phase;
}

bool ServiceRunner::prepared() const noexcept {
    return impl_->prepared;
}

bool ServiceRunner::sequence_active() const noexcept {
    return impl_->inflight != detail::RunnerState::kNoSlot;
}

ServiceRunnerStats ServiceRunner::stats() const noexcept {
    ServiceRunnerStats stats = impl_->stats;
    stats.registered = impl_->registered;
    return stats;
}

bool ServiceRunner::required_failure() const noexcept {
    return impl_->required_failure;
}

std::span<const ServiceId> ServiceRunner::start_order() const noexcept {
    return std::span<const ServiceId>(impl_->order_ids.get(), impl_->order_size);
}

ServiceState ServiceRunner::state_of(ServiceId id) const noexcept {
    const std::size_t slot = impl_->find_slot(id);
    return slot == detail::RunnerState::kNoSlot ? ServiceState::Registered
                                                : impl_->slots[slot].state;
}

} // namespace tessera::appfw
