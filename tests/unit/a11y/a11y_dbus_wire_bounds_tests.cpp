// SPDX-License-Identifier: MIT
//
// Wire codec rejections: truncation, bad header bytes, malformed strings, out-of-range values,
// container/framing mismatches, the nesting cap, and writer overflow.

#include "a11y_dbus_test_support.hpp"
#include "tessera/ui/accessibility/dbus_wire.hpp"

#include <string>
#include <vector>

namespace {

using namespace a11y_dbus_test;
using tessera::ui::a11y::kMaxWireDepth;
using tessera::ui::a11y::kMaxWireText;
using tessera::ui::a11y::MessageHeader;
using tessera::ui::a11y::MessageReader;
using tessera::ui::a11y::MessageType;
using tessera::ui::a11y::MessageWriter;
using tessera::ui::a11y::parse_message;
using tessera::ui::a11y::WireError;

void patch_u32(std::vector<std::uint8_t>& bytes, std::size_t offset, std::uint32_t value) {
    bytes[offset] = static_cast<std::uint8_t>(value & 0xFFU);
    bytes[offset + 1] = static_cast<std::uint8_t>((value >> 8) & 0xFFU);
    bytes[offset + 2] = static_cast<std::uint8_t>((value >> 16) & 0xFFU);
    bytes[offset + 3] = static_cast<std::uint8_t>((value >> 24) & 0xFFU);
}

std::vector<std::uint8_t> golden_method_call() {
    return from_hex("6C010001"
                    "00000000"
                    "01000000"
                    "1A000000"
                    "01016F00"
                    "02000000"
                    "2F6100"
                    "0000000000"
                    "03017300"
                    "01000000"
                    "4D00"
                    "000000000000");
}

bool truncated_messages() {
    const std::vector<std::uint8_t> empty;
    if (!expect(parse_message(empty).error() == WireError::Truncated, "empty buffer")) {
        return false;
    }
    const std::vector<std::uint8_t> short_header(15, 0);
    if (!expect(parse_message(short_header).error() == WireError::Truncated, "short header")) {
        return false;
    }
    std::vector<std::uint8_t> fields_beyond = golden_method_call();
    patch_u32(fields_beyond, 12, 0x100);
    if (!expect(parse_message(fields_beyond).error() == WireError::Truncated,
                "fields beyond the buffer")) {
        return false;
    }
    std::vector<std::uint8_t> fields_too_large = golden_method_call();
    patch_u32(fields_too_large, 12, 0xFFFF);
    if (!expect(parse_message(fields_too_large).error() == WireError::TooLarge,
                "fields array cap")) {
        return false;
    }
    std::vector<std::uint8_t> body_beyond = golden_method_call();
    patch_u32(body_beyond, 4, 4);
    return expect(parse_message(body_beyond).error() == WireError::Truncated,
                  "body beyond the buffer");
}

bool bad_header_bytes() {
    std::vector<std::uint8_t> endian = golden_method_call();
    endian[0] = 'X';
    if (!expect(parse_message(endian).error() == WireError::BadEndian, "bad endianness")) {
        return false;
    }
    std::vector<std::uint8_t> version = golden_method_call();
    version[3] = 2;
    if (!expect(parse_message(version).error() == WireError::BadVersion, "bad version")) {
        return false;
    }
    std::vector<std::uint8_t> type = golden_method_call();
    type[1] = 9;
    if (!expect(parse_message(type).error() == WireError::BadType, "bad message type")) {
        return false;
    }
    return true;
}

bool header_field_validation() {
    // A path field that claims to be a string variant.
    std::vector<std::uint8_t> wrong_variant = golden_method_call();
    wrong_variant[18] = 's';
    if (!expect(parse_message(wrong_variant).error() == WireError::BadSignature,
                "field variant must match the field type")) {
        return false;
    }
    // Member (code 3) renamed to error name (code 4): a method call then has no member.
    std::vector<std::uint8_t> missing_member = golden_method_call();
    missing_member[32] = 4;
    if (!expect(parse_message(missing_member).error() == WireError::MissingField,
                "method call needs a member")) {
        return false;
    }
    // A body without a signature cannot be decoded.
    MessageWriter writer;
    MessageHeader header;
    header.type = MessageType::MethodReturn;
    header.serial = 2;
    header.reply_serial = 1;
    (void)writer.begin_message(header);
    writer.write_byte(0);
    (void)writer.end_message();
    const std::vector<std::uint8_t> unsigned_body(writer.bytes().begin(), writer.bytes().end());
    if (!expect(parse_message(unsigned_body).error() == WireError::BadSignature,
                "a body needs a signature")) {
        return false;
    }
    // Return and error messages must reply to a serial; signals need an interface.
    MessageWriter reply;
    MessageHeader reply_header;
    reply_header.type = MessageType::MethodReturn;
    reply_header.serial = 3;
    (void)reply.begin_message(reply_header);
    (void)reply.end_message();
    const std::vector<std::uint8_t> no_reply_serial(reply.bytes().begin(), reply.bytes().end());
    if (!expect(parse_message(no_reply_serial).error() == WireError::MissingField,
                "return needs a reply serial")) {
        return false;
    }
    MessageWriter signal;
    MessageHeader signal_header;
    signal_header.type = MessageType::Signal;
    signal_header.serial = 4;
    signal_header.path = "/a";
    signal_header.member = "M";
    (void)signal.begin_message(signal_header);
    (void)signal.end_message();
    const std::vector<std::uint8_t> no_interface(signal.bytes().begin(), signal.bytes().end());
    return expect(parse_message(no_interface).error() == WireError::MissingField,
                  "signal needs an interface");
}

bool string_errors() {
    {
        const std::vector<std::uint8_t> missing_nul = from_hex("010000006162");
        MessageReader reader(missing_nul);
        const std::string_view value = reader.read_string();
        if (!expect(value.empty(), "terminated string is rejected") ||
            !expect(reader.error() == WireError::BadTerminator, "missing NUL")) {
            return false;
        }
    }
    {
        const std::vector<std::uint8_t> oversized = from_hex("01020000");
        MessageReader reader(oversized);
        (void)reader.read_string();
        if (!expect(reader.error() == WireError::TooLarge, "string cap")) {
            return false;
        }
    }
    {
        const std::vector<std::uint8_t> bad_path = from_hex("010000007800");
        MessageReader reader(bad_path);
        (void)reader.read_object_path();
        if (!expect(reader.error() == WireError::ValueOutOfRange, "object path needs a slash")) {
            return false;
        }
    }
    {
        const std::vector<std::uint8_t> bad_bool = from_hex("02000000");
        MessageReader reader(bad_bool);
        (void)reader.read_bool();
        if (!expect(reader.error() == WireError::ValueOutOfRange, "boolean range")) {
            return false;
        }
    }
    {
        std::vector<std::uint8_t> unpadded_string = from_hex("01020000");
        unpadded_string.insert(unpadded_string.end(), kMaxWireText + 1, 'a');
        MessageReader reader(unpadded_string);
        (void)reader.read_string();
        return expect(reader.error() == WireError::TooLarge, "bounded string length");
    }
}

bool array_framing() {
    {
        // Array of 5 bytes, but the caller consumes 4: the declared length must match exactly.
        const std::vector<std::uint8_t> bytes = from_hex("050000006162636465");
        MessageReader reader(bytes);
        if (!expect(reader.read_array_header('y').has_value(), "byte array header")) {
            return false;
        }
        for (int index = 0; index < 4; ++index) {
            (void)reader.read_byte();
        }
        if (!expect(reader.finish_array().error() == WireError::BadAlignment,
                    "array length must match the elements")) {
            return false;
        }
    }
    {
        // Declared length beyond the buffer.
        const std::vector<std::uint8_t> bytes = from_hex("0A00000061");
        MessageReader reader(bytes);
        if (!expect(reader.read_array_header('y').error() == WireError::Truncated,
                    "array length beyond the buffer")) {
            return false;
        }
    }
    {
        // Unknown element type.
        const std::vector<std::uint8_t> bytes = from_hex("00000000");
        MessageReader reader(bytes);
        if (!expect(reader.read_array_header('?').error() == WireError::BadType,
                    "unknown element type")) {
            return false;
        }
    }
    {
        // finish_array without an array open.
        const std::vector<std::uint8_t> bytes = from_hex("00000000");
        MessageReader reader(bytes);
        return expect(reader.finish_array().error() == WireError::Unbalanced,
                      "finish without an array");
    }
}

bool nesting_limit() {
    // Innermost array of one byte, then wrap it kMaxWireDepth + 2 times in arrays of arrays.
    std::vector<std::uint8_t> bytes = from_hex("010000007A");
    for (std::size_t level = 0; level < kMaxWireDepth + 2; ++level) {
        std::vector<std::uint8_t> outer;
        const auto length = static_cast<std::uint32_t>(bytes.size());
        outer.push_back(static_cast<std::uint8_t>(length & 0xFFU));
        outer.push_back(static_cast<std::uint8_t>((length >> 8) & 0xFFU));
        outer.push_back(static_cast<std::uint8_t>((length >> 16) & 0xFFU));
        outer.push_back(static_cast<std::uint8_t>((length >> 24) & 0xFFU));
        outer.insert(outer.end(), bytes.begin(), bytes.end());
        bytes = std::move(outer);
    }
    MessageReader reader(bytes);
    bool failed = false;
    for (std::size_t level = 0; level < kMaxWireDepth + 2; ++level) {
        const auto header = reader.read_array_header('a');
        if (!header) {
            failed = header.error() == WireError::Unbalanced;
            break;
        }
    }
    return expect(failed, "nesting beyond the cap is rejected") &&
           expect(reader.failed(), "nesting failure is sticky");
}

bool writer_failures() {
    {
        MessageWriter writer;
        MessageHeader header;
        header.serial = 0;
        if (!expect(writer.begin_message(header).error() == WireError::ValueOutOfRange,
                    "serial must be non-zero")) {
            return false;
        }
    }
    {
        MessageWriter writer;
        MessageHeader header;
        header.serial = 1;
        header.path = "no-slash";
        if (!expect(writer.begin_message(header).error() == WireError::ValueOutOfRange,
                    "object path must start with a slash")) {
            return false;
        }
    }
    {
        MessageWriter writer;
        MessageHeader header;
        header.serial = 1;
        header.path = "/a";
        header.member = "M";
        (void)writer.begin_message(header);
        writer.write_string(std::string(kMaxWireText + 1, 'x'));
        if (!expect(writer.end_message().error() == WireError::TooLarge, "oversized body string")) {
            return false;
        }
    }
    {
        MessageWriter writer;
        MessageHeader header;
        header.serial = 1;
        header.path = "/a";
        header.member = "M";
        (void)writer.begin_message(header);
        writer.end_array();
        if (!expect(writer.end_message().error() == WireError::Unbalanced,
                    "end_array without begin_array")) {
            return false;
        }
    }
    {
        MessageWriter writer;
        MessageHeader header;
        header.serial = 1;
        header.path = "/a";
        header.member = "M";
        (void)writer.begin_message(header);
        writer.begin_array('y');
        if (!expect(writer.end_message().error() == WireError::Unbalanced, "unterminated array")) {
            return false;
        }
    }
    {
        // A variant signature must be exactly one complete type; a malformed one fails the message.
        MessageWriter writer;
        MessageHeader header;
        header.serial = 1;
        header.path = "/a";
        header.member = "M";
        (void)writer.begin_message(header);
        writer.write_variant("s(", [](MessageWriter&) {});
        return expect(writer.error() == WireError::BadSignature, "invalid variant signature") &&
               expect(writer.end_message().error() == WireError::BadSignature,
                      "invalid variant fails the message");
    }
}

} // namespace

int main() {
    if (!truncated_messages()) {
        return 1;
    }
    if (!bad_header_bytes()) {
        return 2;
    }
    if (!header_field_validation()) {
        return 3;
    }
    if (!string_errors()) {
        return 4;
    }
    if (!array_framing()) {
        return 5;
    }
    if (!nesting_limit()) {
        return 6;
    }
    if (!writer_failures()) {
        return 7;
    }
    std::cout << "a11y_dbus_wire_bounds: ok\n";
    return 0;
}
