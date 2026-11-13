// SPDX-License-Identifier: MIT

#include "data_store_core.hpp"
#include "tessera/runtime/clock.hpp"

#include <utility>
#include <vector>

namespace tessera::runtime::detail {

DataStoreCore::DataStoreCore(CompletionQueue& completion_queue, Clock& source_clock,
                             std::size_t entry_capacity)
    : queue(&completion_queue),
      clock(&source_clock),
      capacity(entry_capacity) {}

DataSnapshot DataStoreCore::make_snapshot_locked(const DataKey& key, bool touch) {
    DataSnapshot snapshot;
    snapshot.key = key;
    const auto it = entries.find(key);
    if (it == entries.end()) {
        return snapshot;
    }
    Entry& entry = it->second;
    if (touch) {
        entry.touch = ++touch_counter;
    }
    snapshot.status = entry.status;
    snapshot.value = entry.value;
    snapshot.type = entry.type;
    snapshot.error = entry.error;
    snapshot.version = entry.version;
    snapshot.epoch = entry.epoch;
    snapshot.updated_at = entry.updated_at;
    return snapshot;
}

void DataStoreCore::publish_locked(const DataKey& key, std::shared_ptr<const void> value,
                                   const void* type,
                                   std::vector<std::shared_ptr<FetchState>>& cancelled) {
    Entry& entry = entries[key];
    if (entry.fetch) {
        // A push supersedes the in-flight fetch: clear the dedupe slot now (so a late completion
        // is rejected) and cancel the token outside the lock.
        cancelled.push_back(std::move(entry.fetch));
        entry.fetch.reset();
    }
    entry.status = DataStatus::Success;
    entry.value = std::move(value);
    entry.type = type;
    entry.error.reset();
    entry.updated_at = clock->now_seconds();
    ++entry.epoch;
    ++entry.version;
    entry.touch = ++touch_counter;
}

void DataStoreCore::mark_pending_locked(const DataKey& key) {
    if (!pending.insert(key).second) {
        return;
    }
    pending_order.push_back(key);
}

void DataStoreCore::schedule_notify_locked() {
    if (notify_scheduled || pending_order.empty() || queue == nullptr) {
        return;
    }
    const std::shared_ptr<DataStoreCore> self = weak_from_this().lock();
    if (!self) {
        return;
    }
    notify_scheduled =
        queue->post(CompletionPriority::Visible, [self] { self->drain_notifications(); });
}

void DataStoreCore::evict_overflow_locked(std::vector<std::shared_ptr<FetchState>>& cancelled) {
    // Deterministic LRU by last write/read: the smallest touch counter goes first. The entry just
    // written/created owns the largest counter, so it never evicts itself.
    while (entries.size() > capacity) {
        auto victim = entries.begin();
        for (auto it = entries.begin(); it != entries.end(); ++it) {
            if (it->second.touch < victim->second.touch) {
                victim = it;
            }
        }
        if (victim->second.fetch) {
            cancelled.push_back(std::move(victim->second.fetch));
        }
        entries.erase(victim);
    }
}

void DataStoreCore::drain_notifications() {
    struct Work {
        DataSnapshot snapshot;
        std::vector<DataStore::Observer> callbacks;
    };

    std::vector<Work> work;
    {
        std::lock_guard<std::mutex> lock(mutex);
        notify_scheduled = false;
        if (!alive) {
            return;
        }
        work.reserve(pending_order.size());
        for (const DataKey& key : pending_order) {
            Work item;
            item.snapshot = make_snapshot_locked(key, false);
            const auto it = observers.find(key);
            if (it != observers.end()) {
                item.callbacks.reserve(it->second.size());
                for (const ObserverSlot& slot : it->second) {
                    item.callbacks.push_back(slot.callback);
                }
            }
            delivered += item.callbacks.size();
            work.push_back(std::move(item));
        }
        pending_order.clear();
        pending.clear();
    }

    // Callbacks run outside the lock: an observer may read the store, push, or unsubscribe.
    for (Work& item : work) {
        for (const DataStore::Observer& callback : item.callbacks) {
            if (callback) {
                callback(item.snapshot);
            }
        }
    }
}

bool DataStoreCore::complete_fetch(const std::shared_ptr<FetchState>& state,
                                   std::shared_ptr<const void> value, const void* type,
                                   std::optional<DataError> error) {
    std::lock_guard<std::mutex> lock(mutex);
    if (!alive) {
        return false;
    }
    const auto it = entries.find(state->key);
    if (it == entries.end() || it->second.fetch != state || it->second.epoch != state->epoch) {
        return false;
    }
    Entry& entry = it->second;
    entry.fetch.reset();
    if (value) {
        entry.value = std::move(value);
        entry.type = type;
        entry.error.reset();
        entry.status = DataStatus::Success;
        entry.updated_at = clock->now_seconds();
        ++entry.version;
    } else {
        // A failed refresh keeps the previous published data (spec section 1/6).
        entry.error = std::move(error);
        entry.status = DataStatus::Error;
    }
    mark_pending_locked(state->key);
    schedule_notify_locked();
    return true;
}

} // namespace tessera::runtime::detail
