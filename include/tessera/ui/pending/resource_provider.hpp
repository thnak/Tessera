// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/ui/pending/resource_view.hpp"

#include <charconv>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <type_traits>

namespace tessera::ui::pending {

// The canonical, fixed-capacity encoding of a resource key (docs/specs/pending-ui-v1.md sections
// 3.1 and 10.2): a scalar or a record of scalars, written field by field in declaration order, with
// no pointers and no padding, so equal keys always produce equal bytes. A generated component
// writes its key here every sync and compares it with the last one; nothing allocates.
//
// One field is `name=<kind><payload>;` with kind `b` (bool), `i` (integer), `d` (float, the
// shortest round-trip text) or `s<length>:` (string, length-prefixed so no byte needs escaping). A
// bare scalar key is the single field with an empty name. A key longer than `kCapacity` sets
// `overflow()`; the component then leaves the resource unbound instead of binding a truncated
// key that could collide with another.
class ResourceKey {
public:
    static constexpr std::size_t kCapacity = 192;

    ResourceKey& field(std::string_view name, bool value) noexcept {
        begin(name);
        put(value ? "b1;" : "b0;");
        return *this;
    }
    template<std::integral T>
        requires(!std::is_same_v<T, bool>)
    ResourceKey& field(std::string_view name, T value) noexcept {
        begin(name);
        put('i');
        number(value);
        put(';');
        return *this;
    }
    ResourceKey& field(std::string_view name, double value) noexcept {
        begin(name);
        put('d');
        number(value);
        put(';');
        return *this;
    }
    ResourceKey& field(std::string_view name, std::string_view value) noexcept {
        begin(name);
        put('s');
        number(value.size());
        put(':');
        put(value);
        put(';');
        return *this;
    }
    ResourceKey& field(std::string_view name, const char* value) noexcept {
        return field(name, std::string_view(value));
    }

    // The bare-scalar form: `useResource(api.load, query)`.
    template<typename T> ResourceKey& value(const T& scalar) noexcept {
        return field(std::string_view{}, scalar);
    }

    [[nodiscard]] bool overflow() const noexcept {
        return overflow_;
    }
    [[nodiscard]] std::string_view view() const noexcept {
        return std::string_view(bytes_, size_);
    }
    [[nodiscard]] bool same_as(const ResourceKey& other) const noexcept {
        return !overflow_ && !other.overflow_ && view() == other.view();
    }

private:
    void begin(std::string_view name) noexcept {
        put(name);
        put('=');
    }
    void put(char c) noexcept {
        put(std::string_view(&c, 1));
    }
    void put(std::string_view text) noexcept {
        if (overflow_ || size_ + text.size() > kCapacity) {
            overflow_ = true;
            return;
        }
        for (const char c : text) {
            bytes_[size_++] = c;
        }
    }
    template<typename T> void number(T value) noexcept {
        char digits[40];
        const auto result = std::to_chars(digits, digits + sizeof digits, value);
        put(std::string_view(digits, static_cast<std::size_t>(result.ptr - digits)));
    }

    char bytes_[kCapacity] = {};
    std::size_t size_ = 0;
    bool overflow_ = false;
};

// A service that backs the resources of generated components (pending-ui-v1.md section 10.2). The
// component resolves the service through the usual `useService` seam (ADR-050) and, when the
// service type provides
//
//     ResourceView resource(std::string_view method, std::string_view key) noexcept;
//
// it asks for the view of (`method`, canonical `key`) at attach and again whenever the key changes.
// The service owns what the view reads (typically a `StoreResource` per key over the data store),
// starts the fetch for a key it has not seen, and keeps the viewed object alive for as long as the
// component can read it. A service without the member is not a provider: the host binds a view per
// resource, as before.
template<typename Service>
concept ResourceProvider =
    requires(Service& service, std::string_view method, std::string_view key) {
        { service.resource(method, key) } noexcept -> std::same_as<ResourceView>;
    };

// The view a provider gives for (`method`, `key`); an unbound view for a service that is not a
// provider. A function template, so generated code can call it for any service type: the member
// call sits behind a dependent `if constexpr`, which a call written directly in a generated
// (non-dependent) member body would not get.
template<typename Service>
[[nodiscard]] ResourceView provided_view(Service& service, std::string_view method,
                                         std::string_view key) noexcept {
    if constexpr (ResourceProvider<Service>) {
        return service.resource(method, key);
    } else {
        (void)service;
        (void)method;
        (void)key;
        return ResourceView{};
    }
}

} // namespace tessera::ui::pending
