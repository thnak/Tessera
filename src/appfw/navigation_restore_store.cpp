// SPDX-License-Identifier: MIT
//
// Persistence of the navigation restore snapshot through AF7's `Storage` (docs/specs/
// state-restoration-v1.md sections 3 and 7.1; issue #196).
//
// The seam is `appfw::Storage`, for the three properties the spec asks for and a hand-written file
// would not give:
//
//   * **Atomic** (section 3: "Write atomically (temp + rename); keep one previous good snapshot").
//   AF7
//     writes the complete payload to a temp file beside the target, flushes it, then replaces; a
//     crash, a full disk or a cancellation leaves the complete old content or the complete new one.
//   * **Off the UI thread** (section 7.1: "Never write on every keystroke/scroll tick; no blocking
//   I/O on
//     the UI thread"). `Storage::save` posts the blocking half to an `IoTaskExecutor` and delivers
//     the completion through the queue the UI thread drains, so nothing here can block regardless
//     of what a caller does.
//   * **Corruption is a value, not a crash** (section 6: "Corrupt/partial snapshots are discarded,
//   never
//     executed"). A corrupt entry is `StorageErrorCode::Corrupted`; this store maps every storage
//     failure into the module's own `RestoreError`, so a caller has one vocabulary to switch on.
//
// The store deliberately owns no debounce and no timer: state-restoration-v1.md section 7.1 puts
// the 1 s debounce and the force-on-background rule at the policy level ("Mark dirty on meaningful
// state change; debounce (1 s ...) and force on background/suspend/window close"), and a debounce
// needs a clock. A module that is deterministic by construction cannot own one, so this store saves
// when asked and the host's scheduler decides when that is.

#include "tessera/appfw/navigation_restore.hpp"

#include <cstddef>
#include <functional>
#include <string>
#include <string_view>
#include <utility>

namespace tessera::appfw {
namespace {

// The storage vocabulary the caller never has to see. `NotFound` is separated out because "no
// snapshot yet" is a first run, not a corruption: section 4's fallback to the route root is silent
// in that case.
RestoreError map_storage_error(const StorageError& error) {
    switch (error.code) {
    case StorageErrorCode::Corrupted:
        return RestoreError{RestoreErrorCode::MalformedSnapshot, std::string(error.step), 0};
    case StorageErrorCode::TooLarge:
        return RestoreError{RestoreErrorCode::TooLarge, std::string(error.step), 0};
    case StorageErrorCode::NotFound:
        return RestoreError{RestoreErrorCode::MalformedSnapshot, "not-found", 0};
    default:
        return RestoreError{RestoreErrorCode::MalformedSnapshot, std::string(error.step), 0};
    }
}

} // namespace

NavRestoreStore::NavRestoreStore(Storage& store, std::string entry_name, RestorePolicy policy)
    : store_(&store),
      entry_name_(std::move(entry_name)),
      policy_(policy) {}

bool NavRestoreStore::save(NavSnapshot snapshot, runtime::CancellationToken token,
                           SaveCallback completion) {
    auto encoded = encode_nav_snapshot(snapshot);
    if (!encoded.has_value()) {
        // The submit contract is "false means the callback never runs". An encoding failure is a
        // caller bug (a version this module cannot read, or an oversized state map), and reporting
        // it through the return value keeps that contract exact instead of running a completion the
        // caller would have to correlate with a `true` it did not get.
        return false;
    }
    if (encoded->size() > policy_.max_bytes) {
        return false;
    }
    SaveCallback callback = std::move(completion);
    return store_->save(entry_name_, std::move(*encoded), token,
                        [callback = std::move(callback)](std::expected<void, StorageError> result) {
                            if (!callback) {
                                return;
                            }
                            if (!result.has_value()) {
                                callback(std::unexpected(map_storage_error(result.error())));
                                return;
                            }
                            callback({});
                        });
}

bool NavRestoreStore::load(const RouteTable& table, runtime::CancellationToken token,
                           LoadCallback completion) {
    LoadCallback callback = std::move(completion);
    // The table is borrowed app-static data, so the completion can name it: `Storage::load`
    // delivers on the UI thread after this call returned, and the table outlives the store (it is
    // the app's route table).
    const RouteTable* route_table = &table;
    const RestorePolicy policy = policy_;
    return store_->load(entry_name_, token,
                        [callback = std::move(callback), route_table,
                         policy](std::expected<StoredValue, StorageError> result) {
                            if (!callback) {
                                return;
                            }
                            if (!result.has_value()) {
                                callback(std::unexpected(map_storage_error(result.error())));
                                return;
                            }
                            callback(decode_nav_snapshot(result->data, *route_table, policy));
                        });
}

} // namespace tessera::appfw
