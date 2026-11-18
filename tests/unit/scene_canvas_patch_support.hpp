// SPDX-License-Identifier: MIT

#pragma once

// Shared fixtures for the `source.patch` tests (issue #171, designer-protocol-v1 section 10.2).
//
// **Why a header and not a copy in each test file.** The two suites split by responsibility - the
// fixture suite asserts the builder's byte ranges with no tree, the session suite asserts the IR
// span round trip through a real evaluation - but they must agree on what a project looks like, or
// a range that is right in one would be measured against different bytes in the other. The fixtures
// and the two application helpers live here so there is one spelling of each.
//
// Nothing here re-implements the builder: `apply_in_order` and `apply_simultaneously` are the two
// ways a host could apply an edit list, and the tests assert they agree. That is the ordering
// property, not a second implementation of it.

#include "scene_canvas_patch.hpp"

#include <algorithm>
#include <cstdint>
#include <expected>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace patch_test {

namespace cli = tessera::compiler::codegen::cli;

using cli::PatchContext;
using cli::PatchDestination;
using cli::PatchRequest;
using cli::SourcePatch;
using cli::TextEdit;

// A component with a `style` attribute already on the node a commit targets, and a second element
// after it so an edit that over-reaches is visible.
inline constexpr std::string_view kInlineSource = R"(export function Card(): Node {
  return (
    <View className="card" style={{ color: "red" }}>
      <Text>Hello</Text>
    </View>
  );
}
)";

// The same component with no `style` attribute: the insert case.
inline constexpr std::string_view kNoStyleSource = R"(export function Card(): Node {
  return (
    <View className="card">
      <Text>Hello</Text>
    </View>
  );
}
)";

// A non-ASCII character before the edit point, so a byte offset that is not a code point boundary
// would split it. The `é` is two bytes in UTF-8.
inline constexpr std::string_view kUnicodeSource = R"(export function Card(): Node {
  return (
    <View className="café" style={{ color: "red" }}>
      <Text>Hello</Text>
    </View>
  );
}
)";

inline constexpr std::string_view kStylesheet = R"(.card {
  color: blue;
}

.other {
  color: green;
}
)";

inline constexpr std::string_view kEmptyStylesheet = R"(.other {
  color: green;
}
)";

// The theme asset (`stylesheet-v1.md` section 9.1). Token names carry their `--` prefix, which is
// how `ThemeToken` spells them and how a CSS custom property is written.
inline constexpr std::string_view kTheme = R"({
  "schemaVersion": 1,
  "start": "light",
  "themes": {
    "light": {
      "--accent": "#3366ff",
      "--surface": "#ffffff"
    },
    "dark": {
      "--accent": "#88aaff"
    }
  }
}
)";

// The project a patch is built against: one source, one stylesheet, and the versions the viewer
// holds. Every test starts from this and changes one thing.
inline PatchContext context_for(std::string_view source,
                                std::string_view stylesheet = kStylesheet) {
    PatchContext context;
    context.sources = {cli::ProjectFile{"ui/Card.tsx", source}};
    context.stylesheets = {cli::ProjectFile{"ui/Card.css", stylesheet}};
    context.entry = "ui/Card.tsx";
    context.node_found = true;
    context.file = "ui/Card.tsx";
    context.class_name = "card";
    context.theme_file = "assets.theme";
    context.theme_text = std::string(kTheme);
    context.theme_id = "light";
    context.theme_version = 1;
    context.versions = {{"ui/Card.tsx", 1}, {"ui/Card.css", 1}, {"assets.theme", 1}};
    return context;
}

// The span of the `<View ...>` element in `source`, which is what the IR records for the node. It
// is found by scanning rather than hard-coded, so a fixture edit cannot silently point the test at
// the wrong element.
inline void point_at_view(PatchContext& context, std::string_view source) {
    const std::size_t start = source.find("<View");
    const std::size_t end = source.find('>', start);
    context.span_offset = static_cast<std::uint32_t>(start);
    context.span_length = static_cast<std::uint32_t>(end + 1 - start);
}

inline PatchRequest inline_request() {
    PatchRequest request;
    request.destination = PatchDestination::Inline;
    request.declarations = {{"padding-left", "8px"}};
    return request;
}

// Applies `edits` to `text` in the order given. The spec's ordering rule is that this order never
// shifts a later edit, so this is the reference application.
inline std::string apply_in_order(std::string text, const std::vector<TextEdit>& edits) {
    for (const TextEdit& edit : edits) {
        text.replace(edit.offset, edit.length, edit.new_text);
    }
    return text;
}

// Applies every edit against the ORIGINAL offsets at once, by walking the text and splicing each
// range in. This is the ground truth the spec's ordering rule is about: the patch describes one
// transformation of the original file, so applying the list in the given order must give exactly
// this text. It is order-independent by construction, which is what makes it a reference rather
// than a second implementation of the same convention.
inline std::string apply_simultaneously(std::string_view text, const std::vector<TextEdit>& edits) {
    std::vector<TextEdit> sorted = edits;
    std::sort(sorted.begin(), sorted.end(), [](const TextEdit& left, const TextEdit& right) {
        return left.offset < right.offset;
    });
    std::string out;
    std::size_t cursor = 0;
    for (const TextEdit& edit : sorted) {
        out.append(text.substr(cursor, edit.offset - cursor));
        out += edit.new_text;
        cursor = edit.offset + edit.length;
    }
    out.append(text.substr(cursor));
    return out;
}

// True when `offset` is a UTF-8 code point boundary of `text`.
inline bool on_boundary(std::string_view text, std::uint32_t offset) {
    if (offset > text.size()) {
        return false;
    }
    if (offset == text.size()) {
        return true;
    }
    return (static_cast<unsigned char>(text[offset]) & 0xC0U) != 0x80U;
}

// The failure reason of a patch that must not exist.
inline std::string_view reason_of(const std::expected<SourcePatch, cli::PatchError>& patch) {
    return patch ? std::string_view("ok") : cli::patch_error_name(patch.error().code);
}

} // namespace patch_test
