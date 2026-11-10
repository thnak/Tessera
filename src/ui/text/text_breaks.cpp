// SPDX-License-Identifier: MIT

#include "edit_grapheme.hpp"
#include "icu_runtime.hpp"
#include "text_breaks.hpp"

#if defined(TESSERA_ICU_ENABLED)
#include <unicode/ubrk.h>
#include <unicode/utext.h>
#endif

#include <cstdint>

namespace tessera::ui::text {
namespace {

constexpr std::uint32_t kZeroWidthSpace = 0x200B;

bool is_ascii_space(std::uint32_t cp) {
    return cp == ' ' || cp == '\t' || cp == '\n' || cp == '\r' || cp == '\f' || cp == 0x0B;
}

// Reduced CJK range set for the builtin line rules: kana, CJK ideographs, Hangul syllables,
// compatibility ideographs, and full-width forms. ICU's UAX #14 data covers the full table; this
// is the subset the ICU-free build documents.
bool is_cjk(std::uint32_t cp) {
    return (cp >= 0x2E80 && cp <= 0x33FF) || (cp >= 0x3040 && cp <= 0x30FF) ||
           (cp >= 0x3400 && cp <= 0x4DBF) || (cp >= 0x4E00 && cp <= 0x9FFF) ||
           (cp >= 0xAC00 && cp <= 0xD7AF) || (cp >= 0xF900 && cp <= 0xFAFF) ||
           (cp >= 0xFF00 && cp <= 0xFFEF) || (cp >= 0x20000 && cp <= 0x2FA1F);
}

void push_unique(BreakBoundaries& boundaries, std::size_t offset, std::size_t limit) {
    if (offset > limit) {
        return;
    }
    if (!boundaries.offsets.empty() && boundaries.offsets.back() >= offset) {
        return;
    }
    boundaries.offsets.push_back(offset);
}

void finish(BreakBoundaries& boundaries, std::size_t size, BreakBackend backend) {
    boundaries.backend = backend;
    if (size == 0) {
        return;
    }
    push_unique(boundaries, size, size);
}

#if defined(TESSERA_ICU_ENABLED)
BreakBoundaries icu_break_iteration(std::string_view text, UBreakIteratorType type) {
    BreakBoundaries boundaries;
    boundaries.offsets.push_back(0);
    if (text.empty()) {
        return boundaries;
    }
    if (icu::initialize() == icu::InitStatus::Failed) {
        return type == UBRK_WORD ? builtin_word_boundaries(text) : builtin_line_boundaries(text);
    }

    UErrorCode status = U_ZERO_ERROR;
    UText utext = UTEXT_INITIALIZER;
    utext_openUTF8(&utext, text.data(), static_cast<int64_t>(text.size()), &status);
    if (U_FAILURE(status)) {
        utext_close(&utext);
        return type == UBRK_WORD ? builtin_word_boundaries(text) : builtin_line_boundaries(text);
    }

    // Root rules: the reduced package ships the locale-independent break rules and dictionaries;
    // per-locale tailoring (ja/zh line-break styles) is a stylesheet concern, not data.
    status = U_ZERO_ERROR;
    UBreakIterator* iterator = ubrk_open(type, "", nullptr, 0, &status);
    if (U_FAILURE(status) || iterator == nullptr) {
        utext_close(&utext);
        return type == UBRK_WORD ? builtin_word_boundaries(text) : builtin_line_boundaries(text);
    }
    status = U_ZERO_ERROR;
    ubrk_setUText(iterator, &utext, &status);
    if (U_SUCCESS(status)) {
        // With a UText, iterator indices are native (UTF-8 byte) offsets.
        for (int32_t at = ubrk_first(iterator); at != UBRK_DONE; at = ubrk_next(iterator)) {
            push_unique(boundaries, static_cast<std::size_t>(at), text.size());
        }
    }
    ubrk_close(iterator);
    utext_close(&utext);

    if (U_FAILURE(status)) {
        return type == UBRK_WORD ? builtin_word_boundaries(text) : builtin_line_boundaries(text);
    }
    finish(boundaries, text.size(), BreakBackend::Icu);
    return boundaries;
}
#endif

} // namespace

BreakBackend active_backend() {
#if defined(TESSERA_ICU_ENABLED)
    return BreakBackend::Icu;
#else
    return BreakBackend::Builtin;
#endif
}

BreakBoundaries builtin_word_boundaries(std::string_view text) {
    BreakBoundaries boundaries;
    boundaries.offsets.push_back(0);
    bool in_word = false;
    for (EditOffset at = 0; at < text.size();) {
        const std::uint32_t cp = edit_detail::codepoint_at(text, at);
        const bool is_word = edit_detail::is_word_codepoint(cp);
        if (is_word != in_word) {
            push_unique(boundaries, at, text.size());
            in_word = is_word;
        }
        at = edit_detail::next_codepoint(text, at);
    }
    finish(boundaries, text.size(), BreakBackend::Builtin);
    return boundaries;
}

BreakBoundaries builtin_line_boundaries(std::string_view text) {
    BreakBoundaries boundaries;
    boundaries.offsets.push_back(0);
    bool in_space = false;
    for (EditOffset at = 0; at < text.size();) {
        const std::uint32_t cp = edit_detail::codepoint_at(text, at);
        const EditOffset next = edit_detail::next_codepoint(text, at);
        if (is_ascii_space(cp)) {
            in_space = true;
            at = next;
            continue;
        }
        if (in_space) {
            // A break sits after the whitespace run, as UAX #14 rule LB18 does.
            push_unique(boundaries, at, text.size());
            in_space = false;
        }
        if (cp == '-' || cp == '/' || cp == kZeroWidthSpace) {
            push_unique(boundaries, next, text.size());
        }
        if (is_cjk(cp)) {
            push_unique(boundaries, at, text.size());
            push_unique(boundaries, next, text.size());
        }
        at = next;
    }
    finish(boundaries, text.size(), BreakBackend::Builtin);
    return boundaries;
}

BreakBoundaries word_boundaries(std::string_view text) {
#if defined(TESSERA_ICU_ENABLED)
    return icu_break_iteration(text, UBRK_WORD);
#else
    return builtin_word_boundaries(text);
#endif
}

BreakBoundaries line_boundaries(std::string_view text, BreakBackend backend) {
#if defined(TESSERA_ICU_ENABLED)
    if (backend == BreakBackend::Icu) {
        return icu_break_iteration(text, UBRK_LINE);
    }
#else
    (void)backend; // the builtin rules are the only engine this build carries
#endif
    return builtin_line_boundaries(text);
}

BreakBoundaries line_boundaries(std::string_view text) {
    return line_boundaries(text, active_backend());
}

} // namespace tessera::ui::text
