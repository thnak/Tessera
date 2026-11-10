// SPDX-License-Identifier: MIT

#include "tessera/ui/accessibility/dbus_wire.hpp"

namespace tessera::ui::a11y {
namespace {

char type_code_of(std::string_view signature) {
    return signature.empty() ? '\0' : signature.front();
}

} // namespace

std::expected<ParsedMessage, WireError> parse_message(std::span<const std::uint8_t> bytes) {
    constexpr std::size_t kFixedHeader = 16;
    if (bytes.size() < kFixedHeader) {
        return std::unexpected(WireError::Truncated);
    }
    if (bytes.size() > kMaxWireMessage) {
        return std::unexpected(WireError::TooLarge);
    }
    ByteOrder order = ByteOrder::Little;
    if (bytes[0] == 'l') {
        order = ByteOrder::Little;
    } else if (bytes[0] == 'B') {
        order = ByteOrder::Big;
    } else {
        return std::unexpected(WireError::BadEndian);
    }
    if (bytes[3] != 1) {
        return std::unexpected(WireError::BadVersion);
    }
    if (bytes[1] < static_cast<std::uint8_t>(MessageType::MethodCall) ||
        bytes[1] > static_cast<std::uint8_t>(MessageType::Signal)) {
        return std::unexpected(WireError::BadType);
    }

    MessageReader reader(bytes, order);
    reader.seek(4);
    const std::uint32_t body_length = reader.read_uint32();
    const std::uint32_t serial = reader.read_uint32();
    const std::uint32_t fields_length = reader.read_uint32();
    if (reader.failed()) {
        return std::unexpected(reader.error());
    }
    if (fields_length > kMaxWireFieldBytes) {
        return std::unexpected(WireError::TooLarge);
    }
    if (body_length > kMaxWireBody) {
        return std::unexpected(WireError::TooLarge);
    }
    const std::size_t fields_end = kFixedHeader + fields_length;
    if (fields_end > bytes.size()) {
        return std::unexpected(WireError::Truncated);
    }

    ParsedMessage message;
    message.header.type = static_cast<MessageType>(bytes[1]);
    message.header.flags = bytes[2];
    message.header.serial = serial;

    reader.seek(kFixedHeader);
    reader.push_limit(fields_end);
    while (!reader.exhausted()) {
        const std::size_t element_start = reader.offset();
        if (auto aligned = reader.align(8); !aligned) {
            return std::unexpected(aligned.error());
        }
        if (reader.exhausted()) {
            // Some senders fold the padding after the last field into the array length. That is
            // harmless when it is short and zero, and rejecting it would break interop for no gain;
            // anything else is real corruption.
            if (fields_end - element_start > 8) {
                return std::unexpected(WireError::BadAlignment);
            }
            for (std::size_t index = element_start; index < fields_end; ++index) {
                if (bytes[index] != 0) {
                    return std::unexpected(WireError::BadAlignment);
                }
            }
            break;
        }
        const std::uint8_t code = reader.read_byte();
        const auto signature = reader.read_variant_signature();
        if (!signature) {
            return std::unexpected(signature.error());
        }
        switch (static_cast<HeaderField>(code)) {
        case HeaderField::Path:
            if (*signature != "o") {
                return std::unexpected(WireError::BadSignature);
            }
            message.header.path = std::string(reader.read_object_path());
            break;
        case HeaderField::Interface:
            if (*signature != "s") {
                return std::unexpected(WireError::BadSignature);
            }
            message.header.interface = std::string(reader.read_string());
            break;
        case HeaderField::Member:
            if (*signature != "s") {
                return std::unexpected(WireError::BadSignature);
            }
            message.header.member = std::string(reader.read_string());
            break;
        case HeaderField::ErrorName:
            if (*signature != "s") {
                return std::unexpected(WireError::BadSignature);
            }
            message.header.error_name = std::string(reader.read_string());
            break;
        case HeaderField::ReplySerial:
            if (*signature != "u") {
                return std::unexpected(WireError::BadSignature);
            }
            message.header.reply_serial = reader.read_uint32();
            break;
        case HeaderField::Destination:
            if (*signature != "s") {
                return std::unexpected(WireError::BadSignature);
            }
            message.header.destination = std::string(reader.read_string());
            break;
        case HeaderField::Sender:
            if (*signature != "s") {
                return std::unexpected(WireError::BadSignature);
            }
            message.header.sender = std::string(reader.read_string());
            break;
        case HeaderField::Signature:
            if (*signature != "g") {
                return std::unexpected(WireError::BadSignature);
            }
            message.header.signature = std::string(reader.read_signature());
            break;
        default:
            if (auto skipped = reader.skip_value(*signature); !skipped) {
                return std::unexpected(skipped.error());
            }
            break;
        }
        if (reader.failed()) {
            return std::unexpected(reader.error());
        }
    }
    reader.pop_limit();
    if (auto aligned = reader.align(8); !aligned) {
        return std::unexpected(aligned.error());
    }
    const std::size_t body_start = reader.offset();
    if (body_start > bytes.size() || body_length > bytes.size() - body_start) {
        return std::unexpected(WireError::Truncated);
    }
    if (!message.header.signature.empty() && !valid_signature(message.header.signature)) {
        return std::unexpected(WireError::BadSignature);
    }
    if (!type_code_of(message.header.signature) && body_length != 0) {
        // A body without a signature cannot be decoded; reject rather than silently ignore bytes.
        return std::unexpected(WireError::BadSignature);
    }

    switch (message.header.type) {
    case MessageType::MethodCall:
        if (message.header.path.empty() || message.header.member.empty()) {
            return std::unexpected(WireError::MissingField);
        }
        break;
    case MessageType::MethodReturn:
        if (message.header.reply_serial == 0) {
            return std::unexpected(WireError::MissingField);
        }
        break;
    case MessageType::Error:
        if (message.header.reply_serial == 0 || message.header.error_name.empty()) {
            return std::unexpected(WireError::MissingField);
        }
        break;
    case MessageType::Signal:
        if (message.header.path.empty() || message.header.interface.empty() ||
            message.header.member.empty()) {
            return std::unexpected(WireError::MissingField);
        }
        break;
    }

    message.body = bytes.subspan(body_start, body_length);
    message.consumed = body_start + body_length;
    return message;
}
} // namespace tessera::ui::a11y
