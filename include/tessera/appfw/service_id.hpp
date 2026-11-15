// SPDX-License-Identifier: MIT
//
// Service identity for the application-framework layer (ADR-050 section 3 item 1). A service is
// named at compile time by the 64-bit FNV-1a hash of its reverse-DNS name, so the registry stores
// no string, needs no RTTI and allocates nothing on a lookup.

#pragma once

#include <concepts>
#include <cstdint>
#include <string_view>

namespace tessera::appfw {

struct ServiceId {
    std::uint64_t value = 0;
};

[[nodiscard]] constexpr bool operator==(ServiceId left, ServiceId right) noexcept {
    return left.value == right.value;
}

[[nodiscard]] constexpr bool operator!=(ServiceId left, ServiceId right) noexcept {
    return left.value != right.value;
}

[[nodiscard]] constexpr bool operator<(ServiceId left, ServiceId right) noexcept {
    return left.value < right.value;
}

// FNV-1a over the reverse-DNS name (`tessera.nav`, `com.example.mail-store`). The same function is
// what `service_id()` folds at compile time, so the id of a name is stable across translation units
// and across builds: it is part of the service's contract, not an implementation detail.
[[nodiscard]] constexpr std::uint64_t fnv1a(std::string_view text,
                                            std::uint64_t hash = 14695981039346656037ULL) noexcept {
    for (const char character : text) {
        hash ^= static_cast<std::uint64_t>(static_cast<unsigned char>(character));
        hash *= 1099511628211ULL;
    }
    return hash;
}

[[nodiscard]] consteval ServiceId service_id(std::string_view name) noexcept {
    return ServiceId{fnv1a(name)};
}

// A type that names its registry id. A service interface declares one; a concrete service with no
// interface declares it on itself.
//
//   class Navigator {
//   public:
//       static constexpr ServiceId kServiceId = service_id("tessera.nav");
//   };
template<typename T>
concept ProvidedService = requires {
    { T::kServiceId } -> std::convertible_to<ServiceId>;
};

// Secondary guard against one id being reused by two different types: the registry compares the
// address of a per-instantiation variable as well as the id (ADR-050 section 3 item 1). The id is
// the key, so a folded tag can only tighten this check, never mis-resolve a lookup.
template<typename T> inline constexpr char kServiceTypeTag = 0;

template<typename T> [[nodiscard]] const void* service_type_tag() noexcept {
    return &kServiceTypeTag<T>;
}

enum class ServiceErrorCode : std::uint8_t {
    CapacityExceeded,
    DuplicateId,
    TooManyDependencies,
    Frozen,
    AlreadyPrepared,
    MissingDependency,
    DependencyCycle,
    UnknownService,
    InvalidState,
};

struct ServiceError {
    ServiceErrorCode code = ServiceErrorCode::UnknownService;
    ServiceId service{};
    // The other endpoint of the edge; only meaningful for MissingDependency and DependencyCycle.
    ServiceId dependency{};
};

[[nodiscard]] constexpr std::string_view service_error_code_name(ServiceErrorCode code) noexcept {
    switch (code) {
    case ServiceErrorCode::CapacityExceeded:
        return "capacity exceeded";
    case ServiceErrorCode::DuplicateId:
        return "duplicate id";
    case ServiceErrorCode::TooManyDependencies:
        return "too many dependencies";
    case ServiceErrorCode::Frozen:
        return "context is frozen";
    case ServiceErrorCode::AlreadyPrepared:
        return "runner is already prepared";
    case ServiceErrorCode::MissingDependency:
        return "missing dependency";
    case ServiceErrorCode::DependencyCycle:
        return "dependency cycle";
    case ServiceErrorCode::UnknownService:
        return "unknown service";
    case ServiceErrorCode::InvalidState:
        return "invalid state";
    }
    return "unknown";
}

} // namespace tessera::appfw
