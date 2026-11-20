// SPDX-License-Identifier: MIT

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// A minimal JSON reader for the built-in hook manifest (docs/specs/jsx-subset-v1.md section 7.1).
//
// The front end has no third-party dependency and must not gain one for a file it embeds at build
// time, so this is a total parser over a bounded document: objects, arrays, strings, numbers,
// booleans and null, with a depth cap. It is deliberately not a general-purpose library — there is
// no writer, no streaming, and no number formatting, because the manifest is read once and never
// written back.
//
// Object members keep insertion order, so a diagnostic can name the member the author wrote rather
// than a hash order. Duplicate keys are rejected rather than silently last-wins: a manifest with
// two `name` fields is an authoring error, not a value.
namespace tessera::compiler::jsx::json {

enum class Kind : std::uint8_t {
    Null,
    Bool,
    Number,
    String,
    Array,
    Object,
};

class Value;
using Member = std::pair<std::string, Value>;

class Value {
public:
    using Array = std::vector<Value>;
    using Object = std::vector<Member>;

    Value() = default; // null
    explicit Value(bool value)
        : kind_(Kind::Bool),
          boolean_(value) {}
    explicit Value(double value)
        : kind_(Kind::Number),
          number_(value) {}
    explicit Value(std::string value)
        : kind_(Kind::String),
          text_(std::move(value)) {}
    explicit Value(Array value)
        : kind_(Kind::Array),
          array_(std::move(value)) {}
    explicit Value(Object value)
        : kind_(Kind::Object),
          object_(std::move(value)) {}

    [[nodiscard]] Kind kind() const noexcept {
        return kind_;
    }
    [[nodiscard]] bool is_null() const noexcept {
        return kind_ == Kind::Null;
    }
    [[nodiscard]] std::optional<bool> as_bool() const noexcept {
        return kind_ == Kind::Bool ? std::optional<bool>(boolean_) : std::nullopt;
    }
    [[nodiscard]] std::optional<double> as_number() const noexcept {
        return kind_ == Kind::Number ? std::optional<double>(number_) : std::nullopt;
    }
    [[nodiscard]] std::optional<std::string_view> as_string() const noexcept {
        return kind_ == Kind::String ? std::optional<std::string_view>(text_) : std::nullopt;
    }
    [[nodiscard]] const Array* as_array() const noexcept {
        return kind_ == Kind::Array ? &array_ : nullptr;
    }
    [[nodiscard]] const Object* as_object() const noexcept {
        return kind_ == Kind::Object ? &object_ : nullptr;
    }

    // The member named `key`, or nullptr. A non-object is nullptr, so a caller can chain a lookup
    // without checking the kind first.
    [[nodiscard]] const Value* find(std::string_view key) const noexcept;

private:
    Kind kind_ = Kind::Null;
    bool boolean_ = false;
    double number_ = 0.0;
    std::string text_;
    Array array_;
    Object object_;
};

// Why a document was rejected. The offset is a byte offset into the input, so a diagnostic can
// point at the offending character.
enum class ErrorCode : std::uint8_t {
    UnexpectedEnd,
    UnexpectedCharacter,
    TrailingContent,
    DepthExceeded,
    DuplicateKey,
    InvalidNumber,
    InvalidEscape,
    InvalidUtf8,
};

struct Error {
    ErrorCode code = ErrorCode::UnexpectedEnd;
    std::size_t offset = 0;

    friend constexpr bool operator==(const Error&, const Error&) = default;
};

// The nesting cap. A manifest is three levels deep; the cap exists so a malformed file cannot
// exhaust the stack.
inline constexpr std::size_t kMaxDepth = 32;

// Parses `text` as one JSON document. Trailing whitespace is allowed; trailing content is not.
[[nodiscard]] std::optional<Value> parse(std::string_view text, Error* error = nullptr);

} // namespace tessera::compiler::jsx::json
