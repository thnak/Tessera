// SPDX-License-Identifier: MIT
//
// The resolution strategies compared by the benchmark: a frozen fixed-capacity registry keyed by a
// consteval id plus a per-type address tag (ADR-050 option B), and a naive
// std::unordered_map locator as the baseline. Services are 64 distinct synthetic types.
#pragma once

#include <algorithm>
#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <typeindex>
#include <typeinfo>
#include <unordered_map>
#include <utility>

namespace bench {

struct ServiceId {
    std::uint64_t value = 0;
};

consteval std::uint64_t fnv1a(std::string_view text, std::uint64_t hash = 14695981039346656037ULL) {
    for (char const c : text) {
        hash ^= static_cast<unsigned char>(c);
        hash *= 1099511628211ULL;
    }
    return hash;
}

// Id of "<prefix>.<index>" without building the string at compile time.
consteval ServiceId indexed_service_id(std::string_view prefix, int index) {
    std::uint64_t hash = fnv1a(prefix);
    hash = fnv1a(".", hash);
    for (int shift = 0; shift < 32; shift += 8) {
        hash ^= static_cast<std::uint64_t>((index >> shift) & 0xFF);
        hash *= 1099511628211ULL;
    }
    return ServiceId{hash};
}

// One distinct address per type: the tag the registry compares besides the id. No typeid.
template<typename T> inline constexpr char kTypeTag = 0;

template<typename T> constexpr void const* type_tag() noexcept {
    return &kTypeTag<T>;
}

// Interface used only by the virtual-dispatch case.
class ValueService {
public:
    virtual ~ValueService() = default;
    virtual std::uint64_t bump(std::uint64_t amount) = 0;

protected:
    ValueService() = default;
    ValueService(ValueService const&) = default;
    ValueService& operator=(ValueService const&) = default;
};

// `final`, so a call through Svc<N>& is static dispatch and can be inlined.
template<int N> class Svc final : public ValueService {
public:
    static constexpr ServiceId kServiceId = indexed_service_id("com.example.bench.svc", N);
    static constexpr int kIndex = N;

    std::uint64_t bump(std::uint64_t amount) override {
        value_ += amount;
        return value_;
    }

private:
    std::uint64_t value_ = N;
};

template<typename T>
concept ProvidedService = requires {
    { T::kServiceId } -> std::convertible_to<ServiceId>;
};

// Option B storage: {id, tag, pointer}, filled by provide(), sorted once by freeze(), then
// binary-searched. Fixed capacity: no allocation after construction.
class FrozenRegistry {
public:
    static constexpr std::size_t kCapacity = 64;

    template<ProvidedService T> bool provide(T& service) noexcept {
        if (frozen_ || size_ == kCapacity) {
            return false;
        }
        for (std::size_t i = 0; i < size_; ++i) {
            if (entries_[i].id == T::kServiceId.value) {
                return false; // duplicate id
            }
        }
        entries_[size_++] = Entry{T::kServiceId.value, type_tag<T>(), &service};
        return true;
    }

    void freeze() noexcept {
        std::sort(entries_.begin(), entries_.begin() + static_cast<std::ptrdiff_t>(size_),
                  [](Entry const& a, Entry const& b) { return a.id < b.id; });
        frozen_ = true;
    }

    template<ProvidedService T> T* find() const noexcept {
        auto const* const first = entries_.data();
        auto const* const last = first + size_;
        auto const* const it =
            std::lower_bound(first, last, T::kServiceId.value,
                             [](Entry const& entry, std::uint64_t id) { return entry.id < id; });
        if (it == last || it->id != T::kServiceId.value || it->tag != type_tag<T>()) {
            return nullptr;
        }
        return static_cast<T*>(it->service);
    }

    [[nodiscard]] std::size_t size() const noexcept {
        return size_;
    }

private:
    struct Entry {
        std::uint64_t id = 0;
        void const* tag = nullptr;
        void* service = nullptr;
    };

    std::array<Entry, kCapacity> entries_{};
    std::size_t size_ = 0;
    bool frozen_ = false;
};

// Naive baselines. Both need RTTI (typeid) or string keys; shown only for comparison.
class TypeIndexLocator {
public:
    template<typename T> void provide(T& service) {
        map_[std::type_index(typeid(T))] = &service;
    }

    template<typename T> T* find() const {
        auto const it = map_.find(std::type_index(typeid(T)));
        return it == map_.end() ? nullptr : static_cast<T*>(it->second);
    }

private:
    std::unordered_map<std::type_index, void*> map_;
};

class StringLocator {
public:
    void provide(std::string name, void* service) {
        map_[std::move(name)] = service;
    }

    // Takes std::string like a typical `locator.get("com.example.mail")` API; the key is built
    // per call.
    void* find(std::string const& name) const {
        auto const it = map_.find(name);
        return it == map_.end() ? nullptr : it->second;
    }

private:
    std::unordered_map<std::string, void*> map_;
};

} // namespace bench
