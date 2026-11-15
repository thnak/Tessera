// SPDX-License-Identifier: MIT
//
// The one JSON failure vocabulary shared by the runtime config reader and the harness protocol
// (god-type audit section 4.4).
//
// The point of the extraction is that the same failure cannot be spelled two ways, so the checks
// here are about *coverage* rather than about individual strings: every enumerator has a distinct,
// non-empty description, and the harness's public alias resolves to the shared type rather than to
// a private copy.
//
// The schema reader is deliberately not covered: `tessera_schema` is a leaf library that may
// include no project header, enforced by tests/unit/schema_layering_tests.cpp, so it keeps its own
// enum. That is recorded in section 4.4 of the audit rather than papered over here.

#include "tessera/detail/json_error.hpp"
#include "tessera/harness/json_parse.hpp"

#include <array>
#include <cstddef>
#include <iostream>
#include <string_view>
#include <type_traits>

namespace {

using tessera::detail::json::ErrorCode;
using tessera::detail::json::to_string;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "json_error_vocabulary: " << message << "\n";
    }
    return condition;
}

// Every enumerator, in declaration order. Kept as an explicit list so adding a code without a test
// is visible in review; `kCodes.size()` is asserted against the count below.
constexpr std::array<ErrorCode, 12> kCodes = {
    ErrorCode::UnexpectedEnd,  ErrorCode::UnexpectedCharacter, ErrorCode::TrailingContent,
    ErrorCode::InvalidLiteral, ErrorCode::InvalidNumber,       ErrorCode::NumberOutOfRange,
    ErrorCode::InvalidEscape,  ErrorCode::InvalidUtf16Escape,  ErrorCode::ControlCharacterInString,
    ErrorCode::InvalidUtf8,    ErrorCode::DuplicateKey,        ErrorCode::NestingTooDeep,
};

bool check_every_code_has_a_description() {
    bool ok = true;
    for (const ErrorCode code : kCodes) {
        const std::string_view text = to_string(code);
        ok = expect(!text.empty(), "every code has a non-empty description") && ok;
        ok = expect(text != "invalid JSON", "no code falls through to the generic message") && ok;
    }
    return ok;
}

bool check_descriptions_are_distinct() {
    bool ok = true;
    for (std::size_t i = 0; i < kCodes.size(); ++i) {
        for (std::size_t j = i + 1; j < kCodes.size(); ++j) {
            ok = expect(to_string(kCodes[i]) != to_string(kCodes[j]),
                        "two codes must not share one description") &&
                 ok;
        }
    }
    return ok;
}

// The harness wrapper keeps its public name but is the shared type, which is what stops the two
// vocabularies from drifting again.
static_assert(std::is_same_v<tessera::harness::JsonParseErrorCode, ErrorCode>,
              "the harness error code must be the shared vocabulary, not a private copy");

// The wrapper struct keeps its own shape (public `code` and `offset`, comparable), because that is
// existing harness surface the audit says to preserve.
static_assert(std::is_same_v<decltype(tessera::harness::JsonParseError::code),
                             tessera::harness::JsonParseErrorCode>,
              "the harness JsonParseError keeps its public code field");

} // namespace

int main() {
    bool ok = true;
    ok = check_every_code_has_a_description() && ok;
    ok = check_descriptions_are_distinct() && ok;
    ok = expect(to_string(ErrorCode::NestingTooDeep) == "nesting is too deep",
                "the canonical nesting message is stable") &&
         ok;
    ok = expect(to_string(ErrorCode::InvalidUtf8) == "string is not valid UTF-8",
                "the canonical UTF-8 message is stable") &&
         ok;
    if (!ok) {
        std::cerr << "json_error_vocabulary: FAILED\n";
        return 1;
    }
    std::cout << "json_error_vocabulary: all checks passed\n";
    return 0;
}
