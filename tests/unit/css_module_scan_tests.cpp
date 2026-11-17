// SPDX-License-Identifier: MIT
//
// CSS module scan (issue #463, slice 2b): the path hash, the class/keyframes scoping rewrite and
// the byte-preservation contract. The rewrite is the whole point of the slice, so most cases assert
// on the exact rewritten text rather than on a normalized form.

#include "tessera/compiler/css_module/module_scan.hpp"

#include <algorithm>
#include <cstddef>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

using tessera::compiler::css_module::module_hash;
using tessera::compiler::css_module::ModuleScan;
using tessera::compiler::css_module::ModuleScanError;
using tessera::compiler::css_module::scan_module_css;
using tessera::compiler::css_module::scoped_name;

bool expect(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "css_module_scan: " << message << "\n";
    }
    return condition;
}

std::string replace_all(std::string text, std::string_view from, std::string_view to) {
    std::size_t at = text.find(from);
    while (at != std::string::npos) {
        text.replace(at, from.size(), to);
        at = text.find(from, at + to.size());
    }
    return text;
}

// The hash is a test vector, not a literal in every expectation: the cases below write `<h>` and
// the helper substitutes the real hash, so a hash change fails the hash case alone instead of all
// of them.
std::string with_hash(std::string text, std::string_view hash) {
    return replace_all(std::move(text), "<h>", hash);
}

bool scan_ok(std::string_view path, std::string_view text, ModuleScan& out) {
    auto scanned = scan_module_css(path, text);
    if (!scanned) {
        std::cerr << "css_module_scan: unexpected error " << scanned.error().code << ": "
                  << scanned.error().message << "\n";
        return false;
    }
    out = std::move(*scanned);
    return true;
}

bool scan_fails(std::string_view path, std::string_view text, std::string_view code) {
    auto scanned = scan_module_css(path, text);
    if (scanned) {
        std::cerr << "css_module_scan: expected " << code << ", scan succeeded\n";
        return false;
    }
    if (scanned.error().code != code) {
        std::cerr << "css_module_scan: expected " << code << ", got " << scanned.error().code
                  << "\n";
        return false;
    }
    return !scanned.error().message.empty() &&
           scanned.error().message.find('\n') == std::string::npos;
}

// --- hash ------------------------------------------------------------------------------------

bool hash_vectors() {
    bool ok = expect(module_hash("ui/Card.module.css") == "0b82a5", "hash of ui/Card.module.css");
    ok = expect(module_hash("samples/gallery/form.module.css") == "34762b",
                "hash of samples/gallery/form.module.css") &&
         ok;
    ok = expect(module_hash("ui/sub/Card.module.css") == "76ee60",
                "hash of ui/sub/Card.module.css") &&
         ok;
    ok = expect(module_hash("ui/App.module.css") == "e3039e", "hash of ui/App.module.css") && ok;

    // The same file name in two directories must not collide, or two modules would share a scope.
    ok = expect(module_hash("ui/Card.module.css") != module_hash("ui/sub/Card.module.css"),
                "the directory is part of the hash") &&
         ok;

    const std::string hash = module_hash("ui/Card.module.css");
    ok = expect(hash.size() == 6, "the hash is six characters") && ok;
    ok =
        expect(std::all_of(hash.begin(), hash.end(),
                           [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); }),
               "the hash is lowercase hex") &&
        ok;

    // The input is already POSIX: a backslash path is a different string, not a normalized one.
    ok = expect(module_hash("ui\\Card.module.css") != hash, "a backslash path is not normalized") &&
         ok;

    ok = expect(scoped_name("card", "0b82a5") == "card_0b82a5", "scoped_name joins with `_`") && ok;
    return ok;
}

// --- rename ----------------------------------------------------------------------------------

bool simple_rename() {
    bool ok = true;
    ModuleScan scan;
    if (!scan_ok("ui/Card.module.css", ".card { color: red; }", scan)) {
        return false;
    }
    ok = expect(scan.hash == "0b82a5", "the scan carries the hash") && ok;
    ok = expect(scan.rewritten == with_hash(".card_<h> { color: red; }", scan.hash),
                "a class selector is scoped") &&
         ok;
    ok = expect(scan.classes.size() == 1, "one class is reported") && ok;
    ok = expect(scan.classes.size() == 1 && scan.classes.front().local == "card",
                "the local name is reported") &&
         ok;
    ok = expect(scan.classes.size() == 1 &&
                    scan.classes.front().scoped == scoped_name("card", scan.hash),
                "the scoped name is reported") &&
         ok;
    ok = expect(scan.keyframes.empty(), "no keyframes are reported") && ok;

    // Two classes in one selector, and a descendant combinator.
    ModuleScan nested;
    if (!scan_ok("ui/Card.module.css", ".card .title { color: red; }", nested)) {
        return false;
    }
    ok = expect(nested.rewritten == with_hash(".card_<h> .title_<h> { color: red; }", nested.hash),
                "every class in a selector is scoped") &&
         ok;
    ok = expect(nested.classes.size() == 2, "both classes are reported") && ok;
    return ok;
}

bool selector_versus_value() {
    bool ok = true;
    const std::string text = ".x { content: \".x\"; }\n.y { content: '.y'; }";
    ModuleScan scan;
    if (!scan_ok("ui/Card.module.css", text, scan)) {
        return false;
    }
    ok = expect(scan.rewritten ==
                    with_hash(".x_<h> { content: \".x\"; }\n.y_<h> { content: '.y'; }", scan.hash),
                "a class inside a string value is untouched") &&
         ok;
    ok = expect(scan.classes.size() == 2, "only the two selectors are reported") && ok;

    // A `.` in a value that is not a string is still not a selector.
    ModuleScan numeric;
    if (!scan_ok("ui/Card.module.css", ".x { margin: 0.5em; }", numeric)) {
        return false;
    }
    ok = expect(numeric.rewritten == with_hash(".x_<h> { margin: 0.5em; }", numeric.hash),
                "a decimal in a value is untouched") &&
         ok;
    return ok;
}

bool global_wrapper() {
    bool ok = true;
    ModuleScan scan;
    if (!scan_ok("ui/Card.module.css", ":global(.reset) .card { color: red; }", scan)) {
        return false;
    }
    ok = expect(scan.rewritten == with_hash(".reset .card_<h> { color: red; }", scan.hash),
                ":global( ) drops the wrapper and keeps the class") &&
         ok;
    ok = expect(scan.classes.size() == 1 && scan.classes.front().local == "card",
                "the global class is not reported") &&
         ok;

    // Everything inside the parens is unrenamed, including a compound selector.
    ModuleScan compound;
    if (!scan_ok("ui/Card.module.css", ":global(.a .b) .c { }", compound)) {
        return false;
    }
    ok = expect(compound.rewritten == with_hash(".a .b .c_<h> { }", compound.hash),
                "a compound :global( ) is unrenamed") &&
         ok;
    ok = expect(compound.classes.size() == 1, "only the local class is reported") && ok;

    // A `:global` that is not a function is the block form, which this slice does not support.
    ok = expect(scan_fails("ui/Card.module.css", ":global { .card { } }",
                           "TESSERA_CSS_MODULE_GLOBAL_BLOCK"),
                ":global { } reports GLOBAL_BLOCK") &&
         ok;
    ok = expect(scan_fails("ui/Card.module.css", ":global(.reset .card { }",
                           "TESSERA_CSS_MODULE_MALFORMED_GLOBAL"),
                "an unclosed :global( reports MALFORMED_GLOBAL") &&
         ok;
    return ok;
}

bool keyframes() {
    bool ok = true;
    const std::string text = "@keyframes pulse { from { opacity: 0; } to { opacity: 1; } }\n"
                             ".card { animation: pulse 1s ease; }\n"
                             ".spin { animation: spin 1s; }\n"
                             ".named { animation-name: pulse; }";
    ModuleScan scan;
    if (!scan_ok("ui/Card.module.css", text, scan)) {
        return false;
    }
    const std::string expected =
        "@keyframes pulse_<h> { from { opacity: 0; } to { opacity: 1; } }\n"
        ".card_<h> { animation: pulse_<h> 1s ease; }\n"
        ".spin_<h> { animation: spin 1s; }\n"
        ".named_<h> { animation-name: pulse_<h>; }";
    ok = expect(scan.rewritten == with_hash(expected, scan.hash),
                "keyframes and their references are scoped, a foreign name is not") &&
         ok;
    ok = expect(scan.keyframes.size() == 1 && scan.keyframes.front().local == "pulse",
                "one keyframes block is reported") &&
         ok;
    ok = expect(scan.keyframes.size() == 1 &&
                    scan.keyframes.front().scoped == scoped_name("pulse", scan.hash),
                "the keyframes scoped name is reported") &&
         ok;
    ok = expect(scan.classes.size() == 3, "three classes are reported") && ok;

    // A reference before the definition still resolves: the names are collected up front.
    ModuleScan forward;
    if (!scan_ok("ui/Card.module.css", ".a { animation-name: pulse; }\n@keyframes pulse { }",
                 forward)) {
        return false;
    }
    ok = expect(forward.rewritten ==
                    with_hash(".a_<h> { animation-name: pulse_<h>; }\n@keyframes pulse_<h> { }",
                              forward.hash),
                "a forward reference is scoped") &&
         ok;
    return ok;
}

bool keyframes_body_preludes() {
    bool ok = true;
    const std::string text = "@keyframes pulse {\n"
                             "  from { opacity: 0; }\n"
                             "  to { opacity: 1; }\n"
                             "  50% { opacity: 0.5; }\n"
                             "}";
    ModuleScan scan;
    if (!scan_ok("ui/Card.module.css", text, scan)) {
        return false;
    }
    ok = expect(scan.rewritten == with_hash(replace_all(text, "pulse", "pulse_<h>"), scan.hash),
                "from/to/50% preludes are not class selectors") &&
         ok;
    ok = expect(scan.classes.empty(), "no class is reported from a keyframes body") && ok;
    return ok;
}

bool nested_at_rules() {
    bool ok = true;
    const std::string text = "@media (min-width: 600px) { .card { color: red; } }\n"
                             "@supports (display: grid) { .grid { display: grid; } }\n"
                             "@layer base { .layered { color: blue; } }\n"
                             "@container (min-width: 20rem) { .box { color: green; } }";
    ModuleScan scan;
    if (!scan_ok("ui/Card.module.css", text, scan)) {
        return false;
    }
    const std::string expected = "@media (min-width: 600px) { .card_<h> { color: red; } }\n"
                                 "@supports (display: grid) { .grid_<h> { display: grid; } }\n"
                                 "@layer base { .layered_<h> { color: blue; } }\n"
                                 "@container (min-width: 20rem) { .box_<h> { color: green; } }";
    ok = expect(scan.rewritten == with_hash(expected, scan.hash),
                "a class inside a grouping at-rule is scoped") &&
         ok;
    ok = expect(scan.classes.size() == 4, "all four nested classes are reported") && ok;

    // A declaration-only at-rule is not a selector context.
    ModuleScan font_face;
    if (!scan_ok("ui/Card.module.css",
                 "@font-face { font-family: \"My.face\"; src: url(a.woff2); }", font_face)) {
        return false;
    }
    ok =
        expect(font_face.rewritten == "@font-face { font-family: \"My.face\"; src: url(a.woff2); }",
               "@font-face is untouched") &&
        ok;
    ok = expect(font_face.classes.empty(), "@font-face reports no class") && ok;
    return ok;
}

bool never_renamed() {
    bool ok = true;
    const std::string text = "#id { color: red; }\n"
                             "[data-x=\"1\"] { color: red; }\n"
                             "div { color: red; }\n"
                             "::part(label) { color: red; }\n"
                             ":root { --custom-property: 1px; }\n"
                             "@layer tokens;\n"
                             ".card { --local: 2px; color: var(--local); }";
    ModuleScan scan;
    if (!scan_ok("ui/Card.module.css", text, scan)) {
        return false;
    }
    const std::string expected = "#id { color: red; }\n"
                                 "[data-x=\"1\"] { color: red; }\n"
                                 "div { color: red; }\n"
                                 "::part(label) { color: red; }\n"
                                 ":root { --custom-property: 1px; }\n"
                                 "@layer tokens;\n"
                                 ".card_<h> { --local: 2px; color: var(--local); }";
    ok = expect(scan.rewritten == with_hash(expected, scan.hash),
                "ids, attributes, types, ::part, custom properties and @layer are untouched") &&
         ok;
    ok = expect(scan.classes.size() == 1, "only the one class is reported") && ok;
    return ok;
}

bool byte_preservation() {
    bool ok = true;
    const std::string text = "/* leading comment,   odd   spacing */\n"
                             "\n"
                             ".card   ,\n"
                             ".card:hover\t{\n"
                             "    color :  red ;   /* trailing */\n"
                             "}\n"
                             "\n"
                             "/* between */\n"
                             "@media (min-width:600px){\n"
                             "\t.card{color:blue}\n"
                             "}\n";
    ModuleScan scan;
    if (!scan_ok("ui/Card.module.css", text, scan)) {
        return false;
    }
    // The only difference must be the inserted `_<hash>` suffixes: strip them and the input
    // returns.
    std::string stripped = scan.rewritten;
    const std::string suffix = "_" + scan.hash;
    std::size_t at = stripped.find(suffix);
    while (at != std::string::npos) {
        stripped.erase(at, suffix.size());
        at = stripped.find(suffix, at);
    }
    ok = expect(stripped == text, "the rewrite differs only at the renamed spans") && ok;
    ok = expect(scan.rewritten.size() == text.size() + 3 * suffix.size(),
                "exactly three class spans were renamed") &&
         ok;
    ok = expect(scan.rewritten.find("/* leading comment,   odd   spacing */") != std::string::npos,
                "comments survive verbatim") &&
         ok;
    return ok;
}

bool errors() {
    bool ok = true;
    ok = expect(scan_fails("ui/Card.module.css", ".card { color: red;",
                           "TESSERA_CSS_MODULE_MALFORMED"),
                "an unclosed block reports MALFORMED") &&
         ok;
    ok = expect(scan_fails("ui/Card.module.css", ".card { color: red; } }",
                           "TESSERA_CSS_MODULE_MALFORMED"),
                "a stray `}` reports MALFORMED") &&
         ok;
    ok = expect(scan_fails("ui/Card.module.css", ".card", "TESSERA_CSS_MODULE_MALFORMED"),
                "a selector with no block reports MALFORMED") &&
         ok;

    // The error names the offending construct and carries an offset into the source.
    auto scanned = scan_module_css("ui/Card.module.css", ".card { color: red;");
    ok = expect(scanned.has_value() == false, "the malformed case fails") && ok;
    if (!scanned) {
        ok = expect(scanned.error().offset <= std::string_view(".card { color: red;").size(),
                    "the error offset is inside the source") &&
             ok;
        ok = expect(scanned.error().message.find("brace") != std::string::npos,
                    "the message names the construct") &&
             ok;
    }
    return ok;
}

bool determinism() {
    bool ok = true;
    const std::string text =
        ".b { color: red; }\n.a { color: blue; }\n.b { color: green; }\n"
        "@keyframes z { from { opacity: 0; } }\n@keyframes a { to { opacity: 1; } }";
    ModuleScan first;
    ModuleScan second;
    if (!scan_ok("ui/Card.module.css", text, first) ||
        !scan_ok("ui/Card.module.css", text, second)) {
        return false;
    }
    ok = expect(first.rewritten == second.rewritten, "the same input scans identically") && ok;
    ok = expect(first.classes.size() == second.classes.size(), "the class list is stable") && ok;

    ok = expect(first.classes.size() == 2, "duplicate classes collapse") && ok;
    ok = expect(std::is_sorted(first.classes.begin(), first.classes.end(),
                               [](const auto& a, const auto& b) { return a.local < b.local; }),
                "classes are sorted by local name") &&
         ok;
    ok = expect(first.classes.size() == 2 && first.classes[0].local == "a" &&
                    first.classes[1].local == "b",
                "classes are unique and ordered") &&
         ok;
    ok = expect(first.keyframes.size() == 2 && first.keyframes[0].local == "a" &&
                    first.keyframes[1].local == "z",
                "keyframes are sorted and unique") &&
         ok;
    return ok;
}

} // namespace

int main() {
    bool ok = hash_vectors();
    ok = simple_rename() && ok;
    ok = selector_versus_value() && ok;
    ok = global_wrapper() && ok;
    ok = keyframes() && ok;
    ok = keyframes_body_preludes() && ok;
    ok = nested_at_rules() && ok;
    ok = never_renamed() && ok;
    ok = byte_preservation() && ok;
    ok = errors() && ok;
    ok = determinism() && ok;
    if (!ok) {
        std::cerr << "css_module_scan: FAILED\n";
        return 1;
    }
    std::cout << "css_module_scan: ok\n";
    return 0;
}
