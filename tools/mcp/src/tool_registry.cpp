// SPDX-License-Identifier: MIT

#include "mcp_protocol.hpp"
#include "tool_registry.hpp"

#include <string>
#include <utility>

namespace tessera::mcp {
namespace {

// A JSON-schema object with the given properties and no required member: every tool here accepts a
// partial call and reports the missing argument as InvalidParams, which is friendlier than the
// client's own schema validation refusing the request without a server-side message.
JsonValue schema(JsonValue::Object properties) {
    JsonValue schema_value = JsonValue::object();
    // Every schema declares its dialect (mcp-server-v1.md section 3: JSON Schema 2020-12).
    schema_value.set("$schema", JsonValue::string("https://json-schema.org/draft/2020-12/schema"));
    schema_value.set("type", JsonValue::string("object"));
    schema_value.set("properties", JsonValue::object(std::move(properties)));
    return schema_value;
}

JsonValue boolean_property(std::string_view description) {
    JsonValue value = JsonValue::object();
    value.set("type", JsonValue::string("boolean"));
    value.set("description", JsonValue::string(std::string(description)));
    return value;
}

JsonValue property(std::string_view description) {
    JsonValue value = JsonValue::object();
    value.set("type", JsonValue::string("string"));
    value.set("description", JsonValue::string(std::string(description)));
    return value;
}

// The `source` argument of the compile tools: the TSX text itself, defaulting to a buffer name for
// the spans (`docs/specs/compiler-diagnostics-v1.md` section 2).
JsonValue::Object compile_properties(std::string_view source_description) {
    return JsonValue::Object{{"source", property(source_description)},
                             {"file", property("Project-relative name the diagnostics' spans are "
                                               "reported against, for example `ui/Counter.tsx`; "
                                               "defaults to `buffer.tsx`.")}};
}

std::vector<ToolDefinition> make_tools() {
    std::vector<ToolDefinition> list;

    // --- Compile-only surface (D-S6): these exist because the front end exists. ---
    list.push_back(ToolDefinition{
        "open_project",
        "Open a project directory so later tools resolve project-relative paths against it. An "
        "explicit path overrides the client's `roots`, which override the server's working "
        "directory. This call validates the path and records it; it reads and writes nothing.",
        ToolStatus::Implemented, ToolSource::Compiler,
        schema(JsonValue::Object{{"path", property("Project directory to open.")}})});
    list.push_back(ToolDefinition{
        "diagnostics",
        "Compile TSX with the real front end and return the machine-readable diagnostics document "
        "of docs/specs/compiler-diagnostics-v1.md, byte-identical to `tessera_codegen "
        "--diagnostics-json` for the same input. With `source` it compiles that text in memory; "
        "without it, it compiles the whole open project folder.",
        ToolStatus::Implemented, ToolSource::Compiler,
        schema(compile_properties(
            "The .tsx text to compile in memory. Omit it to compile the open project folder."))});
    list.push_back(ToolDefinition{
        "get_ir",
        "Lower TSX and return its canonical IR JSON (docs/specs/ir-v1.md): the text of `source`, "
        "or the whole open project folder when `source` is omitted. Input that does not reach the "
        "IR returns the diagnostics document with `isError` instead of an empty module.",
        ToolStatus::Implemented, ToolSource::Compiler,
        schema(compile_properties(
            "The .tsx text to lower. Omit it to lower the open project folder."))});
    list.push_back(ToolDefinition{
        "generate",
        "Generate the C++ of the open project folder with the same driver as `tessera_codegen` "
        "(ADR-040). By default a dry run: it reports every generated file (size, SHA-256, `new`, "
        "`changed` or `unchanged`), the developer classes it would scaffold, and `exitCode` 1 when "
        "something differs, and writes nothing. With `write: true` it rewrites the generated bases "
        "inside the output directory (`codegen.outDir`, default `generated`) and scaffolds missing "
        "developer classes; it never modifies an existing developer file and never writes outside "
        "the project root. With `path` it returns that one generated file's text (always a dry "
        "run). A project with a blocking diagnostic returns the diagnostics document with "
        "`isError` and writes nothing.",
        ToolStatus::Implemented, ToolSource::Compiler,
        schema(JsonValue::Object{
            {"write", boolean_property("Write the generated files. Omitted or false: dry run.")},
            {"path", property("A generated file (`generated/App.g.hpp` or `App.g.hpp`): return its "
                              "text instead of the summary.")}})});
    list.push_back(ToolDefinition{
        "list_files",
        "List the files of the open project folder: project-relative path, kind (`source`, "
        "`style`, "
        "`config`, `theme`, `asset`, `other`), size, `readable` and a content digest. A file that "
        "is not accepted UI code, a `node_modules` or dot directory and a symbolic link are listed "
        "as `other` and never read. The folder is the opened project, else the client's first "
        "root, else the server's working directory; the result names `rootSource`.",
        ToolStatus::Implemented, ToolSource::Compiler, schema({})});

    // --- The rest of the section 8 surface, declared so the listing is the whole surface. ---
    list.push_back(ToolDefinition{
        "apply_edits",
        "Apply text edits to the open project in memory; `write: true` also writes them to the "
        "project directory. Never edits generated C++.",
        ToolStatus::Declared, ToolSource::DesignerProtocol,
        schema(
            JsonValue::Object{{"edits", property("Text edits to apply to the open project.")}})});
    list.push_back(ToolDefinition{
        "set_viewport",
        "Set the preview viewport that later tools render at: `width`, `height`, `dpr` and the "
        "simulated foldable `foldPosture` (`flat`, `book`, `tabletop`). Omitted fields keep their "
        "value, as `viewport.set` does. The posture is data - no sensor, no clock - and reaches "
        "the "
        "same metrics and safe-area path the native app runs, so `snapshot` answers the document "
        "the studio previews for that posture.",
        ToolStatus::Implemented, ToolSource::DesignerProtocol,
        schema(JsonValue::Object{
            {"width", property("Logical viewport width in pixels. Omitted: unchanged.")},
            {"height", property("Logical viewport height in pixels. Omitted: unchanged.")},
            {"dpr", property("Device pixel ratio. Omitted: unchanged.")},
            {"foldPosture",
             property("Simulated foldable posture: `flat`, `book` (half-opened, vertical hinge) or "
                      "`tabletop` (half-opened, horizontal hinge). Omitted: unchanged.")}})});
    list.push_back(ToolDefinition{
        "snapshot",
        "Return the harness UiSnapshot document (docs/specs/automation-ids-v1.md section 5.2) for "
        "the source, laid out at the viewport `set_viewport` last set. Byte-identical to the "
        "browser module's `tessera_scene_json_folded` for the same input.",
        ToolStatus::Implemented, ToolSource::Harness,
        schema(JsonValue::Object{
            {"source", property("The .tsx text to evaluate (in memory; no file is read).")},
            {"file", property("Project-relative name the ids are hashed under; defaults to "
                              "`ui/Preview.tsx`.")}})});
    list.push_back(ToolDefinition{
        "query_style",
        "Return a node's computed style, matched rules (highest precedence first, with the "
        "cascade's own `overridden`) and layout box, as the studio's style inspector does. The "
        "source is laid out at the viewport `set_viewport` last set; a `uiId` no node carries "
        "answers `ok:false, reason:no_such_node`. A query: nothing is dispatched or changed.",
        ToolStatus::Implemented, ToolSource::DesignerProtocol,
        schema(JsonValue::Object{
            {"source", property("The .tsx text to evaluate (in memory; no file is read).")},
            {"file", property("Project-relative name the ids are hashed under; defaults to "
                              "`ui/Preview.tsx`.")},
            {"uiId",
             property("The decimal UiId of the node to inspect, as `snapshot` lists it.")}})});
    list.push_back(ToolDefinition{
        "pick",
        "Return the node at a point in the preview: its automation id, element kind and box, found "
        "with the input router's own hit test, so it is the node a press there would reach. `x` "
        "and "
        "`y` are logical viewport pixels, the space `snapshot`'s bounds are in. A point over no "
        "node answers `found:false`. A query: nothing is dispatched or changed.",
        ToolStatus::Implemented, ToolSource::DesignerProtocol,
        schema(JsonValue::Object{
            {"source", property("The .tsx text to evaluate (in memory; no file is read).")},
            {"file", property("Project-relative name the ids are hashed under; defaults to "
                              "`ui/Preview.tsx`.")},
            {"x", property("Point x in logical viewport pixels.")},
            {"y", property("Point y in logical viewport pixels.")}})});
    list.push_back(ToolDefinition{
        "screenshot",
        "Render the scene and return a PNG. Best-effort visual evidence, never a golden.",
        ToolStatus::Declared, ToolSource::Harness, schema({})});
    list.push_back(ToolDefinition{
        "run_script",
        "Run a harness script (launch/click/assert/...) and return its report document.",
        ToolStatus::Declared, ToolSource::Harness,
        schema(JsonValue::Object{{"script", property("The harness script document to run.")}})});
    return list;
}

std::string_view source_name(ToolSource source) noexcept {
    switch (source) {
    case ToolSource::Compiler:
        return "the compiler driver";
    case ToolSource::Harness:
        return "a headless-automation-v1 command";
    case ToolSource::DesignerProtocol:
        return "a designer-protocol-v1 host message";
    }
    return "an unimplemented source";
}

} // namespace

const std::vector<ToolDefinition>& tools() {
    // Built once: the surface is fixed for the life of the process, and `tools/list` must answer
    // the same bytes every time (a client may cache it).
    static const std::vector<ToolDefinition> list = make_tools();
    return list;
}

const ToolDefinition* find_tool(std::string_view name) noexcept {
    for (const ToolDefinition& tool : tools()) {
        if (tool.name == name) {
            return &tool;
        }
    }
    return nullptr;
}

JsonValue tools_list_result() {
    JsonValue entries = JsonValue::array();
    for (const ToolDefinition& tool : tools()) {
        JsonValue entry = JsonValue::object();
        entry.set("name", JsonValue::string(std::string(tool.name)));
        entry.set("description", JsonValue::string(std::string(tool.description)));
        entry.set("inputSchema", tool.input_schema);
        // An extension, not an MCP member: a client that does not know it ignores it, and one that
        // does can tell an implemented tool from a declared one before calling it, which is the
        // honesty the section 8 milestone rule asks for.
        JsonValue annotations = JsonValue::object();
        annotations.set(
            "tesseraStatus",
            JsonValue::string(tool.status == ToolStatus::Implemented ? "implemented" : "declared"));
        entry.set("annotations", std::move(annotations));
        entries.push_back(std::move(entry));
    }
    JsonValue result = JsonValue::object();
    result.set("tools", std::move(entries));
    return result;
}

JsonValue tool_result_json(const ToolResult& result) {
    JsonValue text = JsonValue::object();
    text.set("type", JsonValue::string("text"));
    text.set("text", JsonValue::string(result.text));
    JsonValue content = JsonValue::array();
    content.push_back(std::move(text));

    JsonValue value = JsonValue::object();
    value.set("content", std::move(content));
    value.set("isError", JsonValue::boolean(result.is_error));
    return value;
}

ToolResult unimplemented_tool_result(const ToolDefinition& tool) {
    return ToolResult{true, "TESSERA_MCP_TOOL_UNIMPLEMENTED: `" + std::string(tool.name) +
                                "` is declared but not implemented in this build (it maps to " +
                                std::string(source_name(tool.source)) +
                                "). `tools/list` marks each tool `implemented` or `declared`; see "
                                "docs/specs/designer-studio-v1.md section 8."};
}

} // namespace tessera::mcp
