// SPDX-License-Identifier: MIT
//
// The caret-position memo of EditBuffer (issue #11) must never change a result: after every
// command, caret_position() equals a recount from the start of the buffer. A deterministic
// pseudo-random script mixes inserts of combining marks, ZWJ sequences, flags and CJK with
// deletes, caret jumps and selection replacement.

#include "edit_grapheme.hpp"
#include "tessera/ui/text/edit_buffer.hpp"

#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

namespace {

using tessera::ui::text::EditBuffer;
using tessera::ui::text::EditOffset;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "edit_position_cache: " << message << "\n";
    }
    return condition;
}

bool matches_recount(const EditBuffer& buffer, const char* where) {
    const auto position = buffer.caret_position();
    const bool ok =
        position.bytes == buffer.caret() &&
        position.graphemes ==
            tessera::ui::text::edit_detail::count_graphemes(buffer.text(), buffer.caret()) &&
        position.codepoints ==
            tessera::ui::text::edit_detail::count_codepoints(buffer.text(), buffer.caret());
    if (!ok) {
        std::cerr << "edit_position_cache: mismatch after " << where << ": caret " << position.bytes
                  << " graphemes " << position.graphemes << " (recount "
                  << tessera::ui::text::edit_detail::count_graphemes(buffer.text(), buffer.caret())
                  << ") codepoints " << position.codepoints << " (recount "
                  << tessera::ui::text::edit_detail::count_codepoints(buffer.text(), buffer.caret())
                  << ") text:";
        for (const unsigned char byte : buffer.text()) {
            std::cerr << ' ' << std::hex << static_cast<int>(byte) << std::dec;
        }
        std::cerr << "\n";
    }
    return ok;
}

struct Lcg {
    std::uint32_t state = 12345;
    std::uint32_t next() {
        state = state * 1664525U + 1013904223U;
        return state >> 8;
    }
};

bool random_script() {
    const std::vector<std::string> pieces = {
        "a",
        "bc",
        "\xCC\x81",  // combining acute: extends the cluster before
        "e\xCC\x81", // e + acute
        "\xF0\x9F\x91\xA8\xE2\x80\x8D\xF0\x9F\x91\xA9", // man ZWJ woman
        "\xF0\x9F\x87\xAB\xF0\x9F\x87\xB7",             // flag FR
        "\xF0\x9F\x87\xAB",                             // one regional indicator (flag parity)
        "\xE4\xBD\xA0\xE5\xA5\xBD",                     // CJK
        "\xD7\xA9\xD6\xB8",                             // Hebrew letter + point
        "\n",
        " ",
        "\xE2\x80\x8D", // bare ZWJ
    };
    EditBuffer buffer;
    Lcg rng;
    bool ok = matches_recount(buffer, "construction");
    for (int step = 0; step < 4000 && ok; ++step) {
        switch (rng.next() % 7) {
        case 0:
        case 1:
        case 2:
            buffer.insert(pieces[rng.next() % pieces.size()]);
            ok = matches_recount(buffer, "insert");
            break;
        case 3: {
            const EditOffset size = buffer.text().size();
            buffer.set_caret(size == 0 ? 0 : rng.next() % (size + 1));
            ok = matches_recount(buffer, "set_caret");
            break;
        }
        case 4: {
            const EditOffset size = buffer.text().size();
            if (size != 0) {
                const EditOffset a = rng.next() % (size + 1);
                const EditOffset b = rng.next() % (size + 1);
                buffer.replace_range(a, b, pieces[rng.next() % pieces.size()]);
            }
            ok = matches_recount(buffer, "replace_range");
            break;
        }
        case 5: {
            const EditOffset size = buffer.text().size();
            if (size != 0) {
                const EditOffset a = rng.next() % (size + 1);
                buffer.replace_range(a, a + 1 + rng.next() % 6, "");
            }
            ok = matches_recount(buffer, "delete range");
            break;
        }
        default:
            if (rng.next() % 40 == 0) {
                buffer.set_text(pieces[rng.next() % pieces.size()]);
                ok = matches_recount(buffer, "set_text");
            }
            break;
        }
    }
    return expect(ok, "the memoized position matches a recount through the script");
}

bool large_buffer_typing() {
    EditBuffer buffer;
    std::string big;
    for (int i = 0; i < 6000; ++i) {
        big += "ab\xCC\x81 \xE4\xBD\xA0 ";
    }
    buffer.set_text(big);
    bool ok = matches_recount(buffer, "large set_text");
    for (int i = 0; i < 300 && ok; ++i) {
        buffer.insert(i % 3 == 0 ? "\xCC\x81" : "x");
        ok = matches_recount(buffer, "typing at the end");
    }
    buffer.set_caret(10);
    ok = matches_recount(buffer, "jump back") && ok;
    buffer.insert("\xCC\x81"); // extends the cluster before the caret
    ok = matches_recount(buffer, "mark at the front") && ok;
    return expect(ok, "typing in a large buffer stays exact");
}

} // namespace

int main() {
    bool ok = random_script();
    ok = large_buffer_typing() && ok;
    if (!ok) {
        return 1;
    }
    std::cout << "edit_position_cache: ok\n";
    return 0;
}
