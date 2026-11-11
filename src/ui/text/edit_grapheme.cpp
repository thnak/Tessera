// SPDX-License-Identifier: MIT

#include "edit_grapheme.hpp"
#include "text_breaks.hpp"

#include <cctype>
#include <optional>

namespace tessera::ui::text::edit_detail {
namespace {

constexpr std::uint32_t kReplacement = 0xFFFD;
constexpr std::uint32_t kZwj = 0x200D;

// Extend code points: combining marks (the main combining blocks), variation selectors, emoji
// modifiers, and the enclosing keycap. This is the documented reduced set; full Unicode Grapheme
// Cluster Break (Hangul jamo sequences, Indic conjuncts, spacing marks) is out of scope.
bool is_extend(std::uint32_t cp) {
    return (cp >= 0x0300 && cp <= 0x036F) || (cp >= 0x1AB0 && cp <= 0x1AFF) ||
           (cp >= 0x1DC0 && cp <= 0x1DFF) || (cp >= 0x20D0 && cp <= 0x20FF) ||
           (cp >= 0xFE20 && cp <= 0xFE2F) || (cp >= 0xFE00 && cp <= 0xFE0F) ||
           (cp >= 0xE0100 && cp <= 0xE01EF) || (cp >= 0x1F3FB && cp <= 0x1F3FF) || cp == 0x20E3;
}

bool is_regional_indicator(std::uint32_t cp) {
    return cp >= 0x1F1E6 && cp <= 0x1F1FF;
}

bool is_continuation(unsigned char byte) {
    return (byte & 0xC0U) == 0x80U;
}

} // namespace

std::size_t codepoint_byte_length(unsigned char lead) {
    if (lead < 0x80U) {
        return 1;
    }
    if ((lead & 0xE0U) == 0xC0U) {
        return 2;
    }
    if ((lead & 0xF0U) == 0xE0U) {
        return 3;
    }
    if ((lead & 0xF8U) == 0xF0U) {
        return 4;
    }
    return 1; // invalid lead / continuation byte
}

bool valid_utf8(std::string_view text) {
    std::size_t index = 0;
    while (index < text.size()) {
        const unsigned char lead = static_cast<unsigned char>(text[index]);
        if (lead < 0x80U) {
            ++index;
            continue;
        }
        std::size_t length = 0;
        std::uint32_t cp = 0;
        if ((lead & 0xE0U) == 0xC0U) {
            length = 2;
            cp = lead & 0x1FU;
        } else if ((lead & 0xF0U) == 0xE0U) {
            length = 3;
            cp = lead & 0x0FU;
        } else if ((lead & 0xF8U) == 0xF0U) {
            length = 4;
            cp = lead & 0x07U;
        } else {
            return false;
        }
        if (index + length > text.size()) {
            return false;
        }
        for (std::size_t offset = 1; offset < length; ++offset) {
            const unsigned char byte = static_cast<unsigned char>(text[index + offset]);
            if (!is_continuation(byte)) {
                return false;
            }
            cp = (cp << 6U) | (byte & 0x3FU);
        }
        // Reject overlong encodings, surrogates, and out-of-range code points.
        if ((length == 2 && cp < 0x80U) || (length == 3 && cp < 0x800U) ||
            (length == 4 && cp < 0x10000U)) {
            return false;
        }
        if (cp >= 0xD800U && cp <= 0xDFFFU) {
            return false;
        }
        if (cp > 0x10FFFFU) {
            return false;
        }
        index += length;
    }
    return true;
}

std::string normalize_newlines(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (std::size_t index = 0; index < text.size(); ++index) {
        if (text[index] == '\r') {
            out.push_back('\n');
            if (index + 1 < text.size() && text[index + 1] == '\n') {
                ++index;
            }
            continue;
        }
        out.push_back(text[index]);
    }
    return out;
}

EditOffset next_codepoint(std::string_view text, EditOffset offset) {
    if (offset >= text.size()) {
        return text.size();
    }
    const std::size_t length = codepoint_byte_length(static_cast<unsigned char>(text[offset]));
    return offset + length > text.size() ? text.size() : offset + length;
}

EditOffset prev_codepoint(std::string_view text, EditOffset offset) {
    if (offset == 0) {
        return 0;
    }
    EditOffset index = offset - 1;
    while (index > 0 && is_continuation(static_cast<unsigned char>(text[index]))) {
        --index;
    }
    return index;
}

std::uint32_t codepoint_at(std::string_view text, EditOffset offset) {
    if (offset >= text.size()) {
        return kReplacement;
    }
    const unsigned char lead = static_cast<unsigned char>(text[offset]);
    const std::size_t length = codepoint_byte_length(lead);
    if (lead < 0x80U || length == 1 || offset + length > text.size()) {
        return length == 1 && lead < 0x80U ? lead : kReplacement;
    }
    std::uint32_t cp = 0;
    if (length == 2) {
        cp = lead & 0x1FU;
    } else if (length == 3) {
        cp = lead & 0x0FU;
    } else {
        cp = lead & 0x07U;
    }
    for (std::size_t index = 1; index < length; ++index) {
        const unsigned char byte = static_cast<unsigned char>(text[offset + index]);
        if (!is_continuation(byte)) {
            return kReplacement;
        }
        cp = (cp << 6U) | (byte & 0x3FU);
    }
    return cp;
}

EditOffset next_cluster(std::string_view text, EditOffset offset) {
    if (offset >= text.size()) {
        return text.size();
    }
    if (const std::optional<EditOffset> icu = grapheme_following(text, offset)) {
        return *icu;
    }
    return builtin_next_cluster(text, offset);
}

EditOffset builtin_next_cluster(std::string_view text, EditOffset offset) {
    if (offset >= text.size()) {
        return text.size();
    }
    const std::uint32_t first = codepoint_at(text, offset);
    EditOffset cursor = next_codepoint(text, offset);
    // CRLF is one cluster (the buffer normalizes CR on insertion, but helpers stay defensive).
    if (first == '\r' && cursor < text.size() && text[cursor] == '\n') {
        return next_codepoint(text, cursor);
    }
    const bool first_is_ri = is_regional_indicator(first);
    bool ri_pair_used = false;
    while (cursor < text.size()) {
        const std::uint32_t cp = codepoint_at(text, cursor);
        if (is_extend(cp)) {
            cursor = next_codepoint(text, cursor);
            continue;
        }
        if (cp == kZwj) {
            cursor = next_codepoint(text, cursor);
            if (cursor < text.size()) {
                // A ZWJ joins the next code point (and its Extend sequence) into this cluster.
                cursor = next_codepoint(text, cursor);
            }
            continue;
        }
        if (first_is_ri && is_regional_indicator(cp) && !ri_pair_used) {
            ri_pair_used = true;
            cursor = next_codepoint(text, cursor);
            continue;
        }
        break;
    }
    return cursor;
}

EditOffset prev_cluster(std::string_view text, EditOffset offset) {
    if (offset == 0) {
        return 0;
    }
    if (offset > text.size()) {
        offset = text.size();
    }
    if (const std::optional<EditOffset> icu = grapheme_preceding(text, offset)) {
        return *icu;
    }
    EditOffset cursor = 0;
    EditOffset previous = 0;
    while (cursor < offset) {
        previous = cursor;
        const EditOffset next = builtin_next_cluster(text, cursor);
        if (next >= offset) {
            return previous;
        }
        cursor = next;
    }
    return previous;
}

EditOffset snap_to_cluster(std::string_view text, EditOffset offset) {
    if (offset == 0) {
        return 0;
    }
    if (offset >= text.size()) {
        return text.size();
    }
    if (const std::optional<EditOffset> icu = grapheme_snap(text, offset)) {
        return *icu;
    }
    EditOffset cursor = 0;
    while (cursor < text.size()) {
        if (offset == cursor) {
            return cursor;
        }
        const EditOffset next = builtin_next_cluster(text, cursor);
        if (offset < next) {
            return cursor;
        }
        cursor = next;
    }
    return text.size();
}

namespace {

// Index of the word-set segment containing `offset` (the last segment when offset == size).
std::size_t segment_at(const BreakBoundaries& boundaries, EditOffset offset) {
    std::size_t index = 0;
    while (index + 2 < boundaries.offsets.size() && boundaries.offsets[index + 1] <= offset) {
        ++index;
    }
    return index;
}

} // namespace

EditOffset word_left(std::string_view text, EditOffset offset) {
    const EditOffset limit = offset > text.size() ? text.size() : offset;
    const WordSet set = word_set(text);
    const BreakBoundaries& boundaries = set.boundaries;
    const std::vector<bool>& is_word = set.is_word;
    // The start of the nearest word segment before `limit`: the caret skips the whitespace or
    // punctuation run and lands on the previous word.
    for (std::size_t index = is_word.size(); index-- > 0;) {
        const EditOffset at = boundaries.offsets[index];
        if (at < limit && (at == 0 || is_word[index])) {
            return at;
        }
    }
    return 0;
}

EditOffset word_right(std::string_view text, EditOffset offset) {
    if (offset >= text.size()) {
        return text.size();
    }
    const WordSet set = word_set(text);
    const BreakBoundaries& boundaries = set.boundaries;
    const std::vector<bool>& is_word = set.is_word;
    if (is_word.empty()) {
        return text.size();
    }
    // Inside a word the caret moves to the end of that word; otherwise it skips the non-word run
    // and lands on the end of the next word.
    for (std::size_t index = segment_at(boundaries, offset); index < is_word.size(); ++index) {
        if (is_word[index]) {
            return boundaries.offsets[index + 1];
        }
    }
    return text.size();
}

bool is_word_codepoint(std::uint32_t cp) {
    if (cp < 0x80) {
        return (cp >= 'a' && cp <= 'z') || (cp >= 'A' && cp <= 'Z') || (cp >= '0' && cp <= '9') ||
               cp == '_';
    }
    // Every non-ASCII code point counts as a word code point (the documented reduced rule).
    return true;
}

EditOffset line_start(std::string_view text, EditOffset offset) {
    EditOffset cursor = offset > text.size() ? text.size() : offset;
    while (cursor > 0) {
        if (text[cursor - 1] == '\n') {
            return cursor;
        }
        --cursor;
    }
    return 0;
}

EditOffset line_end(std::string_view text, EditOffset offset) {
    EditOffset cursor = offset > text.size() ? text.size() : offset;
    while (cursor < text.size()) {
        if (text[cursor] == '\n') {
            return cursor;
        }
        ++cursor;
    }
    return text.size();
}

std::uint32_t count_codepoints(std::string_view text, EditOffset end) {
    const EditOffset limit = end > text.size() ? text.size() : end;
    std::uint32_t count = 0;
    EditOffset cursor = 0;
    while (cursor < limit) {
        cursor = next_codepoint(text, cursor);
        ++count;
    }
    return count;
}

std::uint32_t count_graphemes(std::string_view text, EditOffset end) {
    const EditOffset limit = end > text.size() ? text.size() : end;
    std::uint32_t count = 0;
    EditOffset cursor = 0;
    while (cursor < limit) {
        cursor = next_cluster(text, cursor);
        ++count;
    }
    return count;
}

} // namespace tessera::ui::text::edit_detail
