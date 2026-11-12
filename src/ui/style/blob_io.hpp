// SPDX-License-Identifier: MIT

#pragma once

#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace tessera::ui::style::blob {

inline constexpr char kMagic[4] = {'T', 'S', 'S', 'B'};
// magic, format version, engine schema version, source hash, payload size.
inline constexpr std::size_t kHeaderSize = 4 + 4 + 4 + 8 + 4;
inline constexpr int kMaxSelectorDepth = 16;

// Little-endian byte writer with an interned string table ordered by first use.
class Writer {
public:
    void u8(std::uint8_t value) {
        body_.push_back(value);
    }
    void boolean(bool value) {
        u8(value ? 1 : 0);
    }
    void u32(std::uint32_t value) {
        for (int i = 0; i < 4; ++i) {
            body_.push_back(static_cast<std::uint8_t>(value >> (8 * i)));
        }
    }
    void u64(std::uint64_t value) {
        for (int i = 0; i < 8; ++i) {
            body_.push_back(static_cast<std::uint8_t>(value >> (8 * i)));
        }
    }
    void f64(double value) {
        std::uint64_t bits = 0;
        std::memcpy(&bits, &value, sizeof bits);
        u64(bits);
    }
    void count(std::size_t value) {
        u32(static_cast<std::uint32_t>(value));
    }
    void str(const std::string& value) {
        const auto found = lookup_.find(value);
        if (found != lookup_.end()) {
            u32(found->second);
            return;
        }
        const auto index = static_cast<std::uint32_t>(table_.size());
        table_.push_back(value);
        lookup_.emplace(value, index);
        u32(index);
    }

    [[nodiscard]] const std::vector<std::string>& table() const {
        return table_;
    }
    [[nodiscard]] const std::vector<std::uint8_t>& body() const {
        return body_;
    }

private:
    std::vector<std::uint8_t> body_;
    std::vector<std::string> table_;
    std::unordered_map<std::string, std::uint32_t> lookup_; // lookup only, never iterated
};

// Bounds-checked little-endian reader. Any failure sets `failed()`, after which every read
// returns zero/empty, so callers check once per structure.
class Reader {
public:
    explicit Reader(std::span<const std::uint8_t> data)
        : data_(data) {}

    [[nodiscard]] bool failed() const {
        return failed_;
    }
    void fail() {
        failed_ = true;
    }
    [[nodiscard]] std::size_t remaining() const {
        return data_.size() - pos_;
    }

    std::uint8_t u8() {
        if (!need(1)) {
            return 0;
        }
        return data_[pos_++];
    }
    bool boolean() {
        const std::uint8_t value = u8();
        if (value > 1) {
            failed_ = true;
        }
        return value == 1;
    }
    std::uint32_t u32() {
        if (!need(4)) {
            return 0;
        }
        std::uint32_t value = 0;
        for (int i = 0; i < 4; ++i) {
            value |= static_cast<std::uint32_t>(data_[pos_++]) << (8 * i);
        }
        return value;
    }
    std::uint64_t u64() {
        if (!need(8)) {
            return 0;
        }
        std::uint64_t value = 0;
        for (int i = 0; i < 8; ++i) {
            value |= static_cast<std::uint64_t>(data_[pos_++]) << (8 * i);
        }
        return value;
    }
    double f64() {
        const std::uint64_t bits = u64();
        double value = 0.0;
        std::memcpy(&value, &bits, sizeof value);
        return value;
    }
    // Element count; every element takes at least one byte, so a count above the remaining bytes
    // is corrupt and must not drive an allocation.
    std::size_t count() {
        const std::uint32_t value = u32();
        if (value > remaining()) {
            failed_ = true;
            return 0;
        }
        return value;
    }
    // Enum byte at most `max_value`.
    std::uint8_t enumeration(std::uint8_t max_value) {
        const std::uint8_t value = u8();
        if (value > max_value) {
            failed_ = true;
            return 0;
        }
        return value;
    }
    std::string raw_string() {
        const std::size_t length = count();
        if (failed_) {
            return {};
        }
        std::string value(reinterpret_cast<const char*>(data_.data() + pos_), length);
        pos_ += length;
        return value;
    }
    void set_table(std::vector<std::string> table) {
        table_ = std::move(table);
    }
    const std::string& str() {
        static const std::string kEmpty;
        const std::uint32_t index = u32();
        if (index >= table_.size()) {
            failed_ = true;
            return kEmpty;
        }
        return table_[index];
    }
    // Index into a table of `limit` entries, or `kNone` (UINT32_MAX) when `allow_none`.
    std::uint32_t index(std::size_t limit, bool allow_none) {
        const std::uint32_t value = u32();
        if (allow_none && value == UINT32_MAX) {
            return value;
        }
        if (value >= limit) {
            failed_ = true;
            return 0;
        }
        return value;
    }

private:
    bool need(std::size_t bytes) {
        if (failed_ || remaining() < bytes) {
            failed_ = true;
            return false;
        }
        return true;
    }

    std::span<const std::uint8_t> data_;
    std::size_t pos_ = 0;
    bool failed_ = false;
    std::vector<std::string> table_;
};

} // namespace tessera::ui::style::blob
