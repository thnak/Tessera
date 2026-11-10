// SPDX-License-Identifier: MIT

#include "tessera/ui/accessibility/dbus_wire.hpp"

#include <bit>
#include <cstring>

namespace tessera::ui::a11y {
namespace {

struct TypeRow {
    WireType type;
    char code;
    std::size_t alignment;
};

// D-Bus type table: the alignment column is the normative one from the protocol specification.
constexpr TypeRow kTypes[] = {
    {WireType::Byte, 'y', 1},    {WireType::Boolean, 'b', 4},    {WireType::Int16, 'n', 2},
    {WireType::UInt16, 'q', 2},  {WireType::Int32, 'i', 4},      {WireType::UInt32, 'u', 4},
    {WireType::Int64, 'x', 8},   {WireType::UInt64, 't', 8},     {WireType::Double, 'd', 8},
    {WireType::String, 's', 4},  {WireType::ObjectPath, 'o', 4}, {WireType::Signature, 'g', 1},
    {WireType::Array, 'a', 4},   {WireType::Struct, '(', 8},     {WireType::DictEntry, '{', 8},
    {WireType::Variant, 'v', 1}, {WireType::UnixFd, 'h', 4},
};

bool is_basic_code(char code) {
    switch (code) {
    case 'y':
    case 'b':
    case 'n':
    case 'q':
    case 'i':
    case 'u':
    case 'x':
    case 't':
    case 'd':
    case 's':
    case 'o':
    case 'g':
    case 'h':
        return true;
    default:
        return false;
    }
}

// Recursion is bounded by the signature length, which the protocol caps at 255 bytes; the explicit
// counter keeps pathological nesting away from the stack limit.
std::size_t complete_type_length_at(std::string_view signature, std::size_t depth) {
    if (signature.empty() || depth > kMaxWireDepth * 4) {
        return 0;
    }
    switch (signature.front()) {
    case 'y':
    case 'b':
    case 'n':
    case 'q':
    case 'i':
    case 'u':
    case 'x':
    case 't':
    case 'd':
    case 's':
    case 'o':
    case 'g':
    case 'v':
    case 'h':
        return 1;
    case 'a': {
        const std::size_t inner = complete_type_length_at(signature.substr(1), depth + 1);
        return inner == 0 ? 0 : 1 + inner;
    }
    case '(': {
        std::size_t index = 1;
        std::size_t members = 0;
        while (index < signature.size() && signature[index] != ')') {
            const std::size_t member = complete_type_length_at(signature.substr(index), depth + 1);
            if (member == 0) {
                return 0;
            }
            index += member;
            ++members;
        }
        if (index >= signature.size() || signature[index] != ')' || members == 0) {
            return 0;
        }
        return index + 1;
    }
    case '{': {
        if (signature.size() < 2 || !is_basic_code(signature[1])) {
            return 0;
        }
        const std::size_t value = complete_type_length_at(signature.substr(2), depth + 1);
        if (value == 0) {
            return 0;
        }
        const std::size_t closing = 2 + value;
        if (closing >= signature.size() || signature[closing] != '}') {
            return 0;
        }
        return closing + 1;
    }
    default:
        return 0;
    }
}

} // namespace

std::size_t alignment_of(WireType type) {
    for (const TypeRow& row : kTypes) {
        if (row.type == type) {
            return row.alignment;
        }
    }
    return 0;
}

std::size_t alignment_of_code(char code) {
    for (const TypeRow& row : kTypes) {
        if (row.code == code) {
            return row.alignment;
        }
    }
    return 0;
}

char code_of(WireType type) {
    for (const TypeRow& row : kTypes) {
        if (row.type == type) {
            return row.code;
        }
    }
    return '\0';
}

WireType type_from_code(char code) {
    for (const TypeRow& row : kTypes) {
        if (row.code == code) {
            return row.type;
        }
    }
    return WireType::Invalid;
}

std::size_t complete_type_length(std::string_view signature) {
    return complete_type_length_at(signature, 0);
}

bool valid_signature(std::string_view signature) {
    std::size_t index = 0;
    while (index < signature.size()) {
        const std::size_t length = complete_type_length_at(signature.substr(index), 0);
        if (length == 0) {
            return false;
        }
        index += length;
    }
    return true;
}

bool single_complete_type(std::string_view signature) {
    return !signature.empty() && complete_type_length_at(signature, 0) == signature.size();
}

ByteOrder host_byte_order() {
    return std::endian::native == std::endian::little ? ByteOrder::Little : ByteOrder::Big;
}

std::string_view to_string(WireError error) {
    switch (error) {
    case WireError::None:
        return "none";
    case WireError::Truncated:
        return "truncated";
    case WireError::BadType:
        return "bad type";
    case WireError::BadSignature:
        return "bad signature";
    case WireError::BadAlignment:
        return "bad alignment";
    case WireError::BadEndian:
        return "bad endianness";
    case WireError::BadVersion:
        return "bad protocol version";
    case WireError::TooLarge:
        return "too large";
    case WireError::ValueOutOfRange:
        return "value out of range";
    case WireError::MissingField:
        return "missing header field";
    case WireError::Unbalanced:
        return "unbalanced container";
    case WireError::BadTerminator:
        return "bad string terminator";
    }
    return "unknown";
}

MessageWriter::MessageWriter() {
    bytes_.reserve(512);
}

void MessageWriter::reset() {
    bytes_.clear();
    arrays_.clear();
    body_length_offset_ = 0;
    fields_length_offset_ = 0;
    fields_start_ = 0;
    body_start_ = 0;
    message_open_ = false;
    error_ = WireError::None;
}

void MessageWriter::fail(WireError error) {
    if (error_ == WireError::None) {
        error_ = error;
    }
}

void MessageWriter::align(std::size_t alignment) {
    if (error_ != WireError::None || alignment <= 1) {
        return;
    }
    const std::size_t remainder = bytes_.size() % alignment;
    if (remainder == 0) {
        return;
    }
    const std::size_t padding = alignment - remainder;
    if (bytes_.size() + padding > kMaxWireMessage) {
        fail(WireError::TooLarge);
        return;
    }
    bytes_.insert(bytes_.end(), padding, 0);
}

void MessageWriter::reserve(std::size_t size) {
    if (bytes_.size() + size > kMaxWireMessage) {
        fail(WireError::TooLarge);
    }
}

void MessageWriter::write_raw(const void* data, std::size_t size) {
    if (error_ != WireError::None || size == 0) {
        return;
    }
    reserve(size);
    if (error_ != WireError::None) {
        return;
    }
    const auto* const first = static_cast<const std::uint8_t*>(data);
    bytes_.insert(bytes_.end(), first, first + size);
}

void MessageWriter::patch_u32(std::size_t offset, std::uint32_t value) {
    if (error_ != WireError::None || offset + sizeof(value) > bytes_.size()) {
        fail(WireError::BadAlignment);
        return;
    }
    std::memcpy(bytes_.data() + offset, &value, sizeof(value));
}

void MessageWriter::write_byte(std::uint8_t value) {
    write_raw(&value, sizeof(value));
}

void MessageWriter::write_bool(bool value) {
    align(4);
    const std::uint32_t encoded = value ? 1U : 0U;
    write_raw(&encoded, sizeof(encoded));
}

void MessageWriter::write_int16(std::int16_t value) {
    align(2);
    write_raw(&value, sizeof(value));
}

void MessageWriter::write_uint16(std::uint16_t value) {
    align(2);
    write_raw(&value, sizeof(value));
}

void MessageWriter::write_int32(std::int32_t value) {
    align(4);
    write_raw(&value, sizeof(value));
}

void MessageWriter::write_uint32(std::uint32_t value) {
    align(4);
    write_raw(&value, sizeof(value));
}

void MessageWriter::write_int64(std::int64_t value) {
    align(8);
    write_raw(&value, sizeof(value));
}

void MessageWriter::write_uint64(std::uint64_t value) {
    align(8);
    write_raw(&value, sizeof(value));
}

void MessageWriter::write_double(double value) {
    align(8);
    write_raw(&value, sizeof(value));
}

void MessageWriter::write_string_like(std::string_view value, std::size_t length_alignment) {
    if (value.size() > kMaxWireText) {
        fail(WireError::TooLarge);
        return;
    }
    align(length_alignment);
    const auto length = static_cast<std::uint32_t>(value.size());
    write_raw(&length, sizeof(length));
    write_raw(value.data(), value.size());
    const std::uint8_t terminator = 0;
    write_raw(&terminator, 1);
}

void MessageWriter::write_string(std::string_view value) {
    write_string_like(value, 4);
}

void MessageWriter::write_object_path(std::string_view value) {
    if (value.empty() || value.front() != '/') {
        fail(WireError::ValueOutOfRange);
        return;
    }
    write_string_like(value, 4);
}

void MessageWriter::write_signature(std::string_view value) {
    if (!valid_signature(value) || value.size() > 255) {
        fail(WireError::BadSignature);
        return;
    }
    write_byte(static_cast<std::uint8_t>(value.size()));
    write_raw(value.data(), value.size());
    const std::uint8_t terminator = 0;
    write_raw(&terminator, 1);
}

void MessageWriter::begin_array(char element_code) {
    if (alignment_of_code(element_code) == 0) {
        fail(WireError::BadType);
        return;
    }
    if (arrays_.size() >= kMaxWireDepth) {
        fail(WireError::Unbalanced);
        return;
    }
    align(4);
    const std::size_t length_offset = bytes_.size();
    const std::uint32_t placeholder = 0;
    write_raw(&placeholder, sizeof(placeholder));
    align(alignment_of_code(element_code));
    if (error_ != WireError::None) {
        return;
    }
    arrays_.push_back(ArrayFrame{length_offset, bytes_.size(), element_code});
}

void MessageWriter::end_array() {
    if (arrays_.empty()) {
        fail(WireError::Unbalanced);
        return;
    }
    const ArrayFrame frame = arrays_.back();
    arrays_.pop_back();
    patch_u32(frame.length_offset, static_cast<std::uint32_t>(bytes_.size() - frame.start));
}

void MessageWriter::begin_struct() {
    align(8);
}

void MessageWriter::end_struct() {
    // A struct has no length prefix and no trailing padding requirement.
}

void MessageWriter::write_header_field(HeaderField field, std::string_view signature,
                                       const std::string& value) {
    align(8);
    write_byte(static_cast<std::uint8_t>(field));
    write_signature(signature);
    // The variant's declared signature decides the value encoding: a signature value is a byte
    // length, not the 32-bit length a string uses.
    if (signature == "o") {
        write_object_path(value);
    } else if (signature == "g") {
        write_signature(value);
    } else {
        write_string(value);
    }
}

void MessageWriter::write_header_field_u32(HeaderField field, std::uint32_t value) {
    align(8);
    write_byte(static_cast<std::uint8_t>(field));
    write_signature("u");
    write_uint32(value);
}

std::expected<void, WireError> MessageWriter::begin_message(const MessageHeader& header) {
    reset();
    if (header.serial == 0) {
        fail(WireError::ValueOutOfRange);
        return std::unexpected(error_);
    }
    write_byte(host_byte_order() == ByteOrder::Little ? 'l' : 'B');
    write_byte(static_cast<std::uint8_t>(header.type));
    write_byte(header.flags);
    write_byte(1);
    body_length_offset_ = bytes_.size();
    const std::uint32_t placeholder = 0;
    write_raw(&placeholder, sizeof(placeholder));
    write_uint32(header.serial);
    fields_length_offset_ = bytes_.size();
    write_raw(&placeholder, sizeof(placeholder));
    fields_start_ = bytes_.size();

    if (!header.path.empty()) {
        write_header_field(HeaderField::Path, "o", header.path);
    }
    if (!header.interface.empty()) {
        write_header_field(HeaderField::Interface, "s", header.interface);
    }
    if (!header.member.empty()) {
        write_header_field(HeaderField::Member, "s", header.member);
    }
    if (!header.error_name.empty()) {
        write_header_field(HeaderField::ErrorName, "s", header.error_name);
    }
    if (header.reply_serial != 0) {
        write_header_field_u32(HeaderField::ReplySerial, header.reply_serial);
    }
    if (!header.destination.empty()) {
        write_header_field(HeaderField::Destination, "s", header.destination);
    }
    if (!header.sender.empty()) {
        write_header_field(HeaderField::Sender, "s", header.sender);
    }
    if (!header.signature.empty()) {
        write_header_field(HeaderField::Signature, "g", header.signature);
    }
    if (error_ != WireError::None) {
        return std::unexpected(error_);
    }
    patch_u32(fields_length_offset_, static_cast<std::uint32_t>(bytes_.size() - fields_start_));
    align(8);
    body_start_ = bytes_.size();
    message_open_ = true;
    return {};
}

std::expected<void, WireError> MessageWriter::end_message() {
    if (!message_open_) {
        fail(WireError::BadType);
        return std::unexpected(error_);
    }
    message_open_ = false;
    if (error_ == WireError::None && !arrays_.empty()) {
        fail(WireError::Unbalanced);
    }
    const std::size_t body_length = bytes_.size() - body_start_;
    if (error_ == WireError::None && body_length > kMaxWireBody) {
        fail(WireError::TooLarge);
    }
    if (error_ != WireError::None) {
        return std::unexpected(error_);
    }
    patch_u32(body_length_offset_, static_cast<std::uint32_t>(body_length));
    return {};
}

} // namespace tessera::ui::a11y
