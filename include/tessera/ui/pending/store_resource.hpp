// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/runtime/data_store.hpp"
#include "tessera/ui/pending/resource_view.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace tessera::ui::pending {

// The data store behind a `ResourceView` (docs/specs/pending-ui-v1.md sections 3.2 and 3.3):
// one key of one store, read through `DataStore::try_published`, which allocates nothing after
// warm-up and never waits.
//
// `state()` keeps the last state it read. When the store mutex is contended (a writer on another
// thread), the frame keeps rendering that state and the next read picks up the write; completion
// itself still arrives through the CompletionQueue at a tick boundary. The store and the adapter
// are UI-thread objects, so the cached state needs no synchronization of its own.
//
// Typed data (section 3.4). `StoreResource::typed<T>` publishes the stored value as the scalar
// `.data` of a resource whose service method returns `T` (bool, an integer, a float or a
// `std::string`): `number` for the first three, `text` for the string. A value of another type
// published under the key reads as absent, never as a wrong scalar. The text is a view into the
// stored value, which the adapter keeps alive until its next successful read. `.error` is the
// store's last failure message, in the same way.
//
// The adapter holds views into its own members, so it is neither copyable nor movable; the
// generated component views it by address.
class StoreResource {
public:
    StoreResource(const runtime::DataStore& store, runtime::DataKey key)
        : store_(&store),
          key_(std::move(key)) {}

    // A resource whose published value is a `T` scalar (`.data` of a typed resource).
    template<typename T>
    [[nodiscard]] static StoreResource typed(const runtime::DataStore& store,
                                             runtime::DataKey key) {
        static_assert(std::is_arithmetic_v<T> || std::is_same_v<T, std::string>,
                      "a typed resource publishes bool, an integer, a float or std::string");
        return StoreResource(store, std::move(key), runtime::detail::type_token<T>(),
                             [](const void* value, ResourceState& state) noexcept {
                                 if constexpr (std::is_same_v<T, std::string>) {
                                     state.text = *static_cast<const std::string*>(value);
                                 } else {
                                     state.number =
                                         static_cast<double>(*static_cast<const T*>(value));
                                 }
                             });
    }

    StoreResource(const StoreResource&) = delete;
    StoreResource& operator=(const StoreResource&) = delete;

    [[nodiscard]] const runtime::DataKey& key() const noexcept {
        return key_;
    }

    // A key change (a new epoch and, per async-data-v1 section 6, kept data): the cached state is
    // dropped only to the extent the next read refreshes it, so rebinding is just a new key.
    void rebind(runtime::DataKey key) {
        key_ = std::move(key);
        last_ = ResourceState{};
        payload_ = runtime::PublishedPayload{};
    }

    [[nodiscard]] ResourceState state() const noexcept {
        runtime::PublishedState published;
        if (store_->try_published(key_, published, payload_)) {
            last_ = ResourceState{published.epoch, published.fetching(), published.has_value,
                                  published.status == runtime::DataStatus::Error};
            if (extract_ != nullptr && payload_.value != nullptr && payload_.type == type_) {
                extract_(payload_.value.get(), last_);
            }
            last_.error = payload_.error_message;
        }
        return last_;
    }

private:
    using Extract = void (*)(const void* value, ResourceState& state) noexcept;

    StoreResource(const runtime::DataStore& store, runtime::DataKey key, const void* type,
                  Extract extract)
        : store_(&store),
          key_(std::move(key)),
          type_(type),
          extract_(extract) {}

    const runtime::DataStore* store_;
    runtime::DataKey key_;
    const void* type_ = nullptr;
    Extract extract_ = nullptr;
    mutable ResourceState last_;
    // Holds the stored value alive, so `text` stays valid until the next successful read.
    mutable runtime::PublishedPayload payload_;
};

} // namespace tessera::ui::pending
