// SPDX-License-Identifier: MIT

// Shared helpers of the intrinsic element data headers.

#pragma once

#include "tessera/schema/intrinsics.hpp"

#include <cstddef>
#include <span>
#include <string_view>

namespace tessera::schema::internal::data {

template<typename Row, std::size_t N> constexpr std::span<const Row> rows(const Row (&list)[N]) {
    return std::span<const Row>(list, N);
}

template<std::size_t N>
constexpr std::span<const std::string_view> tags(const std::string_view (&list)[N]) {
    return std::span<const std::string_view>(list, N);
}

} // namespace tessera::schema::internal::data