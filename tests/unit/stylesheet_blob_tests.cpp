// SPDX-License-Identifier: MIT

#include "tessera/ui/style/compiled_stylesheet.hpp"
#include "tessera/ui/style/stylesheet_blob.hpp"

#include <cstdint>
#include <iostream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace tessera::ui::style;

bool g_ok = true;
constexpr std::size_t kHeaderBytes = 24;

void expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "stylesheet_blob: " << message << "\n";
        g_ok = false;
    }
}

// Covers every prepared item kind: selectors with nesting, attributes, pseudo-classes, media,
// container, layers (named, anonymous, statement), supports, keyframes and shorthand expansion.
constexpr std::string_view kCorpus[] = {
    "button { color: red; margin: 1px 2px; }\n.a > .b:hover, #c[x=\"y\" i] { width: 10px }\n",
    "@layer base, theme;\n@layer base { .x { color: blue } }\n@layer { .y { color: green } }\n"
    ".z { color: black }\n",
    "@media (min-width: 800px) { .wide { width: 800px } @layer late { .l { color: red } } }\n"
    "@media (prefers-color-scheme: dark) { .t { color: white } }\n"
    "@layer late { .m { color: tan } }\n.t { color: black }\n",
    "@container card (min-width: 300px) { .c { padding: 4px } }\n"
    "@container (width >= 100px) and (height < 50px) { .d { gap: 2px } }\n",
    "@supports (display: flex) { .f { display: flex } }\n"
    "@supports (not-a-property: 1) { .g { color: red } }\n"
    "@media (min-width: 10px) { @keyframes spin { from { opacity: 0 } to { opacity: 1 } } }\n"
    "@keyframes fade { 0% { opacity: 1 } 50% { opacity: .5 } 100% { opacity: 0 } }\n",
    ".n:not(.a, .b) { color: red } .p::part(label) { color: blue } .q { not-real: 1; color: red }\n"
    "@media (bogus {{ .never { color: red } }\n",
    "",
};

std::vector<std::uint8_t> blob_of(std::string_view css) {
    return serialize_stylesheet(prepare_stylesheet(css));
}

bool same_rules(const CompiledStyleSheet& lhs, const CompiledStyleSheet& rhs) {
    if (lhs.rules().size() != rhs.rules().size() ||
        lhs.keyframes().size() != rhs.keyframes().size() ||
        lhs.layer_order() != rhs.layer_order() ||
        lhs.container_queries().size() != rhs.container_queries().size() ||
        lhs.has_container_queries() != rhs.has_container_queries() ||
        lhs.diagnostics().size() != rhs.diagnostics().size() ||
        lhs.index().size() != rhs.index().size()) {
        return false;
    }
    for (std::size_t i = 0; i < lhs.rules().size(); ++i) {
        const CompiledRule& a = lhs.rules()[i];
        const CompiledRule& b = rhs.rules()[i];
        if (a.source_order != b.source_order || a.layer != b.layer ||
            a.declarations.size() != b.declarations.size() ||
            a.containers.size() != b.containers.size()) {
            return false;
        }
        for (std::size_t d = 0; d < a.declarations.size(); ++d) {
            if (a.declarations[d].property != b.declarations[d].property ||
                a.declarations[d].value != b.declarations[d].value ||
                a.declarations[d].important != b.declarations[d].important) {
                return false;
            }
        }
    }
    return true;
}

void round_trip_matches_direct_compile() {
    const MediaContext contexts[] = {
        MediaContext{},
        MediaContext{500.0, 400.0, 96.0, ColorScheme::Dark, true},
        MediaContext{1200.0, 900.0, 192.0, ColorScheme::Dark, false},
    };
    for (const std::string_view css : kCorpus) {
        const std::vector<std::uint8_t> bytes = blob_of(css);
        const auto loaded = load_stylesheet_blob(bytes, fnv1a64(css));
        expect(loaded.has_value(), "blob loads: " + std::string(css.substr(0, 24)));
        if (!loaded) {
            continue;
        }
        expect(serialize_stylesheet(*loaded) == bytes,
               "serialize(load(blob)) is byte-identical: " + std::string(css.substr(0, 24)));
        for (const MediaContext& media : contexts) {
            const CompiledStyleSheet direct =
                compile_stylesheet(css, Origin::Author, PropertyRegistry{}, media);
            const CompiledStyleSheet from_blob(*loaded, Origin::Author, PropertyRegistry{}, media);
            expect(same_rules(direct, from_blob),
                   "blob resolve equals compile_stylesheet: " + std::string(css.substr(0, 24)));
        }
    }
}

void output_is_deterministic() {
    for (const std::string_view css : kCorpus) {
        expect(blob_of(css) == blob_of(css), "two serializations are identical");
    }
    // Byte-level regression guard for the v1 layout of a small sheet.
    const std::vector<std::uint8_t> bytes = blob_of(".a { color: red }");
    expect(bytes.size() == 135, "v1 layout size of the reference sheet is 135 bytes, got " +
                                    std::to_string(bytes.size()));
    expect(std::string_view(reinterpret_cast<const char*>(bytes.data()), 4) == "TSSB",
           "blob starts with the magic");
}

void media_is_resolved_at_load_not_baked() {
    const std::string_view css =
        "@media (min-width: 800px) { .a { color: red } } .b { color: blue }";
    const PreparedStyleSheet prepared = prepare_stylesheet(css);
    const CompiledStyleSheet narrow(prepared, Origin::Author, PropertyRegistry{},
                                    MediaContext{400.0, 300.0});
    const CompiledStyleSheet wide(prepared, Origin::Author, PropertyRegistry{},
                                  MediaContext{1000.0, 300.0});
    expect(narrow.rules().size() == 1, "narrow viewport drops the media rule");
    expect(wide.rules().size() == 2,
           "wide viewport keeps the media rule from the same prepared data");
}

void layers_in_false_media_take_no_slot() {
    const std::string_view css = "@media (min-width: 5000px) { @layer a { .x { color: red } } }\n"
                                 "@layer b { .y { color: red } }\n@layer a { .z { color: red } }\n";
    const CompiledStyleSheet sheet = compile_stylesheet(css);
    expect(sheet.layer_order().at("b") == 0 && sheet.layer_order().at("a") == 1,
           "layer order follows applicable appearances only");
}

void header_errors_are_reported() {
    const std::string_view css = ".a { color: red }";
    const std::vector<std::uint8_t> good = blob_of(css);

    auto code_of = [](const std::vector<std::uint8_t>& bytes, std::optional<std::uint64_t> hash) {
        const auto result = load_stylesheet_blob(bytes, hash);
        return result ? std::optional<BlobErrorCode>{}
                      : std::optional<BlobErrorCode>{result.error().code};
    };

    expect(!code_of(good, fnv1a64(css)).has_value(), "good blob loads");
    expect(code_of(good, fnv1a64(css) + 1) == BlobErrorCode::HashMismatch, "hash mismatch");
    expect(code_of({}, std::nullopt) == BlobErrorCode::Truncated, "empty input");

    std::vector<std::uint8_t> bad_magic = good;
    bad_magic[0] = 'X';
    expect(code_of(bad_magic, std::nullopt) == BlobErrorCode::BadMagic, "bad magic");

    std::vector<std::uint8_t> bad_format = good;
    bad_format[4] = 2;
    expect(code_of(bad_format, std::nullopt) == BlobErrorCode::UnsupportedFormatVersion,
           "format version");

    std::vector<std::uint8_t> bad_schema = good;
    bad_schema[8] = 9;
    expect(code_of(bad_schema, std::nullopt) == BlobErrorCode::EngineSchemaMismatch,
           "engine schema version");

    std::vector<std::uint8_t> trailing = good;
    trailing.push_back(0);
    expect(code_of(trailing, std::nullopt) == BlobErrorCode::Truncated,
           "trailing byte vs payload size");
}

void malformed_input_never_crashes() {
    for (const std::string_view css : kCorpus) {
        const std::vector<std::uint8_t> good = blob_of(css);
        // Every strict prefix must fail cleanly.
        for (std::size_t length = 0; length < good.size(); ++length) {
            const auto result =
                load_stylesheet_blob(std::span<const std::uint8_t>(good.data(), length));
            expect(!result.has_value(), "truncated blob is rejected");
        }
        // Single-byte corruption anywhere in the payload either fails or loads a valid structure;
        // loading must never read out of bounds (checked under the sanitizers/CRT debug heap).
        for (std::size_t i = kHeaderBytes; i < good.size(); ++i) {
            for (const std::uint8_t value :
                 {std::uint8_t{0}, std::uint8_t{0xFF}, std::uint8_t{0x7F}}) {
                std::vector<std::uint8_t> mutated = good;
                mutated[i] = value;
                const auto result = load_stylesheet_blob(mutated);
                if (result) {
                    const CompiledStyleSheet sheet(*result);
                    (void)sheet.rules().size();
                }
            }
        }
    }
}

} // namespace

int main() {
    round_trip_matches_direct_compile();
    output_is_deterministic();
    media_is_resolved_at_load_not_baked();
    layers_in_false_media_take_no_slot();
    header_errors_are_reported();
    malformed_input_never_crashes();
    return g_ok ? 0 : 1;
}
