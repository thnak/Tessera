// SPDX-License-Identifier: MIT

#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace tessera::ui::a11y {

// D-Bus wire codec (docs/specs/accessibility-v1.md section 7, docs/design/accessibility.md section
// 3). The AT-SPI2 bridge speaks the protocol itself instead of linking libatspi/glib (ADR-012), so
// this file is the complete serialization surface: the type/alignment tables, signature
// validation, `MessageWriter`, and `MessageReader`/`parse_message`.
//
// Rules enforced on both directions:
//   * every value is written and read at its D-Bus alignment;
//   * every read is bounds-checked against the enclosing container, and an array's declared byte
//     length must exactly match the bytes its elements consume;
//   * strings, object paths, signatures, header fields, bodies, and whole messages are capped, so a
//     hostile or corrupt peer cannot make the bridge allocate or loop without bound;
//   * errors are values (`WireError`), never exceptions.

enum class WireType : std::uint8_t {
    Invalid = 0,
    Byte,       // y
    Boolean,    // b
    Int16,      // n
    UInt16,     // q
    Int32,      // i
    UInt32,     // u
    Int64,      // x
    UInt64,     // t
    Double,     // d
    String,     // s
    ObjectPath, // o
    Signature,  // g
    Array,      // a
    Struct,     // (
    DictEntry,  // {
    Variant,    // v
    UnixFd,     // h
};

// Alignment in bytes for a value of `type`; 0 for Invalid.
[[nodiscard]] std::size_t alignment_of(WireType type);
// Alignment in bytes for the type code character; 0 for an unknown code.
[[nodiscard]] std::size_t alignment_of_code(char code);
[[nodiscard]] char code_of(WireType type);
[[nodiscard]] WireType type_from_code(char code);

// Length in bytes of the single complete type at the start of `signature`, or 0 when the signature
// is truncated or malformed (unbalanced containers, unknown codes, dict entries without exactly two
// members, arrays without a complete element type).
[[nodiscard]] std::size_t complete_type_length(std::string_view signature);
// True when `signature` is one or more complete types and nothing else (a message signature).
[[nodiscard]] bool valid_signature(std::string_view signature);
// True when `signature` is exactly one complete type (a variant value signature).
[[nodiscard]] bool single_complete_type(std::string_view signature);

enum class WireError : std::uint8_t {
    None = 0,
    Truncated,       // not enough bytes for the value/container being read
    BadType,         // unknown or unexpected type code
    BadSignature,    // malformed signature text
    BadAlignment,    // a container length does not match its element framing
    BadEndian,       // the message does not start with 'l' or 'B'
    BadVersion,      // protocol version is not 1
    TooLarge,        // a bounded value (text/body/message) exceeded its cap
    ValueOutOfRange, // boolean outside {0,1} or a malformed object path
    MissingField,    // a required header field is absent for the message type
    Unbalanced,      // container nesting exceeded the reader depth limit
    BadTerminator,   // a string/signature is not NUL-terminated on the wire
};

[[nodiscard]] std::string_view to_string(WireError error);

// Sizes are chosen so a single bridged node (name+value+children) always fits while a corrupt
// length prefix cannot turn into unbounded work.
inline constexpr std::size_t kMaxWireText = 512;        // one string/object path/signature
inline constexpr std::size_t kMaxWireBody = 256 * 1024; // one message body
inline constexpr std::size_t kMaxWireMessage = kMaxWireBody + 4096;
inline constexpr std::size_t kMaxWireFieldBytes = 4096; // the header-fields array
inline constexpr std::size_t kMaxWireDepth = 16;        // nested containers

enum class ByteOrder : std::uint8_t { Little, Big };

[[nodiscard]] ByteOrder host_byte_order();

enum class MessageType : std::uint8_t {
    MethodCall = 1,
    MethodReturn = 2,
    Error = 3,
    Signal = 4,
};

enum class MessageFlag : std::uint8_t {
    None = 0,
    NoReplyExpected = 1U << 0,
    NoAutoStart = 1U << 1,
    AllowInteractiveAuth = 1U << 2,
};

// Header field codes, in protocol order.
enum class HeaderField : std::uint8_t {
    Path = 1,
    Interface = 2,
    Member = 3,
    ErrorName = 4,
    ReplySerial = 5,
    Destination = 6,
    Sender = 7,
    Signature = 8,
};

// The header fields the bridge uses. Strings are owned; `signature` is the body signature.
struct MessageHeader {
    MessageType type = MessageType::MethodCall;
    std::uint8_t flags = 0;
    std::uint32_t serial = 0;
    std::uint32_t reply_serial = 0;
    std::string path;
    std::string interface;
    std::string member;
    std::string error_name;
    std::string destination;
    std::string sender;
    std::string signature;
};

// Serializes one message. All writes are append-only into a bounded buffer; the first failure is
// sticky (`error()`), so callers can build a message with straight-line code and check once when
// `end_message()` returns. Writes after a failure are ignored.
class MessageWriter {
public:
    MessageWriter();

    void write_byte(std::uint8_t value);
    void write_bool(bool value);
    void write_int16(std::int16_t value);
    void write_uint16(std::uint16_t value);
    void write_int32(std::int32_t value);
    void write_uint32(std::uint32_t value);
    void write_int64(std::int64_t value);
    void write_uint64(std::uint64_t value);
    void write_double(double value);
    void write_string(std::string_view value);
    void write_object_path(std::string_view value);
    void write_signature(std::string_view value);

    // Containers. `begin_array`/`end_array` must pair; `write_array` and `write_struct` wrap the
    // pairing so a lambda cannot forget it. `write_variant` writes the value signature and then
    // calls `value(*this)`; the first value write supplies its own alignment.
    void begin_array(char element_code);
    void end_array();
    void begin_struct();
    void end_struct();

    template<typename Fn> void write_array(char element_code, Fn&& elements) {
        begin_array(element_code);
        elements(*this);
        end_array();
    }
    template<typename Fn> void write_struct(Fn&& fields) {
        begin_struct();
        fields(*this);
        end_struct();
    }
    template<typename Fn> void write_variant(std::string_view signature, Fn&& value) {
        write_signature(signature);
        value(*this);
    }

    // Frames a message: fixed header, header fields, padding to the body, then the caller writes
    // the body and calls `end_message()`, which patches the body length.
    [[nodiscard]] std::expected<void, WireError> begin_message(const MessageHeader& header);
    [[nodiscard]] std::expected<void, WireError> end_message();

    [[nodiscard]] std::span<const std::uint8_t> bytes() const {
        return bytes_;
    }
    [[nodiscard]] std::size_t size() const {
        return bytes_.size();
    }
    [[nodiscard]] WireError error() const {
        return error_;
    }
    void reset();

private:
    struct ArrayFrame {
        std::size_t length_offset = 0;
        std::size_t start = 0;
        char code = 'y';
    };

    void fail(WireError error);
    void align(std::size_t alignment);
    void write_raw(const void* data, std::size_t size);
    void reserve(std::size_t size);
    void patch_u32(std::size_t offset, std::uint32_t value);
    void write_string_like(std::string_view value, std::size_t length_alignment);
    void write_header_field(HeaderField field, std::string_view signature,
                            const std::string& value);
    void write_header_field_u32(HeaderField field, std::uint32_t value);

    std::vector<std::uint8_t> bytes_;
    std::vector<ArrayFrame> arrays_;
    std::size_t body_length_offset_ = 0;
    std::size_t fields_length_offset_ = 0;
    std::size_t fields_start_ = 0;
    std::size_t body_start_ = 0;
    bool message_open_ = false;
    WireError error_ = WireError::None;
};

// Parses a message header and returns the body span plus the total bytes consumed, so a stream
// reader can advance. Every header field is validated: bounded text, aligned values, and exactly
// one value per element; unknown fields are skipped through their variant signature.
struct ParsedMessage {
    MessageHeader header;
    std::span<const std::uint8_t> body;
    std::size_t consumed = 0;
};

[[nodiscard]] std::expected<ParsedMessage, WireError>
parse_message(std::span<const std::uint8_t> bytes);

// Bounded, alignment-checking reader. Limits nest: an array pushes its declared end, so a nested
// value can never read past the bytes its container claims.
//
// Scalar reads are sticky: a failed read records `error()` and returns a zero value, so
// straight-line decoding code checks `failed()` once per group instead of threading a
// `std::expected` through every field. Framing operations (`read_array_header`, `finish_array`,
// `begin_struct`, `read_variant_signature`, `skip_value`) return `std::expected` because their
// failures are the ones callers dispatch on.
class MessageReader {
public:
    explicit MessageReader(std::span<const std::uint8_t> bytes,
                           ByteOrder order = ByteOrder::Little);

    [[nodiscard]] std::uint8_t read_byte();
    [[nodiscard]] bool read_bool();
    [[nodiscard]] std::int16_t read_int16();
    [[nodiscard]] std::uint16_t read_uint16();
    [[nodiscard]] std::int32_t read_int32();
    [[nodiscard]] std::uint32_t read_uint32();
    [[nodiscard]] std::int64_t read_int64();
    [[nodiscard]] std::uint64_t read_uint64();
    [[nodiscard]] double read_double();
    // Borrowed views into the input buffer; valid as long as the buffer is.
    [[nodiscard]] std::string_view read_string();
    [[nodiscard]] std::string_view read_object_path();
    [[nodiscard]] std::string_view read_signature();

    // Array framing: reads the uint32 length, applies the element alignment, and pushes the array's
    // end as the active limit. `array_done()` reports when the caller consumed exactly the declared
    // bytes; `finish_array()` pops the limit and fails when they do not match.
    [[nodiscard]] std::expected<void, WireError> read_array_header(char element_code);
    [[nodiscard]] bool array_done() const;
    [[nodiscard]] std::expected<void, WireError> finish_array();

    [[nodiscard]] std::expected<void, WireError> begin_struct();
    void end_struct();
    // Reads a variant's value signature; callers dispatch on the returned text and then read the
    // value. The signature is validated as exactly one complete type.
    [[nodiscard]] std::expected<std::string_view, WireError> read_variant_signature();
    // Skips one value of `signature` (must be a single complete type); used for unknown header
    // fields and for validating bodies the bridge does not interpret.
    [[nodiscard]] std::expected<void, WireError> skip_value(std::string_view signature);

    [[nodiscard]] std::expected<void, WireError> align(std::size_t alignment);
    [[nodiscard]] std::size_t offset() const {
        return offset_;
    }
    [[nodiscard]] std::size_t limit() const;
    [[nodiscard]] bool exhausted() const;
    [[nodiscard]] bool failed() const {
        return error_ != WireError::None;
    }
    [[nodiscard]] WireError error() const {
        return error_;
    }
    void seek(std::size_t offset);
    void push_limit(std::size_t limit);
    void pop_limit();

private:
    [[nodiscard]] bool can_read(std::size_t count) const;
    void fail(WireError error);
    [[nodiscard]] std::uint32_t read_u32_unchecked();
    [[nodiscard]] std::uint64_t read_u64_unchecked();

    std::span<const std::uint8_t> bytes_;
    ByteOrder order_ = ByteOrder::Little;
    std::size_t offset_ = 0;
    std::size_t depth_ = 0;
    std::size_t limits_[kMaxWireDepth] = {};
    WireError error_ = WireError::None;
};

} // namespace tessera::ui::a11y
