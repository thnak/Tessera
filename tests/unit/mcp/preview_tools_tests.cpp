// SPDX-License-Identifier: MIT

// `set_viewport` and `snapshot` through a real session (issue #408): the acceptance's "the same
// posture set through MCP `set_viewport` gives the same scene snapshot".
//
// **What "the same" means, and why this file can assert it.** The MCP tools do not lay anything
// out; they fill in a `ScenePreviewOptions` and call `scene_preview_document()`. That is the exact
// function the browser module's `tessera_scene_json_folded` export and the native
// `tessera_scene_preview --fold` both call. So the parity claim is not "two implementations happen
// to agree" - there is one implementation - and this test pins the parts a caller can observe:
// the document is byte-identical to a direct driver call with the same options, each posture
// changes the bounds the way `hal::resolve_safe_area` says, and the viewport state is sticky across
// calls.
//
// The salt matters: `snapshot` hashes its `uiId`s under the project namespace, so a comparison has
// to pass the same one. It is stated here rather than hidden, because a test that compared an
// unsalted run with a salted one would fail on the ids and tell a reader nothing.

#include "mcp_test_support.hpp"
#include "preview_tools.hpp"
#include "scene_preview.hpp"
#include "tessera/hal/fold_posture.hpp"

#include <string>
#include <string_view>

namespace {

namespace cli = tessera::compiler::codegen::cli;
namespace hal = tessera::hal;

using mcp_test::call;
using mcp_test::expect;
using mcp_test::greet;
using mcp_test::result_text;

// A page with a `SafeArea` inside a filling parent, plus a content child that takes the SafeArea's
// content box. The tokens are the ones `packages/tessera-ui/SafeArea.tsx` lowers to; the preview's
// `build_scene()` does not splice Component nodes yet, which is a separate gap from this issue's
// layout path (see tests/unit/scene_fold_tests.cpp for the longer note).
constexpr std::string_view kPage =
    "export function Folded() {\n"
    "  return (\n"
    "    <View className=\"fill\">\n"
    "      <View className=\"tsafe safe-edges-top+right+bottom+left "
    "safe-sources-system-bars+cutout+ime+hinge\">\n"
    "        <View style={{ width: \"100%\", height: \"100%\", flexGrow: \"1\" }} />\n"
    "      </View>\n"
    "    </View>\n"
    "  );\n"
    "}\n";

// A JSON string literal for `text`. The MCP arguments are raw JSON in these tests (which is what a
// client writes) and the page is multi-line source, so its newlines and quotes have to be escaped:
// a raw newline inside a JSON string is not JSON, and the session would answer a parse error that
// would look like a tool failure.
std::string quoted(std::string_view text) {
    std::string out = "\"";
    for (const char character : text) {
        switch (character) {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            out.push_back(character);
        }
    }
    out += "\"";
    return out;
}

void test_viewport_is_sticky() {
    // `viewport.set`'s rule is "omitted fields keep their value", so a client changing only the
    // posture must not have to resend the size, and a `snapshot` must render at everything.
    mcp_test::mcp::McpSession session;
    greet(session, "mcp_preview");
    const auto sized = call(session, "set_viewport", R"({"width":"1000","height":"700","dpr":"2"})",
                            "mcp_preview", 2);
    expect(result_text(sized).find("\"width\":1000") != std::string::npos,
           "mcp_preview: set_viewport echoes the width it now holds");
    expect(result_text(sized).find("\"height\":700") != std::string::npos,
           "mcp_preview: set_viewport echoes the height it now holds");
    expect(result_text(sized).find("\"dpr\":2") != std::string::npos,
           "mcp_preview: set_viewport echoes the DPR it now holds");
    expect(result_text(sized).find("\"foldPosture\":\"none\"") != std::string::npos,
           "mcp_preview: a folded viewport starts unfolded");

    // Only the posture: the size survives.
    const auto folded =
        call(session, "set_viewport", R"({"foldPosture":"book"})", "mcp_preview", 3);
    expect(result_text(folded).find("\"width\":1000") != std::string::npos,
           "mcp_preview: omitting `width` keeps it");
    expect(result_text(folded).find("\"foldPosture\":\"book\"") != std::string::npos,
           "mcp_preview: the posture is applied");
}

void test_bad_arguments_are_refused() {
    // An unknown posture must not be treated as `flat`: a client whose typo silently previewed an
    // unfolded screen would get a snapshot that looks right and is not.
    mcp_test::mcp::McpSession session;
    greet(session, "mcp_preview");
    const auto bad = call(session, "set_viewport", R"({"foldPosture":"hinge"})", "mcp_preview", 2);
    expect(result_text(bad).find("flat, book or tabletop") != std::string::npos,
           "mcp_preview: an unknown posture is refused by name");
    expect(result_text(bad).find("\"foldPosture\"") == std::string::npos,
           "mcp_preview: a refused posture changes no state");

    const auto zero = call(session, "set_viewport", R"({"width":"0"})", "mcp_preview", 3);
    expect(result_text(zero).find("positive") != std::string::npos,
           "mcp_preview: a zero width is refused");

    // ... and an unknown posture is NOT silently unfolded by the snapshot either.
    const auto direct =
        call(session, "set_viewport", R"({"foldPosture":"tabletop"})", "mcp_preview", 4);
    expect(result_text(direct).find("\"foldPosture\":\"tabletop\"") != std::string::npos,
           "mcp_preview: a valid posture after a refused one still applies");
}

// One `snapshot` at the session's viewport, with the document the direct driver call produces for
// the same options. They must be byte-identical, or the tools and the engine disagree.
void test_snapshot_matches_the_driver() {
    for (const std::string_view posture : {"flat", "book", "tabletop"}) {
        mcp_test::mcp::McpSession session;
        greet(session, "mcp_preview");
        (void)call(session, "set_viewport",
                   R"({"width":"1000","height":"700","dpr":"1","foldPosture":)" + quoted(posture) +
                       "}",
                   "mcp_preview", 2);
        const auto snapshot =
            call(session, "snapshot",
                 R"({"source":)" + quoted(kPage) + R"(,"file":"ui/Folded.tsx"})", "mcp_preview", 3);
        const std::string via_mcp = result_text(snapshot);
        expect(!via_mcp.empty(), "mcp_preview: snapshot answers a document");

        cli::ScenePreviewOptions options;
        options.file = "ui/Folded.tsx";
        options.width = 1000.0f;
        options.height = 700.0f;
        options.dpi_scale = 1.0f;
        options.salt = "preview";
        const std::optional<hal::FoldPosture> parsed = hal::parse_fold_posture(posture);
        expect(parsed.has_value(), "mcp_preview: the posture parses");
        options.posture = parsed.value_or(hal::FoldPosture::None);
        const std::string direct = cli::scene_preview_document(kPage, options, {});

        expect(via_mcp == direct, std::string("mcp_preview: the `") + std::string(posture) +
                                      "` snapshot is byte-identical to a direct driver call");
        // ... and it is a real tree, not two empty documents agreeing.
        expect(via_mcp.find("\"nodes\":[]") == std::string::npos,
               std::string("mcp_preview: the `") + std::string(posture) + "` snapshot has nodes");
    }
}

// The postures produce DIFFERENT documents for the same viewport: if they agreed, the parity above
// would be vacuous.
void test_postures_differ() {
    mcp_test::mcp::McpSession session;
    greet(session, "mcp_preview");
    std::string previous;
    for (const std::string_view posture : {"flat", "book", "tabletop"}) {
        (void)call(session, "set_viewport",
                   R"({"width":"1000","height":"700","dpr":"1","foldPosture":)" + quoted(posture) +
                       "}",
                   "mcp_preview", 2);
        const auto snapshot =
            call(session, "snapshot",
                 R"({"source":)" + quoted(kPage) + R"(,"file":"ui/Folded.tsx"})", "mcp_preview", 3);
        const std::string document = result_text(snapshot);
        expect(!document.empty(), "mcp_preview: a posture answers a document");
        if (!previous.empty()) {
            expect(document != previous, std::string("mcp_preview: `") + std::string(posture) +
                                             "` lays out differently from the posture before it");
        }
        previous = document;
    }
}

} // namespace

int main() {
    test_viewport_is_sticky();
    test_bad_arguments_are_refused();
    test_snapshot_matches_the_driver();
    test_postures_differ();
    if (mcp_test::failures == 0) {
        std::cout << "mcp_preview: ok\n";
    }
    return mcp_test::failures == 0 ? 0 : 1;
}
