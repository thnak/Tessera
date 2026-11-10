// SPDX-License-Identifier: MIT

#include "tessera/ui/text/bidi.hpp"
#include "tessera/ui/text/text_layout.hpp"

#include <SheenBidi/SheenBidi.h>
#include <algorithm>
#include <cstddef>
#include <utility>
#include <vector>

namespace tessera::ui::text {
namespace {

struct Utf16Text {
    std::vector<SBUInt16> units;
    // unit_to_byte[i] is the byte offset of UTF-16 unit i; the last entry is text.size().
    std::vector<std::size_t> unit_to_byte;
};

Utf16Text to_utf16(std::string_view utf8) {
    Utf16Text result;
    result.unit_to_byte.reserve(utf8.size() + 1);
    std::size_t index = 0;
    while (index < utf8.size()) {
        const auto lead = static_cast<unsigned char>(utf8[index]);
        std::uint32_t codepoint = 0;
        std::size_t length = 1;
        if (lead < 0x80) {
            codepoint = lead;
        } else if ((lead & 0xE0) == 0xC0 && index + 1 < utf8.size()) {
            codepoint = ((lead & 0x1F) << 6) | (static_cast<unsigned char>(utf8[index + 1]) & 0x3F);
            length = 2;
        } else if ((lead & 0xF0) == 0xE0 && index + 2 < utf8.size()) {
            codepoint = ((lead & 0x0F) << 12) |
                        ((static_cast<unsigned char>(utf8[index + 1]) & 0x3F) << 6) |
                        (static_cast<unsigned char>(utf8[index + 2]) & 0x3F);
            length = 3;
        } else if ((lead & 0xF8) == 0xF0 && index + 3 < utf8.size()) {
            codepoint = ((lead & 0x07) << 18) |
                        ((static_cast<unsigned char>(utf8[index + 1]) & 0x3F) << 12) |
                        ((static_cast<unsigned char>(utf8[index + 2]) & 0x3F) << 6) |
                        (static_cast<unsigned char>(utf8[index + 3]) & 0x3F);
            length = 4;
        } else {
            codepoint = 0xFFFD; // invalid sequence: replacement character, one byte consumed
            length = 1;
        }
        if (codepoint >= 0x10000) {
            codepoint -= 0x10000;
            const SBUInt16 high = static_cast<SBUInt16>(0xD800 + (codepoint >> 10));
            const SBUInt16 low = static_cast<SBUInt16>(0xDC00 + (codepoint & 0x3FF));
            result.units.push_back(high);
            result.unit_to_byte.push_back(index);
            result.units.push_back(low);
            result.unit_to_byte.push_back(index);
        } else {
            result.units.push_back(static_cast<SBUInt16>(codepoint));
            result.unit_to_byte.push_back(index);
        }
        index += length;
    }
    result.unit_to_byte.push_back(utf8.size());
    return result;
}

struct Algorithm {
    SBAlgorithmRef algorithm = nullptr;
    SBParagraphRef paragraph = nullptr;
    Utf16Text text;

    Algorithm(std::string_view utf8, TextDirection direction) {
        text = to_utf16(utf8);
        if (text.units.empty()) {
            return;
        }
        SBCodepointSequence sequence{SBStringEncodingUTF16, text.units.data(),
                                     static_cast<SBUInteger>(text.units.size())};
        algorithm = SBAlgorithmCreate(&sequence);
        if (algorithm != nullptr) {
            const SBLevel base =
                direction == TextDirection::RightToLeft ? SBLevelDefaultRTL : SBLevelDefaultLTR;
            paragraph = SBAlgorithmCreateParagraph(
                algorithm, 0, static_cast<SBUInteger>(text.units.size()), base);
        }
    }
    ~Algorithm() {
        if (paragraph != nullptr) {
            SBParagraphRelease(paragraph);
        }
        if (algorithm != nullptr) {
            SBAlgorithmRelease(algorithm);
        }
    }
    Algorithm(const Algorithm&) = delete;
    Algorithm& operator=(const Algorithm&) = delete;

    [[nodiscard]] std::size_t byte_offset(SBUInteger unit) const {
        return text.unit_to_byte[std::min<std::size_t>(unit, text.unit_to_byte.size() - 1)];
    }
};

} // namespace

TextDirection bidi_paragraph_direction(std::string_view utf8) {
    const Utf16Text text = to_utf16(utf8);
    if (text.units.empty()) {
        return TextDirection::LeftToRight;
    }
    SBCodepointSequence sequence{SBStringEncodingUTF16, text.units.data(),
                                 static_cast<SBUInteger>(text.units.size())};
    SBAlgorithmRef algorithm = SBAlgorithmCreate(&sequence);
    if (algorithm == nullptr) {
        return TextDirection::LeftToRight;
    }
    TextDirection result = TextDirection::LeftToRight;
    const SBBidiType* types = SBAlgorithmGetBidiTypesPtr(algorithm);
    if (types != nullptr) {
        for (SBUInteger index = 0; index < text.units.size(); ++index) {
            if (types[index] == SBBidiTypeR || types[index] == SBBidiTypeAL) {
                result = TextDirection::RightToLeft;
                break;
            }
            if (types[index] == SBBidiTypeL) {
                break;
            }
        }
    }
    SBAlgorithmRelease(algorithm);
    return result;
}

namespace {

// Resolves the visual runs of every line that lies inside one paragraph. `base` is the byte offset
// of the paragraph in the full text; lines and runs keep full-text offsets, SheenBidi sees only the
// paragraph (it would otherwise stop at the first separator and ignore the rest).
void apply_paragraph_runs(std::string_view paragraph, std::size_t base,
                          TextDirection fallback_direction, std::vector<TextLine>& lines,
                          std::size_t first_line, std::size_t last_line) {
    Algorithm resolved(paragraph, fallback_direction);
    if (resolved.paragraph == nullptr) {
        return;
    }
    const auto unit_for = [&](std::size_t byte) {
        return static_cast<std::size_t>(
            std::distance(resolved.text.unit_to_byte.begin(),
                          std::lower_bound(resolved.text.unit_to_byte.begin(),
                                           resolved.text.unit_to_byte.end(), byte)));
    };
    for (std::size_t line_index = first_line; line_index < last_line; ++line_index) {
        TextLine& line = lines[line_index];
        const std::size_t begin_unit = unit_for(line.begin - base);
        const std::size_t end_unit = unit_for(line.end - base);
        if (begin_unit >= end_unit || begin_unit >= resolved.text.units.size()) {
            continue;
        }
        SBLineRef sb_line =
            SBParagraphCreateLine(resolved.paragraph, static_cast<SBUInteger>(begin_unit),
                                  static_cast<SBUInteger>(end_unit - begin_unit));
        if (sb_line == nullptr) {
            continue;
        }
        const SBUInteger run_count = SBLineGetRunCount(sb_line);
        const SBRun* runs = SBLineGetRunsPtr(sb_line);
        bool needs_visual_order = false;
        std::size_t previous_begin = 0;
        for (SBUInteger index = 0; index < run_count; ++index) {
            const std::size_t run_begin = base + resolved.byte_offset(runs[index].offset);
            const std::size_t run_end =
                base + resolved.byte_offset(runs[index].offset + runs[index].length);
            line.visual_runs.push_back(BidiVisualRun{run_begin, run_end, runs[index].level});
            if ((runs[index].level & 1U) != 0U || run_begin < previous_begin) {
                needs_visual_order = true;
            }
            previous_begin = run_begin;
        }
        SBLineRelease(sb_line);
        if (!needs_visual_order) {
            line.visual_runs.clear();
        }
    }
}

} // namespace

void bidi_apply_visual_runs(std::string_view utf8, TextDirection paragraph_direction,
                            std::vector<TextLine>& lines) {
    for (TextLine& line : lines) {
        line.visual_runs.clear();
    }
    if (lines.empty()) {
        return;
    }
    // Each newline-separated paragraph is resolved on its own, so a Hebrew line after a Latin one
    // gets its own direction and runs. Lines are in text order, so one forward cursor assigns them.
    std::size_t line_cursor = 0;
    std::size_t paragraph_begin = 0;
    while (paragraph_begin <= utf8.size() && line_cursor < lines.size()) {
        const std::size_t newline = utf8.find('\n', paragraph_begin);
        const std::size_t paragraph_end = newline == std::string_view::npos ? utf8.size() : newline;
        const std::size_t first_line = line_cursor;
        while (line_cursor < lines.size() && lines[line_cursor].begin <= paragraph_end &&
               lines[line_cursor].end <= paragraph_end) {
            ++line_cursor;
        }
        if (line_cursor > first_line) {
            apply_paragraph_runs(utf8.substr(paragraph_begin, paragraph_end - paragraph_begin),
                                 paragraph_begin, paragraph_direction, lines, first_line,
                                 line_cursor);
        }
        if (newline == std::string_view::npos) {
            break;
        }
        paragraph_begin = newline + 1;
    }
}

float bidi_offset_x(std::string_view utf8, const TextLine& line, std::size_t logical_offset,
                    const TextStyle& style, BidiAdvance advance, void* user) {
    if (line.visual_runs.empty()) {
        const std::size_t clamped = std::min(std::max(logical_offset, line.begin), line.end);
        return advance(utf8.substr(line.begin, clamped - line.begin), style, user);
    }
    float x = 0.0f;
    float total = 0.0f;
    std::vector<float> widths;
    widths.reserve(line.visual_runs.size());
    for (const BidiVisualRun& run : line.visual_runs) {
        const float width = advance(utf8.substr(run.begin, run.end - run.begin), style, user);
        widths.push_back(width);
        total += width;
    }
    const std::size_t clamped = std::min(std::max(logical_offset, line.begin), line.end);
    std::size_t chosen = 0;
    if (clamped == line.begin) {
        chosen = 0;
        for (std::size_t index = 0; index < line.visual_runs.size(); ++index) {
            if (line.visual_runs[index].begin == line.begin) {
                chosen = index;
                break;
            }
        }
    } else {
        // Right-biased affinity: the run whose logical range ends at the offset.
        for (std::size_t index = 0; index < line.visual_runs.size(); ++index) {
            const BidiVisualRun& run = line.visual_runs[index];
            if (clamped > run.begin && clamped <= run.end) {
                chosen = index;
                break;
            }
        }
    }
    for (std::size_t index = 0; index < chosen; ++index) {
        x += widths[index];
    }
    const BidiVisualRun& run = line.visual_runs[chosen];
    const std::size_t local = std::min(std::max(clamped, run.begin), run.end) - run.begin;
    if ((run.level & 1U) == 0U) {
        x += advance(utf8.substr(run.begin, local), style, user);
    } else {
        // Odd (RTL) run: logical offset 0 is the visual right edge, so subtract the prefix width.
        x += widths[chosen] - advance(utf8.substr(run.begin, local), style, user);
    }
    (void)total;
    return x;
}

} // namespace tessera::ui::text
