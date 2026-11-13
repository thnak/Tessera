// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/runtime/data_store.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace tessera::runtime::detail {

// One in-flight fetch: the epoch and dedupe slot a completion must still match to be published.
struct FetchState {
    DataKey key;
    std::uint64_t epoch = 0;
    CancellationSource cancellation;
};

// Shared by a DataStore and by the observer work it posts to the completion queue. The posted
// lambda owns it, so a destroyed store never leaves a dangling callback and a queued notification
// is harmless after destruction (`alive` is false).
struct DataStoreCore : std::enable_shared_from_this<DataStoreCore> {
    struct Entry {
        DataStatus status = DataStatus::Idle;
        std::shared_ptr<const void> value;
        const void* type = nullptr;
        std::optional<DataError> error;
        std::uint64_t version = 0;
        std::uint64_t epoch = 0;
        double updated_at = 0.0;
        std::uint64_t touch = 0;
        std::shared_ptr<FetchState> fetch;
    };

    struct ObserverSlot {
        DataStore::ObserverId id = 0;
        DataStore::Observer callback;
    };

    DataStoreCore(CompletionQueue& completion_queue, Clock& source_clock,
                  std::size_t entry_capacity);

    [[nodiscard]] DataSnapshot make_snapshot_locked(const DataKey& key, bool touch);
    void publish_locked(const DataKey& key, std::shared_ptr<const void> value, const void* type,
                        std::vector<std::shared_ptr<FetchState>>& cancelled);
    void mark_pending_locked(const DataKey& key);
    void schedule_notify_locked();
    void evict_overflow_locked(std::vector<std::shared_ptr<FetchState>>& cancelled);
    void drain_notifications();
    bool complete_fetch(const std::shared_ptr<FetchState>& state, std::shared_ptr<const void> value,
                        const void* type, std::optional<DataError> error);

    mutable std::mutex mutex;
    CompletionQueue* queue = nullptr;
    Clock* clock = nullptr;
    std::size_t capacity = 0;
    std::unordered_map<DataKey, Entry, DataKeyHash> entries;
    std::unordered_map<DataKey, std::vector<ObserverSlot>, DataKeyHash> observers;
    std::unordered_map<DataStore::ObserverId, DataKey> observer_keys;
    std::vector<DataKey> pending_order;
    std::unordered_set<DataKey, DataKeyHash> pending;
    DataStore::ObserverId next_observer_id = 1;
    std::uint64_t touch_counter = 0;
    std::uint64_t delivered = 0;
    bool notify_scheduled = false;
    bool alive = true;
};

} // namespace tessera::runtime::detail
