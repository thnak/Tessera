// SPDX-License-Identifier: MIT

#include "storage_settings.hpp"

#include <cstdint>

namespace tessera::appfw::detail {
namespace {

void put_u32(std::string& out, std::uint32_t value) {
    for (int shift = 0; shift < 32; shift += 8) {
        out.push_back(static_cast<char>((value >> static_cast<unsigned>(shift)) & 0xFFu));
    }
}

[[nodiscard]] bool read_u32(std::string_view bytes, std::size_t& offset, std::uint32_t& value) {
    if (offset + 4 > bytes.size()) {
        return false;
    }
    value = 0;
    for (int index = 3; index >= 0; --index) {
        value = (value << 8u) |
                static_cast<unsigned char>(bytes[offset + static_cast<std::size_t>(index)]);
    }
    offset += 4;
    return true;
}

[[nodiscard]] bool read_string(std::string_view bytes, std::size_t& offset, std::string& value) {
    std::uint32_t length = 0;
    if (!read_u32(bytes, offset, length)) {
        return false;
    }
    if (offset + length > bytes.size()) {
        return false;
    }
    value.assign(bytes.substr(offset, length));
    offset += length;
    return true;
}

template<typename T> [[nodiscard]] std::expected<T, StorageError> corrupted(std::string_view step) {
    return std::unexpected(StorageError{StorageErrorCode::Corrupted, step});
}

} // namespace

std::expected<std::string, StorageError>
encode_settings(const std::vector<std::pair<std::string, std::string>>& values,
                std::size_t max_payload) {
    std::size_t total = 4;
    for (const auto& [key, value] : values) {
        total += 8 + key.size() + value.size();
    }
    if (total > max_payload) {
        return std::unexpected(StorageError{StorageErrorCode::TooLarge, "encode-settings"});
    }

    std::string bytes;
    bytes.reserve(total);
    put_u32(bytes, static_cast<std::uint32_t>(values.size()));
    for (const auto& [key, value] : values) {
        put_u32(bytes, static_cast<std::uint32_t>(key.size()));
        bytes.append(key);
        put_u32(bytes, static_cast<std::uint32_t>(value.size()));
        bytes.append(value);
    }
    return bytes;
}

std::expected<std::vector<std::pair<std::string, std::string>>, StorageError>
decode_settings(std::string_view payload, std::size_t max_payload) {
    if (payload.size() > max_payload) {
        return std::unexpected(StorageError{StorageErrorCode::TooLarge, "decode-settings"});
    }

    std::size_t offset = 0;
    std::uint32_t count = 0;
    if (!read_u32(payload, offset, count)) {
        return corrupted<Settings>("settings-count");
    }
    // Every entry needs at least 8 bytes of length prefixes, so a count larger than that bound is a
    // corrupt file rather than an allocation the process should attempt.
    if (static_cast<std::uint64_t>(count) * 8u > payload.size()) {
        return corrupted<Settings>("settings-count");
    }

    std::vector<std::pair<std::string, std::string>> values;
    values.reserve(count);
    for (std::uint32_t index = 0; index < count; ++index) {
        std::string key;
        std::string value;
        if (!read_string(payload, offset, key) || !read_string(payload, offset, value)) {
            return corrupted<Settings>("settings-entry");
        }
        values.emplace_back(std::move(key), std::move(value));
    }
    if (offset != payload.size()) {
        return corrupted<Settings>("settings-tail");
    }
    return values;
}

} // namespace tessera::appfw::detail
