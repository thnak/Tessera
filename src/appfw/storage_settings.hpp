// SPDX-License-Identifier: MIT
//
// The settings payload: an ordered list of (key, value) string pairs, encoded into the envelope's
// `Settings` kind. Kept out of `storage.cpp` so the store file stays inside its size budget and the
// codec is testable on its own.

#pragma once

#include "tessera/appfw/storage.hpp"

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace tessera::appfw::detail {

// Binary, length-prefixed: `u32 count`, then `u32 key_bytes | key | u32 value_bytes | value` per
// entry. Binary rather than JSON so the format has no escaping rules to get wrong and a value can
// hold any byte sequence; the restoration snapshot's JSON choice (state-restoration-v1 section 3)
// is a different artifact with a different audience.
[[nodiscard]] std::expected<std::string, StorageError>
encode_settings(const std::vector<std::pair<std::string, std::string>>& values,
                std::size_t max_payload);

[[nodiscard]] std::expected<std::vector<std::pair<std::string, std::string>>, StorageError>
decode_settings(std::string_view payload, std::size_t max_payload);

} // namespace tessera::appfw::detail
