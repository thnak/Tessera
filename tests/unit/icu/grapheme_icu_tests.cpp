// SPDX-License-Identifier: MIT

// ICU-backed grapheme-cluster caret movement (issue #11): marks the reduced builtin Extend table
// misses stay attached to their base, and the builtin rule is still the documented fallback.

#include "edit_grapheme.hpp"
#include "text_breaks.hpp"

#include <iostream>
#include <string>
#include <string_view>

namespace {

using tessera::ui::text::edit_detail::builtin_next_cluster;
using tessera::ui::text::edit_detail::next_cluster;
using tessera::ui::text::edit_detail::prev_cluster;
using tessera::ui::text::edit_detail::snap_to_cluster;
using tessera::ui::text::edit_detail::word_left;
using tessera::ui::text::edit_detail::word_right;

int failures = 0;

void check(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << "\n";
        ++failures;
    }
}

struct Fixture {
    const char* name;
    std::string_view text;     // one letter plus its mark(s), then 'x'
    std::size_t cluster_bytes; // expected size of the first cluster
};

} // namespace

int main() {
    const Fixture fixtures[] = {
        {"hebrew point", "\xD7\x90\xD6\xB8x", 4},              // U+05D0 U+05B8
        {"arabic harakat", "\xD8\xA8\xD9\x8Fx", 4},            // U+0628 U+064F
        {"arabic dagger alef", "\xD8\xA8\xD9\xB0x", 4},        // U+0628 U+0670
        {"thai above vowel", "\xE0\xB8\x81\xE0\xB8\xB4x", 6},  // U+0E01 U+0E34
        {"thai tone mark", "\xE0\xB8\x81\xE0\xB9\x88x", 6},    // U+0E01 U+0E48
        {"kana voicing mark", "\xE3\x81\x8B\xE3\x82\x99x", 6}, // U+304B U+3099
        {"crlf", "\r\nx", 2},
        {"zwj emoji family", "\xF0\x9F\x91\xA9\xE2\x80\x8D\xF0\x9F\x91\xA7x", 11},
        {"flag pair", "\xF0\x9F\x87\xAB\xF0\x9F\x87\xB7x", 8},
    };
    const bool icu = tessera::ui::text::active_backend() == tessera::ui::text::BreakBackend::Icu;
    for (const Fixture& fixture : fixtures) {
        const std::string name = fixture.name;
        const std::string_view text = fixture.text;
        const std::size_t end = fixture.cluster_bytes;
        check(next_cluster(text, 0) == end, name + ": next_cluster keeps the mark on its base");
        check(prev_cluster(text, end) == 0, name + ": prev_cluster steps over the whole cluster");
        check(snap_to_cluster(text, end - 1) == 0, name + ": a mid-cluster offset snaps back");
        check(snap_to_cluster(text, end) == end, name + ": a boundary stays put");
        if (!icu) {
            continue;
        }
    }
    if (icu) {
        // The reduced builtin table lacks these ranges: that gap is what the ICU backend closes.
        check(builtin_next_cluster(fixtures[0].text, 0) == 2, "builtin splits a Hebrew point");
        check(builtin_next_cluster(fixtures[3].text, 0) == 3, "builtin splits a Thai vowel");
    }
    if (icu) {
        // Non-ASCII spaces and punctuation are not word characters under the ICU rule status.
        const std::string_view nbsp = "foo\xC2\xA0"
                                      "bar"; // U+00A0
        check(word_right(nbsp, 0) == 3, "word_right stops before a no-break space");
        check(word_right(nbsp, 3) == 8, "word_right skips a no-break space to the next word end");
        check(word_left(nbsp, 8) == 5, "word_left lands on the word after a no-break space");
        const std::string_view dash = "foo\xE2\x80\x94"
                                      "bar"; // U+2014 em dash
        check(word_right(dash, 0) == 3, "word_right stops before an em dash");
        check(word_left(dash, 9) == 6, "word_left skips an em dash");
        const std::string_view ideographic = "a\xE3\x80\x80"
                                             "b"; // U+3000
        check(word_right(ideographic, 0) == 1, "word_right stops before an ideographic space");
        check(word_left(ideographic, 5) == 4, "word_left skips an ideographic space");
        const std::string_view cjk = "\xE4\xBB\x8A\xE5\xA4\xA9\xE5\xA5\xBD"; // CJK run
        check(word_right(cjk, 0) < cjk.size(), "a CJK run is segmented into more than one word");
    }
    if (failures != 0) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "grapheme icu tests passed\n";
    return 0;
}
