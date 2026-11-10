// SPDX-License-Identifier: MIT

#include "tessera/ui/accessibility/dbus_wire.hpp"

#include <bit>
#include <cstring>

namespace tessera::ui::a11y {
namespace {

std::expected<void, WireError> skip_single(MessageReader& reader, std::string_view signature,
                                           std::size_t depth) {
    if (signature.empty() || depth > kMaxWireDepth) {
        return std::unexpected(WireError::Unbalanced);
    }
    switch (signature.front()) {
    case 'y':
        (void)reader.read_byte();
        break;
    case 'b':
        (void)reader.read_bool();
        break;
    case 'n':
        (void)reader.read_int16();
        break;
    case 'q':
        (void)reader.read_uint16();
        break;
    case 'i':
        (void)reader.read_int32();
        break;
    case 'u':
        (void)reader.read_uint32();
        break;
    case 'x':
        (void)reader.read_int64();
        break;
    case 't':
        (void)reader.read_uint64();
        break;
    case 'd':
        (void)reader.read_double();
        break;
    case 's':
    case 'o':
        (void)reader.read_string();
        break;
    case 'g':
        (void)reader.read_signature();
        break;
    case 'h':
        // A UNIX fd travels as a uint32 index into the message's fd array.
        (void)reader.read_uint32();
        break;
    case 'v': {
        const auto inner = reader.read_variant_signature();
        if (!inner) {
            return std::unexpected(inner.error());
        }
        return skip_single(reader, *inner, depth + 1);
    }
    case 'a': {
        const std::string_view element = signature.substr(1);
        if (element.empty()) {
            return std::unexpected(WireError::BadSignature);
        }
        const auto header = reader.read_array_header(element.front());
        if (!header) {
            return header;
        }
        while (!reader.array_done()) {
            const auto skipped = skip_single(reader, element, depth + 1);
            if (!skipped) {
                return skipped;
            }
        }
        return reader.finish_array();
    }
    case '(':
    case '{': {
        if (signature.size() < 2 || signature.back() != ')') {
            return std::unexpected(WireError::BadSignature);
        }
        if (signature.front() == '(') {
            const auto entered = reader.begin_struct();
            if (!entered) {
                return entered;
            }
        }
        std::string_view members = signature.substr(1, signature.size() - 2);
        while (!members.empty()) {
            const std::size_t length = complete_type_length(members);
            if (length == 0) {
                return std::unexpected(WireError::BadSignature);
            }
            const auto skipped = skip_single(reader, members.substr(0, length), depth + 1);
            if (!skipped) {
                return skipped;
            }
            members.remove_prefix(length);
        }
        if (signature.front() == '(') {
            reader.end_struct();
        }
        break;
    }
    default:
        return std::unexpected(WireError::BadType);
    }
    if (reader.failed()) {
        return std::unexpected(reader.error());
    }
    return {};
}

} // namespace

MessageReader::MessageReader(std::span<const std::uint8_t> bytes, ByteOrder order)
    : bytes_(bytes),
      order_(order) {
    limits_[0] = bytes.size();
}

bool MessageReader::can_read(std::size_t count) const {
    return offset_ <= limit() && count <= limit() - offset_;
}

void MessageReader::fail(WireError error) {
    if (error_ == WireError::None) {
        error_ = error;
    }
}

std::size_t MessageReader::limit() const {
    return limits_[depth_];
}

bool MessageReader::exhausted() const {
    return offset_ >= limit();
}

void MessageReader::seek(std::size_t offset) {
    if (offset > limit()) {
        fail(WireError::Truncated);
        return;
    }
    offset_ = offset;
}

void MessageReader::push_limit(std::size_t limit) {
    if (depth_ + 1 >= kMaxWireDepth) {
        fail(WireError::Unbalanced);
        return;
    }
    ++depth_;
    limits_[depth_] = limit < limits_[depth_ - 1] ? limit : limits_[depth_ - 1];
}

void MessageReader::pop_limit() {
    if (depth_ == 0) {
        fail(WireError::Unbalanced);
        return;
    }
    --depth_;
}

std::expected<void, WireError> MessageReader::align(std::size_t alignment) {
    if (error_ != WireError::None) {
        return std::unexpected(error_);
    }
    if (alignment <= 1) {
        return {};
    }
    const std::size_t remainder = offset_ % alignment;
    if (remainder == 0) {
        return {};
    }
    const std::size_t padding = alignment - remainder;
    if (!can_read(padding)) {
        fail(WireError::Truncated);
        return std::unexpected(error_);
    }
    offset_ += padding;
    return {};
}

std::uint32_t MessageReader::read_u32_unchecked() {
    std::uint32_t value = 0;
    std::memcpy(&value, bytes_.data() + offset_, sizeof(value));
    offset_ += sizeof(value);
    if (order_ != host_byte_order()) {
        value = std::byteswap(value);
    }
    return value;
}

std::uint64_t MessageReader::read_u64_unchecked() {
    std::uint64_t value = 0;
    std::memcpy(&value, bytes_.data() + offset_, sizeof(value));
    offset_ += sizeof(value);
    if (order_ != host_byte_order()) {
        value = std::byteswap(value);
    }
    return value;
}

std::uint8_t MessageReader::read_byte() {
    if (!can_read(sizeof(std::uint8_t))) {
        fail(WireError::Truncated);
        return 0;
    }
    return bytes_[offset_++];
}

bool MessageReader::read_bool() {
    const std::uint32_t value = read_uint32();
    if (error_ != WireError::None) {
        return false;
    }
    if (value > 1U) {
        fail(WireError::ValueOutOfRange);
        return false;
    }
    return value == 1U;
}

std::int16_t MessageReader::read_int16() {
    if (auto aligned = align(2); !aligned) {
        return 0;
    }
    if (!can_read(2)) {
        fail(WireError::Truncated);
        return 0;
    }
    std::uint16_t raw = 0;
    std::memcpy(&raw, bytes_.data() + offset_, sizeof(raw));
    offset_ += sizeof(raw);
    if (order_ != host_byte_order()) {
        raw = std::byteswap(raw);
    }
    std::int16_t value = 0;
    std::memcpy(&value, &raw, sizeof(value));
    return value;
}

std::uint16_t MessageReader::read_uint16() {
    if (auto aligned = align(2); !aligned) {
        return 0;
    }
    if (!can_read(2)) {
        fail(WireError::Truncated);
        return 0;
    }
    std::uint16_t raw = 0;
    std::memcpy(&raw, bytes_.data() + offset_, sizeof(raw));
    offset_ += sizeof(raw);
    if (order_ != host_byte_order()) {
        raw = std::byteswap(raw);
    }
    return raw;
}

std::int32_t MessageReader::read_int32() {
    const std::uint32_t raw = read_uint32();
    std::int32_t value = 0;
    std::memcpy(&value, &raw, sizeof(value));
    return value;
}

std::uint32_t MessageReader::read_uint32() {
    if (auto aligned = align(4); !aligned) {
        return 0;
    }
    if (!can_read(sizeof(std::uint32_t))) {
        fail(WireError::Truncated);
        return 0;
    }
    return read_u32_unchecked();
}

std::int64_t MessageReader::read_int64() {
    const std::uint64_t raw = read_uint64();
    std::int64_t value = 0;
    std::memcpy(&value, &raw, sizeof(value));
    return value;
}

std::uint64_t MessageReader::read_uint64() {
    if (auto aligned = align(8); !aligned) {
        return 0;
    }
    if (!can_read(sizeof(std::uint64_t))) {
        fail(WireError::Truncated);
        return 0;
    }
    return read_u64_unchecked();
}

double MessageReader::read_double() {
    const std::uint64_t raw = read_uint64();
    double value = 0.0;
    std::memcpy(&value, &raw, sizeof(value));
    return value;
}

std::string_view MessageReader::read_string() {
    if (auto aligned = align(4); !aligned) {
        return {};
    }
    if (!can_read(sizeof(std::uint32_t))) {
        fail(WireError::Truncated);
        return {};
    }
    const std::uint32_t length = read_u32_unchecked();
    if (length > kMaxWireText) {
        fail(WireError::TooLarge);
        return {};
    }
    if (!can_read(static_cast<std::size_t>(length) + 1)) {
        fail(WireError::Truncated);
        return {};
    }
    if (bytes_[offset_ + length] != 0) {
        fail(WireError::BadTerminator);
        return {};
    }
    const auto* const text = reinterpret_cast<const char*>(bytes_.data() + offset_);
    offset_ += static_cast<std::size_t>(length) + 1;
    return std::string_view(text, length);
}

std::string_view MessageReader::read_object_path() {
    const std::string_view value = read_string();
    if (error_ != WireError::None) {
        return {};
    }
    if (value.empty() || value.front() != '/') {
        fail(WireError::ValueOutOfRange);
        return {};
    }
    return value;
}

std::string_view MessageReader::read_signature() {
    const std::uint8_t length = read_byte();
    if (error_ != WireError::None) {
        return {};
    }
    if (static_cast<std::size_t>(length) > kMaxWireText) {
        fail(WireError::TooLarge);
        return {};
    }
    if (!can_read(static_cast<std::size_t>(length) + 1)) {
        fail(WireError::Truncated);
        return {};
    }
    if (bytes_[offset_ + length] != 0) {
        fail(WireError::BadTerminator);
        return {};
    }
    const auto* const text = reinterpret_cast<const char*>(bytes_.data() + offset_);
    offset_ += static_cast<std::size_t>(length) + 1;
    return std::string_view(text, length);
}

std::expected<void, WireError> MessageReader::read_array_header(char element_code) {
    const std::size_t alignment = alignment_of_code(element_code);
    if (alignment == 0) {
        fail(WireError::BadType);
        return std::unexpected(error_);
    }
    if (depth_ + 1 >= kMaxWireDepth) {
        fail(WireError::Unbalanced);
        return std::unexpected(error_);
    }
    if (auto aligned = align(4); !aligned) {
        return aligned;
    }
    if (!can_read(sizeof(std::uint32_t))) {
        fail(WireError::Truncated);
        return std::unexpected(error_);
    }
    const std::uint32_t length = read_u32_unchecked();
    if (length > kMaxWireBody) {
        fail(WireError::TooLarge);
        return std::unexpected(error_);
    }
    if (auto aligned = align(alignment); !aligned) {
        return aligned;
    }
    if (!can_read(length)) {
        fail(WireError::Truncated);
        return std::unexpected(error_);
    }
    push_limit(offset_ + length);
    if (error_ != WireError::None) {
        return std::unexpected(error_);
    }
    return {};
}

bool MessageReader::array_done() const {
    return depth_ > 0 && offset_ >= limit();
}

std::expected<void, WireError> MessageReader::finish_array() {
    if (depth_ == 0) {
        fail(WireError::Unbalanced);
        return std::unexpected(error_);
    }
    if (offset_ != limit()) {
        fail(WireError::BadAlignment);
        return std::unexpected(error_);
    }
    pop_limit();
    return {};
}

std::expected<void, WireError> MessageReader::begin_struct() {
    return align(8);
}

void MessageReader::end_struct() {
    // Nothing to pop: a struct has no length prefix.
}

std::expected<std::string_view, WireError> MessageReader::read_variant_signature() {
    const std::string_view signature = read_signature();
    if (error_ != WireError::None) {
        return std::unexpected(error_);
    }
    if (!single_complete_type(signature)) {
        fail(WireError::BadSignature);
        return std::unexpected(error_);
    }
    return signature;
}

std::expected<void, WireError> MessageReader::skip_value(std::string_view signature) {
    if (!single_complete_type(signature)) {
        fail(WireError::BadSignature);
        return std::unexpected(error_);
    }
    return skip_single(*this, signature, 0);
}

} // namespace tessera::ui::a11y
