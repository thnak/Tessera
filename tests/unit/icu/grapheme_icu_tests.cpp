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
    if (failures != 0) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "grapheme icu tests passed\n";
    return 0;
}
