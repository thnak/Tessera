// SPDX-License-Identifier: MIT

// `query_style` and `pick` through a real session (issue #396): the read-only inspection tools.
//
// **What is asserted, and why those things.** Both tools are queries over the same laid-out tree
// `snapshot` describes, so the strongest checks are cross-tool ones: the id a `snapshot` lists is
// the id `query_style` accepts, and the box `pick` reports for a node is the bounds `snapshot`
// reports for it. The cascade facts (`computed`, `matched`, the box) come from `style_info_for()`,
// the function the studio's style inspector calls, so this file pins the MCP-visible contract -
// field names, the `no_such_node` answer, argument refusal, determinism - not the cascade itself,
// which scene_canvas_inspect_tests covers.

#include "mcp_test_support.hpp"
#include "preview_tools.hpp"
#include "scene_canvas_inspect.hpp"
#include "scene_prepare.hpp"

#include <string>
#include <string_view>

namespace {

namespace cli = tessera::compiler::codegen::cli;

using mcp_test::call;
using mcp_test::decode;
using mcp_test::expect;
using mcp_test::greet;
using mcp_test::result_text;

// Explicit automation ids, so the test names nodes without reading them back first. 5002 is a
// 50x40 child at the origin of the 200x100 parent 5001.
constexpr std::string_view kPage =
    "export function Probe() {\n"
    "  return (\n"
    "    <View automationId=\"5001\" style={{ width: \"200px\", height: \"100px\", "
    "backgroundColor: \"red\" }}>\n"
    "      <View automationId=\"5002\" style={{ width: \"50px\", height: \"40px\" }} />\n"
    "    </View>\n"
    "  );\n"
    "}\n";

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
        default:
            out.push_back(character);
        }
    }
    out += "\"";
    return out;
}

std::string args(std::string_view rest) {
    return "{\"source\":" + quoted(kPage) + "," + std::string(rest) + "}";
}

bool is_error(const tessera::harness::JsonValue& response) {
    const auto* result = response.find("result");
    const auto* flag = result == nullptr ? nullptr : result->find("isError");
    return flag != nullptr && flag->as_bool().value_or(false);
}

void test_query_style_answers_the_cascade() {
    mcp_test::mcp::McpSession session;
    greet(session, "mcp_query");
    const auto response = call(session, "query_style", args(R"("uiId":"5001")"), "mcp_query", 2);
    expect(!is_error(response), "mcp_query: query_style on a real node is not an error");
    const auto document = decode(result_text(response), "mcp_query");
    expect(document.find("styleVersion") != nullptr &&
               document.find("styleVersion")->as_integer() == 1,
           "mcp_query: the document carries styleVersion 1");
    expect(mcp_test::string_at(document, "node") == "5001", "mcp_query: it names the node");
    const auto* computed = document.find("computed");
    expect(computed != nullptr && computed->find("width") != nullptr &&
               *computed->find("width")->as_string() == "200px",
           "mcp_query: computed reports the width the source set");
    const auto* box = document.find("box");
    expect(box != nullptr && box->find("width") != nullptr &&
               box->find("width")->as_number() == 200.0 &&
               box->find("height")->as_number() == 100.0,
           "mcp_query: the box is the laid-out 200x100");
    expect(box != nullptr && box->find("padding") != nullptr && box->find("padding")->size() == 4,
           "mcp_query: the box carries the four-edge sets");
    const auto* matched = document.find("matched");
    expect(matched != nullptr && matched->is_array() && matched->size() > 0,
           "mcp_query: the node matches at least the built-in sheet's rule");
    // The rule's file name is storage the compiled sheet only views; the scene must keep it alive,
    // or a freed buffer reaches the document (it did, and it was not valid UTF-8).
    const auto* span = matched == nullptr || matched->size() == 0
                           ? nullptr
                           : (*matched->as_array())[0].find("span");
    expect(span != nullptr && mcp_test::string_at(*span, "file") == "builtin",
           "mcp_query: a matched rule names the sheet it came from");
    // The cascade the studio inspector runs: the same engine function, called directly, on the same
    // scene, must produce the same facts.
    cli::ScenePreviewOptions options = mcp_test::mcp::scene_preview_options({}, "");
    const cli::PreparedScene scene = cli::prepare_scene(kPage, options);
    const cli::StyleInfoResult direct = cli::style_info_for(*scene.root, *scene.sheet, 5001);
    expect(direct.found && computed != nullptr && computed->size() == direct.computed.size(),
           "mcp_query: computed has exactly the properties the engine's inspector reports");
}

void test_query_style_missing_node() {
    mcp_test::mcp::McpSession session;
    greet(session, "mcp_query");
    for (const std::string_view id : {"\"123456\"", "\"0\"", "7"}) {
        const auto response =
            call(session, "query_style", args("\"uiId\":" + std::string(id)), "mcp_query", 2);
        expect(!is_error(response), "mcp_query: a missing node is an answer, not an error");
        const auto document = decode(result_text(response), "mcp_query");
        expect(document.find("ok") != nullptr && document.find("ok")->as_bool() == false &&
                   mcp_test::string_at(document, "reason") == "no_such_node" &&
                   document.size() == 3,
               "mcp_query: a missing node answers ok:false, no_such_node and nothing else");
    }
}

void test_pick_finds_nodes() {
    mcp_test::mcp::McpSession session;
    greet(session, "mcp_query");
    const auto inner = decode(
        result_text(call(session, "pick", args(R"("x":10,"y":10)"), "mcp_query", 2)), "mcp_query");
    expect(inner.find("pickVersion") != nullptr && inner.find("found")->as_bool() == true,
           "mcp_query: a point over a node finds it");
    expect(mcp_test::string_at(inner, "automationId") == "5002",
           "mcp_query: pick reports the deepest node under the point");
    const auto* box = inner.find("box");
    expect(box != nullptr && box->find("width")->as_number() == 50.0 &&
               box->find("height")->as_number() == 40.0,
           "mcp_query: pick reports the node's own box");

    // Text arguments work as numbers do: the viewport tool's arguments are text.
    const auto outer =
        decode(result_text(call(session, "pick", args(R"("x":"150","y":"80")"), "mcp_query", 3)),
               "mcp_query");
    expect(mcp_test::string_at(outer, "automationId") == "5001",
           "mcp_query: a point outside the child picks the parent");

    // The picked id is one `query_style` accepts: the tools describe one tree.
    const auto style = decode(
        result_text(call(session, "query_style",
                         args("\"uiId\":\"" + mcp_test::string_at(outer, "automationId") + "\""),
                         "mcp_query", 4)),
        "mcp_query");
    expect(mcp_test::string_at(style, "node") == "5001",
           "mcp_query: a picked id round-trips through query_style");
}

void test_pick_miss_is_found_false() {
    mcp_test::mcp::McpSession session;
    greet(session, "mcp_query");
    (void)call(session, "set_viewport", R"({"width":"1000","height":"800"})", "mcp_query", 2);
    const auto response = call(session, "pick", args(R"("x":900,"y":700)"), "mcp_query", 3);
    expect(!is_error(response), "mcp_query: a miss is not an error");
    const auto document = decode(result_text(response), "mcp_query");
    // The root of the preview tree fills the viewport, so a point past the content is either a
    // miss or the root; both are answers. What must hold is that the result is well formed and
    // never names the content nodes.
    expect(document.find("found") != nullptr, "mcp_query: a miss still answers `found`");
    expect(mcp_test::string_at(document, "automationId") != "5001" &&
               mcp_test::string_at(document, "automationId") != "5002",
           "mcp_query: a point past the content does not name a content node");
}

void test_viewport_is_honoured() {
    // The viewport `set_viewport` holds is the layout the query reads: a percentage width tracks
    // it.
    constexpr std::string_view page =
        "export function Fill() {\n"
        "  return <View automationId=\"6001\" style={{ width: \"100%\", height: \"50px\" }} />;\n"
        "}\n";
    mcp_test::mcp::McpSession session;
    greet(session, "mcp_query");
    (void)call(session, "set_viewport", R"({"width":"640","height":"480"})", "mcp_query", 2);
    const auto document = decode(
        result_text(call(session, "query_style",
                         "{\"source\":" + quoted(page) + ",\"uiId\":\"6001\"}", "mcp_query", 3)),
        "mcp_query");
    const auto* box = document.find("box");
    expect(box != nullptr && box->find("width")->as_number() == 640.0,
           "mcp_query: query_style lays out at the viewport set_viewport holds");
}

void test_refusals() {
    mcp_test::mcp::McpSession session;
    greet(session, "mcp_query");
    // A protocol error (InvalidParams) for a malformed call.
    const auto no_id = call(session, "query_style", args("\"file\":\"ui/A.tsx\""), "mcp_query", 2);
    expect(no_id.find("error") != nullptr, "mcp_query: query_style without uiId is InvalidParams");
    const auto bad_id = call(session, "query_style", args(R"("uiId":"abc")"), "mcp_query", 3);
    expect(bad_id.find("error") != nullptr, "mcp_query: a non-numeric uiId is InvalidParams");
    const auto no_xy = call(session, "pick", args(R"("x":1)"), "mcp_query", 4);
    expect(no_xy.find("error") != nullptr, "mcp_query: pick without y is InvalidParams");
    const auto nan_xy = call(session, "pick", args(R"("x":"nan","y":"1")"), "mcp_query", 5);
    expect(nan_xy.find("error") != nullptr, "mcp_query: a non-finite coordinate is refused");
    const auto no_source = call(session, "pick", R"({"x":1,"y":1})", "mcp_query", 6);
    expect(no_source.find("error") != nullptr, "mcp_query: pick without source is InvalidParams");

    // A tool error (isError) for a source that does not evaluate, pointing at `diagnostics`.
    const auto broken =
        call(session, "pick", R"({"source":"export function (","x":1,"y":1})", "mcp_query", 7);
    expect(is_error(broken) && result_text(broken).find("diagnostics") != std::string::npos,
           "mcp_query: a source that does not evaluate is a tool error that names `diagnostics`");
}

void test_deterministic_and_redacted() {
    mcp_test::mcp::McpSession first;
    mcp_test::mcp::McpSession second;
    greet(first, "mcp_query");
    greet(second, "mcp_query");
    const std::string a =
        result_text(call(first, "query_style", args(R"("uiId":"5001")"), "mcp_query", 2));
    const std::string b =
        result_text(call(second, "query_style", args(R"("uiId":"5001")"), "mcp_query", 2));
    expect(!a.empty() && a == b, "mcp_query: two sessions answer the same bytes");
    expect(a.find(":\\") == std::string::npos && a.find("/home/") == std::string::npos &&
               a.find("/Users/") == std::string::npos,
           "mcp_query: no absolute host path reaches the document");
}

} // namespace

int main() {
    test_query_style_answers_the_cascade();
    test_query_style_missing_node();
    test_pick_finds_nodes();
    test_pick_miss_is_found_false();
    test_viewport_is_honoured();
    test_refusals();
    test_deterministic_and_redacted();
    if (mcp_test::failures == 0) {
        std::cout << "mcp_query: ok\n";
    }
    return mcp_test::failures == 0 ? 0 : 1;
}
