// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/runtime/data_store.hpp"
#include "tessera/ui/pending/resource_view.hpp"

#include <utility>

namespace tessera::ui::pending {

// The data store behind a `ResourceView` (docs/specs/pending-ui-v1.md sections 3.2 and 3.3):
// one key of one store, read through `DataStore::try_published`, which allocates nothing and never
// waits.
//
// `state()` keeps the last state it read. When the store mutex is contended (a writer on another
// thread), the frame keeps rendering that state and the next read picks up the write; completion
// itself still arrives through the CompletionQueue at a tick boundary. The store and the adapter
// are UI-thread objects, so the cached state needs no synchronization of its own.
class StoreResource {
public:
    StoreResource(const runtime::DataStore& store, runtime::DataKey key)
        : store_(&store),
          key_(std::move(key)) {}

    [[nodiscard]] const runtime::DataKey& key() const noexcept {
        return key_;
    }

    // A key change (a new epoch and, per async-data-v1 section 6, kept data): the cached state is
    // dropped only to the extent the next read refreshes it, so rebinding is just a new key.
    void rebind(runtime::DataKey key) {
        key_ = std::move(key);
        last_ = ResourceState{};
    }

    [[nodiscard]] ResourceState state() const noexcept {
        runtime::PublishedState published;
        if (store_->try_published(key_, published)) {
            last_ = ResourceState{published.epoch, published.fetching(), published.has_value,
                                  published.status == runtime::DataStatus::Error};
        }
        return last_;
    }

private:
    const runtime::DataStore* store_;
    runtime::DataKey key_;
    mutable ResourceState last_;
};

} // namespace tessera::ui::pending
