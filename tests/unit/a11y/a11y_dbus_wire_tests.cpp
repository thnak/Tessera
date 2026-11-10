// SPDX-License-Identifier: MIT
//
// Wire codec: signature validation, golden message bytes, round-tripped bodies, big-endian input,
// and stream framing.

#include "a11y_dbus_test_support.hpp"
#include "tessera/ui/accessibility/dbus_wire.hpp"

#include <string>
#include <vector>

namespace {

using namespace a11y_dbus_test;
using tessera::ui::a11y::alignment_of;
using tessera::ui::a11y::alignment_of_code;
using tessera::ui::a11y::ByteOrder;
using tessera::ui::a11y::code_of;
using tessera::ui::a11y::complete_type_length;
using tessera::ui::a11y::MessageHeader;
using tessera::ui::a11y::MessageReader;
using tessera::ui::a11y::MessageType;
using tessera::ui::a11y::MessageWriter;
using tessera::ui::a11y::parse_message;
using tessera::ui::a11y::single_complete_type;
using tessera::ui::a11y::type_from_code;
using tessera::ui::a11y::valid_signature;
using tessera::ui::a11y::WireType;

bool type_tables() {
    return expect(alignment_of(WireType::Byte) == 1, "byte alignment") &&
           expect(alignment_of(WireType::UInt32) == 4, "uint32 alignment") &&
           expect(alignment_of(WireType::UInt64) == 8, "uint64 alignment") &&
           expect(alignment_of(WireType::Struct) == 8, "struct alignment") &&
           expect(alignment_of_code('d') == 8, "double alignment by code") &&
           expect(alignment_of_code('?') == 0, "unknown code has no alignment") &&
           expect(code_of(WireType::ObjectPath) == 'o', "object path code") &&
           expect(type_from_code('a') == WireType::Array, "array code") &&
           expect(type_from_code('?') == WireType::Invalid, "unknown code is invalid");
}

bool signature_validation() {
    return expect(valid_signature(""), "empty signature is valid") &&
           expect(valid_signature("s"), "single basic type") &&
           expect(valid_signature("siu"), "several complete types") &&
           expect(valid_signature("a{sv}"), "dict array") &&
           expect(valid_signature("a(so)"), "struct array") &&
           expect(valid_signature("(sss)"), "three member struct") &&
           expect(valid_signature("aay"), "nested arrays") &&
           expect(!valid_signature("a"), "array without an element type") &&
           expect(!valid_signature("("), "unclosed struct") &&
           expect(!valid_signature("(s"), "struct missing close") &&
           expect(!valid_signature("()"), "empty struct") &&
           expect(!valid_signature("{}"), "empty dict entry") &&
           expect(!valid_signature("a{vs}"), "variant cannot be a dict key") &&
           expect(!valid_signature("q?"), "unknown code") &&
           expect(single_complete_type("(so)"), "single struct is one complete type") &&
           expect(single_complete_type("a{sv}"), "single dict array is one complete type") &&
           expect(!single_complete_type("ss"), "two types are not one") &&
           expect(complete_type_length("(so)u") == 4, "first complete type length") &&
           expect(complete_type_length("a{sv}") == 5, "dict array length") &&
           expect(complete_type_length("x") == 1, "basic length") &&
           expect(complete_type_length("Q") == 0, "invalid length");
}

// Golden bytes for the smallest useful call: method call, path "/a", member "M", serial 1.
//   6C 01 00 01 | 00 00 00 00 | 01 00 00 00 | 1A 00 00 00   fixed header, 26 field bytes
//   01 01 6F 00 | 02 00 00 00 | 2F 61 00 | 00 00 00 00 00   path "/a" + padding to 8
//   03 01 73 00 | 01 00 00 00 | 4D 00    | 00 00 00 00 00 00   member "M" + padding to 8
bool golden_method_call() {
    MessageWriter writer;
    MessageHeader header;
    header.type = MessageType::MethodCall;
    header.serial = 1;
    header.path = "/a";
    header.member = "M";
    if (!expect(writer.begin_message(header).has_value(), "begin message")) {
        return false;
    }
    if (!expect(writer.end_message().has_value(), "end message")) {
        return false;
    }
    const std::string expected = "6C010001"
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
                                 "000000000000";
    if (!expect_equal(hex(writer.bytes()), expected, "golden method call bytes")) {
        return false;
    }
    if (!expect(writer.size() == 48, "golden message size")) {
        return false;
    }
    const auto parsed = parse_message(writer.bytes());
    return expect(parsed.has_value(), "golden message reparses") &&
           expect(parsed->header.type == MessageType::MethodCall, "golden type") &&
           expect(parsed->header.serial == 1, "golden serial") &&
           expect_equal(parsed->header.path, "/a", "golden path") &&
           expect_equal(parsed->header.member, "M", "golden member") &&
           expect(parsed->body.empty(), "golden empty body") &&
           expect(parsed->consumed == 48, "golden consumed size");
}

// Golden bytes for a method return carrying one accessible reference: body a(so), one element
// (":1.0", "/org/a11y/atspi/accessible/0"), reply to serial 1.
bool golden_reference_return() {
    MessageWriter writer;
    MessageHeader header;
    header.type = MessageType::MethodReturn;
    header.serial = 2;
    header.reply_serial = 1;
    header.signature = "a(so)";
    if (!expect(writer.begin_message(header).has_value(), "begin return")) {
        return false;
    }
    writer.write_array('(', [](MessageWriter& out) {
        out.write_struct([](MessageWriter& element) {
            element.write_string(":1.0");
            element.write_object_path("/org/a11y/atspi/accessible/0");
        });
    });
    if (!expect(writer.end_message().has_value(), "end return")) {
        return false;
    }
    const std::string expected = "6C020001350000000200000013000000"
                                 "0501750001000000"
                                 "08016700056128736F2900"
                                 "0000000000"
                                 "2D00000000000000"
                                 "040000003A312E3000000000"
                                 "1C000000"
                                 "2F6F72672F613131792F61747370692F61636365737369626C652F30"
                                 "00";
    if (!expect_equal(hex(writer.bytes()), expected, "golden reference return bytes")) {
        return false;
    }
    const auto parsed = parse_message(writer.bytes());
    if (!expect(parsed.has_value(), "reference return reparses") ||
        !expect(parsed->header.type == MessageType::MethodReturn, "return type") ||
        !expect(parsed->header.reply_serial == 1, "reply serial") ||
        !expect_equal(parsed->header.signature, "a(so)", "body signature")) {
        return false;
    }
    // Decode the body and check the values the golden bytes encode.
    MessageReader reader(parsed->body);
    if (!expect(reader.read_array_header('(').has_value(), "reference array header")) {
        return false;
    }
    int count = 0;
    while (!reader.array_done()) {
        if (!expect(reader.begin_struct().has_value(), "reference struct")) {
            return false;
        }
        const std::string_view bus = reader.read_string();
        const std::string_view path = reader.read_object_path();
        reader.end_struct();
        if (!expect_equal(bus, ":1.0", "reference bus name") ||
            !expect_equal(path, "/org/a11y/atspi/accessible/0", "reference path")) {
            return false;
        }
        ++count;
    }
    return expect(reader.finish_array().has_value(), "reference array ends") &&
           expect(!reader.failed(), "reference body has no errors") &&
           expect(count == 1, "one reference");
}

bool body_round_trip() {
    MessageWriter writer;
    MessageHeader header;
    header.type = MessageType::Signal;
    header.serial = 7;
    header.path = "/org/a11y/atspi/accessible/3";
    header.interface = "org.a11y.atspi.Event.Object";
    header.member = "PropertyChange";
    header.signature = "siiva{sv}";
    if (!writer.begin_message(header).has_value()) {
        return expect(false, "begin signal");
    }
    writer.write_string("accessible-name");
    writer.write_int32(2);
    writer.write_int32(-5);
    writer.write_variant("s", [](MessageWriter& out) { out.write_string("Save"); });
    writer.write_array('{', [](MessageWriter& out) {
        out.write_struct([](MessageWriter& entry) {
            entry.write_string("toolkit");
            entry.write_variant("s", [](MessageWriter& value) { value.write_string("tessera"); });
        });
        out.write_struct([](MessageWriter& entry) {
            entry.write_string("count");
            entry.write_variant("u", [](MessageWriter& value) { value.write_uint32(42); });
        });
    });
    if (!expect(writer.end_message().has_value(), "end signal")) {
        return false;
    }
    const auto parsed = parse_message(writer.bytes());
    if (!expect(parsed.has_value(), "signal reparses") ||
        !expect_equal(parsed->header.signature, "siiva{sv}", "signal signature")) {
        return false;
    }
    MessageReader reader(parsed->body);
    const std::string_view name = reader.read_string();
    const std::int32_t detail1 = reader.read_int32();
    const std::int32_t detail2 = reader.read_int32();
    const auto value_signature = reader.read_variant_signature();
    if (!expect(value_signature.has_value(), "variant signature") ||
        !expect_equal(*value_signature, "s", "variant is a string")) {
        return false;
    }
    const std::string_view value = reader.read_string();
    if (!expect_equal(name, "accessible-name", "signal name") ||
        !expect(detail1 == 2 && detail2 == -5, "signal detail integers") ||
        !expect_equal(value, "Save", "signal variant value")) {
        return false;
    }
    if (!expect(reader.read_array_header('{').has_value(), "attributes array")) {
        return false;
    }
    std::string keys;
    std::uint32_t count_value = 0;
    while (!reader.array_done()) {
        if (!expect(reader.begin_struct().has_value(), "attribute entry")) {
            return false;
        }
        const std::string_view key = reader.read_string();
        const auto entry_signature = reader.read_variant_signature();
        if (!expect(entry_signature.has_value(), "attribute value signature")) {
            return false;
        }
        keys.append(key);
        keys.push_back(',');
        if (*entry_signature == "s") {
            keys.append(reader.read_string());
            keys.push_back(',');
        } else if (*entry_signature == "u") {
            count_value = reader.read_uint32();
        } else {
            return expect(false, "unexpected attribute value type");
        }
    }
    return expect(reader.finish_array().has_value(), "attributes array ends") &&
           expect_equal(keys, "toolkit,tessera,count,", "attribute keys") &&
           expect(count_value == 42, "attribute uint value");
}

bool big_endian_input() {
    const std::string bytes = "42010001"
                              "00000000"
                              "00000001"
                              "0000001A"
                              "01016F00"
                              "00000002"
                              "2F6100"
                              "0000000000"
                              "03017300"
                              "00000001"
                              "4D00"
                              "000000000000";
    const std::vector<std::uint8_t> raw = from_hex(bytes);
    const auto parsed = parse_message(raw);
    return expect(parsed.has_value(), "big-endian message parses") &&
           expect_equal(parsed->header.path, "/a", "big-endian path") &&
           expect_equal(parsed->header.member, "M", "big-endian member") &&
           expect(parsed->header.serial == 1, "big-endian serial") &&
           expect(parsed->consumed == raw.size(), "big-endian consumed");
}

bool stream_framing() {
    MessageWriter first;
    MessageHeader header;
    header.type = MessageType::MethodCall;
    header.serial = 1;
    header.path = "/a";
    header.member = "M";
    (void)first.begin_message(header);
    (void)first.end_message();

    MessageWriter second;
    MessageHeader reply;
    reply.type = MessageType::MethodReturn;
    reply.serial = 2;
    reply.reply_serial = 1;
    reply.signature = "s";
    (void)second.begin_message(reply);
    second.write_string("ok");
    (void)second.end_message();

    std::vector<std::uint8_t> stream(first.bytes().begin(), first.bytes().end());
    stream.insert(stream.end(), second.bytes().begin(), second.bytes().end());

    const auto first_parsed = parse_message(stream);
    if (!expect(first_parsed.has_value(), "first frame parses") ||
        !expect(first_parsed->consumed == first.size(), "first frame size")) {
        return false;
    }
    const auto second_parsed =
        parse_message(std::span<const std::uint8_t>(stream).subspan(first_parsed->consumed));
    if (!expect(second_parsed.has_value(), "second frame parses") ||
        !expect(second_parsed->consumed == second.size(), "second frame size")) {
        return false;
    }
    MessageReader reader(second_parsed->body);
    return expect_equal(reader.read_string(), "ok", "second frame body");
}

bool unknown_header_field_is_skipped() {
    // Field code 9 is unassigned in the protocol: a real bus may add fields, so the parser must
    // walk the variant and keep going instead of failing. The bytes below are a method call with
    // path "/a", unknown field (variant "s" = "x"), and member "M".
    const std::string bytes = "6C010001"
                              "00000000"
                              "01000000"
                              "30000000"
                              "01016F00"
                              "02000000"
                              "2F6100"
                              "0000000000"
                              "09017300"
                              "01000000"
                              "7800"
                              "000000000000"
                              "03017300"
                              "01000000"
                              "4D00"
                              "000000000000";
    const std::vector<std::uint8_t> raw = from_hex(bytes);
    const auto parsed = parse_message(raw);
    return expect(parsed.has_value(), "unknown header field is skipped") &&
           expect(parsed->header.destination.empty(), "unknown field is not assigned") &&
           expect_equal(parsed->header.path, "/a", "path before the unknown field") &&
           expect_equal(parsed->header.member, "M", "member after the unknown field") &&
           expect(parsed->consumed == raw.size(), "unknown field message size");
}

} // namespace

int main() {
    if (!type_tables()) {
        return 1;
    }
    if (!signature_validation()) {
        return 2;
    }
    if (!golden_method_call()) {
        return 3;
    }
    if (!golden_reference_return()) {
        return 4;
    }
    if (!body_round_trip()) {
        return 5;
    }
    if (!big_endian_input()) {
        return 6;
    }
    if (!stream_framing()) {
        return 7;
    }
    if (!unknown_header_field_is_skipped()) {
        return 8;
    }
    std::cout << "a11y_dbus_wire: ok\n";
    return 0;
}
