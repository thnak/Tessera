// SPDX-License-Identifier: MIT

#include "data_store_core.hpp"
#include "tessera/runtime/clock.hpp"
#include "tessera/runtime/data_store.hpp"

#include <algorithm>
#include <mutex>
#include <stdexcept>
#include <vector>

namespace tessera::runtime {

std::size_t DataKeyHash::operator()(const DataKey& key) const noexcept {
    std::size_t hash = 0;
    const auto combine = [&hash](const std::string& part) {
        hash ^= std::hash<std::string>{}(part) + 0x9e3779b9U + (hash << 6U) + (hash >> 2U);
    };
    combine(key.type);
    combine(key.scope);
    combine(key.route);
    combine(key.locale);
    combine(key.params_hash);
    return hash;
}

DataStore::DataStore(CompletionQueue& queue, Clock& clock, std::size_t cache_capacity)
    : core_(std::make_shared<detail::DataStoreCore>(queue, clock, cache_capacity)) {
    if (cache_capacity == 0) {
        throw std::invalid_argument("data store cache capacity must be greater than zero");
    }
}

DataStore::~DataStore() {
    if (!core_) {
        return;
    }
    std::vector<std::shared_ptr<detail::FetchState>> cancelled;
    {
        std::lock_guard<std::mutex> lock(core_->mutex);
        core_->alive = false;
        for (auto& item : core_->entries) {
            if (item.second.fetch) {
                cancelled.push_back(std::move(item.second.fetch));
            }
        }
        core_->entries.clear();
        core_->observers.clear();
        core_->observer_keys.clear();
        core_->pending_order.clear();
        core_->pending.clear();
    }
    for (const std::shared_ptr<detail::FetchState>& state : cancelled) {
        state->cancellation.cancel();
    }
}

DataStore::DataStore(DataStore&&) noexcept = default;

DataStore& DataStore::operator=(DataStore&&) noexcept = default;

void DataStore::set_erased(const DataKey& key, std::shared_ptr<const void> value,
                           const void* type) {
    const std::shared_ptr<detail::DataStoreCore> core = core_;
    if (!core || !value) {
        return;
    }
    std::vector<std::shared_ptr<detail::FetchState>> cancelled;
    {
        std::lock_guard<std::mutex> lock(core->mutex);
        if (!core->alive) {
            return;
        }
        core->publish_locked(key, std::move(value), type, cancelled);
        core->evict_overflow_locked(cancelled);
        core->mark_pending_locked(key);
        core->schedule_notify_locked();
    }
    for (const std::shared_ptr<detail::FetchState>& state : cancelled) {
        state->cancellation.cancel();
    }
}

bool DataStore::update_erased(
    const DataKey& key, const void* type,
    const std::function<std::shared_ptr<const void>(const void*)>& transform) {
    const std::shared_ptr<detail::DataStoreCore> core = core_;
    if (!core) {
        return false;
    }
    for (;;) {
        std::shared_ptr<const void> current;
        std::uint64_t seen_version = 0;
        bool existed = false;
        {
            std::lock_guard<std::mutex> lock(core->mutex);
            if (!core->alive) {
                return false;
            }
            const auto it = core->entries.find(key);
            if (it != core->entries.end()) {
                if (it->second.type != type) {
                    return false;
                }
                existed = true;
                seen_version = it->second.version;
                current = it->second.value;
            }
        }

        std::shared_ptr<const void> next = transform(current.get());
        if (!next) {
            return false;
        }

        std::vector<std::shared_ptr<detail::FetchState>> cancelled;
        {
            std::lock_guard<std::mutex> lock(core->mutex);
            if (!core->alive) {
                return false;
            }
            const auto it = core->entries.find(key);
            if (existed) {
                if (it == core->entries.end() || it->second.version != seen_version ||
                    it->second.type != type) {
                    continue;
                }
            } else if (it != core->entries.end()) {
                continue;
            }
            core->publish_locked(key, std::move(next), type, cancelled);
            core->evict_overflow_locked(cancelled);
            core->mark_pending_locked(key);
            core->schedule_notify_locked();
        }
        for (const std::shared_ptr<detail::FetchState>& state : cancelled) {
            state->cancellation.cancel();
        }
        return true;
    }
}

DataSnapshot DataStore::snapshot(const DataKey& key) const {
    const std::shared_ptr<detail::DataStoreCore> core = core_;
    if (!core) {
        return DataSnapshot{};
    }
    std::lock_guard<std::mutex> lock(core->mutex);
    return core->make_snapshot_locked(key, true);
}

DataStore::ObserverId DataStore::observe(DataKey key, Observer observer) {
    if (!observer) {
        throw std::invalid_argument("data store observer must not be empty");
    }
    const std::shared_ptr<detail::DataStoreCore> core = core_;
    if (!core) {
        return 0;
    }
    std::lock_guard<std::mutex> lock(core->mutex);
    if (!core->alive) {
        return 0;
    }
    const ObserverId id = core->next_observer_id++;
    core->observers[key].push_back(detail::DataStoreCore::ObserverSlot{id, std::move(observer)});
    core->observer_keys.emplace(id, std::move(key));
    return id;
}

bool DataStore::unobserve(ObserverId id) {
    const std::shared_ptr<detail::DataStoreCore> core = core_;
    if (!core) {
        return false;
    }
    std::lock_guard<std::mutex> lock(core->mutex);
    const auto key_it = core->observer_keys.find(id);
    if (key_it == core->observer_keys.end()) {
        return false;
    }
    const auto observers_it = core->observers.find(key_it->second);
    if (observers_it != core->observers.end()) {
        std::vector<detail::DataStoreCore::ObserverSlot>& slots = observers_it->second;
        slots.erase(std::remove_if(slots.begin(), slots.end(),
                                   [id](const detail::DataStoreCore::ObserverSlot& slot) {
                                       return slot.id == id;
                                   }),
                    slots.end());
        if (slots.empty()) {
            core->observers.erase(observers_it);
        }
    }
    core->observer_keys.erase(key_it);
    return true;
}

FetchHandle DataStore::begin_fetch(const DataKey& key) {
    const std::shared_ptr<detail::DataStoreCore> core = core_;
    if (!core) {
        return {};
    }
    std::vector<std::shared_ptr<detail::FetchState>> cancelled;
    std::shared_ptr<detail::FetchState> state;
    {
        std::lock_guard<std::mutex> lock(core->mutex);
        if (!core->alive) {
            return {};
        }
        detail::DataStoreCore::Entry& entry = core->entries[key];
        if (entry.fetch) {
            // Concurrent requests for one key dedupe to the same task (spec section 2).
            state = entry.fetch;
        } else {
            state = std::make_shared<detail::FetchState>();
            state->key = key;
            state->epoch = ++entry.epoch;
            entry.fetch = state;
            entry.status = entry.value ? DataStatus::Refreshing : DataStatus::Loading;
            entry.touch = ++core->touch_counter;
            core->mark_pending_locked(key);
            core->evict_overflow_locked(cancelled);
            core->schedule_notify_locked();
        }
    }
    for (const std::shared_ptr<detail::FetchState>& fetch : cancelled) {
        fetch->cancellation.cancel();
    }
    return FetchHandle{core, std::move(state)};
}

std::size_t DataStore::size() const {
    const std::shared_ptr<detail::DataStoreCore> core = core_;
    if (!core) {
        return 0;
    }
    std::lock_guard<std::mutex> lock(core->mutex);
    return core->entries.size();
}

std::size_t DataStore::capacity() const {
    const std::shared_ptr<detail::DataStoreCore> core = core_;
    if (!core) {
        return 0;
    }
    return core->capacity;
}

std::uint64_t DataStore::notifications_delivered() const {
    const std::shared_ptr<detail::DataStoreCore> core = core_;
    if (!core) {
        return 0;
    }
    std::lock_guard<std::mutex> lock(core->mutex);
    return core->delivered;
}

CancellationToken FetchHandle::token() const {
    return state_ ? state_->cancellation.token() : CancellationToken{};
}

bool FetchHandle::resolve_erased(std::shared_ptr<const void> value, const void* type) {
    const std::shared_ptr<detail::DataStoreCore> core = core_.lock();
    if (!core || !state_ || !value) {
        return false;
    }
    return core->complete_fetch(state_, std::move(value), type, std::nullopt);
}

bool FetchHandle::fail(DataError error) {
    const std::shared_ptr<detail::DataStoreCore> core = core_.lock();
    if (!core || !state_) {
        return false;
    }
    return core->complete_fetch(state_, nullptr, nullptr, std::move(error));
}

} // namespace tessera::runtime
