// SPDX-License-Identifier: MIT
//
// One JSON error vocabulary for the three in-tree JSON readers (god-type audit section 4.4).
//
// The project schema, the runtime config and the harness protocol each carry their own JSON parser
// on purpose: they read different documents with different extra rules (the manifest rejects
// duplicate keys as a build error, the harness envelope bounds nesting for untrusted input). What
// they share is the *failure vocabulary* - syntax, depth, escape, UTF-8, trailing data - and that
// used to be three parallel enums with three formatters, so the same failure could be reported
// under three names.
//
// This header is the one vocabulary. It lives in `tessera/detail/` because it is not an API: the
// harness keeps its own `JsonParseError` wrapper struct (its `code` field is public surface), and
// mapping to it is a one-line conversion rather than an alias, so a future harness-specific code
// can be added without widening this enum for everyone.
//
// Adding a code here means every reader must handle it, which is the point: `to_string` is a switch
// over the whole enum with no `default`, so a new code that is not described is a compile error.

#pragma once

#include <cstdint>
#include <string_view>

namespace tessera::detail::json {

enum class ErrorCode : std::uint8_t {
    // Generic syntax.
    UnexpectedEnd,
    UnexpectedCharacter,
    TrailingContent,
    // Literals and numbers.
    InvalidLiteral,
    InvalidNumber,
    NumberOutOfRange,
    // Strings.
    InvalidEscape,
    InvalidUtf16Escape,
    ControlCharacterInString,
    InvalidUtf8,
    // Structure.
    DuplicateKey,
    NestingTooDeep,
};

// The human-readable reason, for a diagnostic message. Every code has one; the switch has no
// `default` arm so a new code fails to compile rather than falling through to a generic string.
[[nodiscard]] constexpr std::string_view to_string(ErrorCode code) noexcept {
    switch (code) {
    case ErrorCode::UnexpectedEnd:
        return "unexpected end of input";
    case ErrorCode::UnexpectedCharacter:
        return "unexpected character";
    case ErrorCode::TrailingContent:
        return "trailing content after the document";
    case ErrorCode::InvalidLiteral:
        return "invalid literal";
    case ErrorCode::InvalidNumber:
        return "invalid number literal";
    case ErrorCode::NumberOutOfRange:
        return "number is out of range";
    case ErrorCode::InvalidEscape:
        return "invalid escape sequence";
    case ErrorCode::InvalidUtf16Escape:
        return "invalid UTF-16 escape";
    case ErrorCode::ControlCharacterInString:
        return "raw control character in a string";
    case ErrorCode::InvalidUtf8:
        return "string is not valid UTF-8";
    case ErrorCode::DuplicateKey:
        return "duplicate object key";
    case ErrorCode::NestingTooDeep:
        return "nesting is too deep";
    }
    return "invalid JSON";
}

} // namespace tessera::detail::json
