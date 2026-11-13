// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/runtime/async.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>

namespace tessera::runtime {

class Clock;

// Observable lifecycle of one resource (docs/specs/async-data-v1.md section 1). Cached is not a
// status: an entry that was never written reports Idle with no value.
enum class DataStatus { Idle, Loading, Refreshing, Success, Error, Stale };

// A value error (docs/specs/error-model-v1.md); the UI edge localizes it, the store only carries
// it.
struct DataError {
    std::string code;
    std::string message;

    [[nodiscard]] bool operator==(const DataError&) const = default;
};

// Canonical typed cache key (docs/specs/async-data-v1.md section 8.1). The caller serializes typed
// params canonically into `params_hash`; the store only compares and hashes the fields.
struct DataKey {
    std::string type;
    std::string scope;
    std::string route;
    std::string locale;
    std::string params_hash;

    [[nodiscard]] bool operator==(const DataKey&) const = default;
};

struct DataKeyHash {
    [[nodiscard]] std::size_t operator()(const DataKey& key) const noexcept;
};

namespace detail {

// RTTI-free type identity: function-local statics of an inline template are one object across
// translation units, which is all a typed push needs (no typeid, no dynamic_cast).
template<typename T> [[nodiscard]] const void* type_token() noexcept {
    static const int token = 0;
    return &token;
}

struct DataStoreCore;
struct FetchState;

} // namespace detail

// Immutable published view of one key. A snapshot owns the value; a later write never mutates it,
// so a UI observer can keep rendering the snapshot it received (docs/specs/async-data-v1.md
// section 1: "Data is immutable once published; UI reads snapshots, not mutable caches").
struct DataSnapshot {
    DataKey key;
    DataStatus status = DataStatus::Idle;
    std::shared_ptr<const void> value;
    const void* type = nullptr;
    std::optional<DataError> error;
    // Per-key write counter: 0 = never written, +1 on every accepted data write (push or fetch
    // success). Late fetch results superseded by a push never bump it.
    std::uint64_t version = 0;
    // Per-key staleness counter: +1 on every key/param/cancel change (fetch start, push, fetch
    // finish). A completion whose epoch no longer matches is discarded.
    std::uint64_t epoch = 0;
    // Seconds from the store clock at the write that produced `value`.
    double updated_at = 0.0;

    [[nodiscard]] bool has_value() const {
        return value != nullptr;
    }

    template<typename T> [[nodiscard]] std::shared_ptr<const T> value_as() const {
        if (type != detail::type_token<T>()) {
            return nullptr;
        }
        return std::static_pointer_cast<const T>(value);
    }
};

// One in-flight fetch. The app owns the completion; the store owns ordering, so this handle is the
// minimal fetch seam the push contract is specified against (transport/policy stay after V1).
// `resolve`/`fail` may be called from any thread and return false when the result is discarded: the
// fetch was cancelled or superseded (a push bumps the key epoch) or the entry was evicted. A
// discarded result never overwrites newer data and never notifies observers.
class FetchHandle {
public:
    FetchHandle() = default;

    [[nodiscard]] bool valid() const {
        return state_ != nullptr;
    }

    [[nodiscard]] CancellationToken token() const;

    template<typename T> bool resolve(std::shared_ptr<const T> value) {
        return resolve_erased(std::shared_ptr<const void>(std::move(value)),
                              detail::type_token<T>());
    }

    bool fail(DataError error);

private:
    friend class DataStore;
    FetchHandle(std::weak_ptr<detail::DataStoreCore> core,
                std::shared_ptr<detail::FetchState> state)
        : core_(std::move(core)),
          state_(std::move(state)) {}

    bool resolve_erased(std::shared_ptr<const void> value, const void* type);

    std::weak_ptr<detail::DataStoreCore> core_;
    std::shared_ptr<detail::FetchState> state_;
};

// Typed resource cache with the V1 app-fed push entry (issue #233, docs/specs/async-data-v1.md
// sections 8 and 8.1, G-21). No transport is implied: the app calls `set`/`update` from whatever
// source it already has, and observers are notified when the frame tick drains the completion
// queue.
//
// Contract:
// - `set`/`update` write the cache entry, mark it Success (fresh, `updated_at` set) and notify
//   observers once per key per tick on the thread that drains the completion queue (the UI thread).
//   They never run an observer inline and never block the caller beyond the store mutex.
// - Last write wins per key, with a monotonic `version`; pushes are accepted from any thread.
// - A push supersedes an in-flight fetch: it bumps the key epoch and cancels the fetch token, so
//   the late `resolve`/`fail` returns false and cannot overwrite the pushed value.
// - Pushed entries share the bounded cache (deterministic LRU by last write/read; the bound is
//   always honoured). Eviction does not notify; an observer of an evicted key sees Idle until the
//   key is written again.
//
// The completion queue must outlive the store. A store destruction drops pending observer work and
// cancels fetches; observers are never called after the store is gone.
class DataStore {
public:
    using ObserverId = std::uint64_t;
    using Observer = std::function<void(const DataSnapshot&)>;

    DataStore(CompletionQueue& queue, Clock& clock, std::size_t cache_capacity = 256);
    ~DataStore();

    DataStore(const DataStore&) = delete;
    DataStore& operator=(const DataStore&) = delete;
    DataStore(DataStore&&) noexcept;
    DataStore& operator=(DataStore&&) noexcept;

    // Push entry: publish `value` for `key` (last write wins) and mark the entry fresh.
    template<typename T> void set(const DataKey& key, std::shared_ptr<const T> value) {
        set_erased(key, std::shared_ptr<const void>(std::move(value)), detail::type_token<T>());
    }

    // Push entry: read-modify-write `key`. `fn` receives the current typed value (nullptr when the
    // key is absent) and returns the new value; it runs without the store lock, and the write is
    // retried if a concurrent write changed the key meanwhile. Returns false without writing when
    // the key holds another type or `fn` returns a null value.
    template<typename T, typename Fn> bool update(const DataKey& key, Fn&& fn) {
        return update_erased(key, detail::type_token<T>(),
                             [&fn](const void* current) -> std::shared_ptr<const void> {
                                 const T* previous = static_cast<const T*>(current);
                                 return std::make_shared<const T>(fn(previous));
                             });
    }

    // Latest published snapshot; `Idle` when the key was never written or was evicted. Reading
    // counts as a use for LRU eviction.
    [[nodiscard]] DataSnapshot snapshot(const DataKey& key) const;

    template<typename T> [[nodiscard]] std::shared_ptr<const T> get(const DataKey& key) const {
        return snapshot(key).value_as<T>();
    }

    // Subscribe to changes of one key. Observers are called on the draining thread, once per key
    // per tick, after at least one write/fetch-state change; they are not called by `observe`.
    ObserverId observe(DataKey key, Observer observer);
    bool unobserve(ObserverId id);

    // Start (or join, per-key dedupe) a fetch. The status becomes Loading, or Refreshing when
    // previous data is kept; observers see that at the next tick. No transport is started here.
    FetchHandle begin_fetch(const DataKey& key);

    [[nodiscard]] std::size_t size() const;
    [[nodiscard]] std::size_t capacity() const;
    // Observer callbacks dispatched so far (diagnostics/tests).
    [[nodiscard]] std::uint64_t notifications_delivered() const;

private:
    void set_erased(const DataKey& key, std::shared_ptr<const void> value, const void* type);
    bool update_erased(const DataKey& key, const void* type,
                       const std::function<std::shared_ptr<const void>(const void*)>& transform);

    std::shared_ptr<detail::DataStoreCore> core_;
};

} // namespace tessera::runtime
