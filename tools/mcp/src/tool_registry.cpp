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
    schema_value.set("type", JsonValue::string("object"));
    schema_value.set("properties", JsonValue::object(std::move(properties)));
    return schema_value;
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
        "Open a project directory so later tools resolve project-relative paths against it. This "
        "build validates the path and records it; it does not read or write the directory.",
        ToolStatus::Implemented, ToolSource::Compiler,
        schema(JsonValue::Object{{"path", property("Project directory to open.")}})});
    list.push_back(ToolDefinition{
        "diagnostics",
        "Compile one TSX source with the real front end and return the machine-readable "
        "diagnostics document of docs/specs/compiler-diagnostics-v1.md, byte-identical to "
        "`tessera_codegen --diagnostics-json` for the same input.",
        ToolStatus::Implemented, ToolSource::Compiler,
        schema(compile_properties("The .tsx text to compile (in memory; no file is read)."))});
    list.push_back(ToolDefinition{
        "get_ir",
        "Lower one TSX source and return its canonical IR JSON (docs/specs/ir-v1.md). A source "
        "that "
        "does not reach the IR returns the diagnostics document with `isError` instead of an empty "
        "module.",
        ToolStatus::Implemented, ToolSource::Compiler,
        schema(compile_properties("The .tsx text to lower."))});
    list.push_back(ToolDefinition{
        "list_files", "List the files of the open project, with their kinds and versions.",
        ToolStatus::Declared, ToolSource::DesignerProtocol, schema({})});

    // --- The rest of the section 8 surface, declared so the listing is the whole surface. ---
    list.push_back(ToolDefinition{
        "apply_edits",
        "Apply text edits to the open project in memory; `write: true` also writes them to the "
        "project directory. Never edits generated C++.",
        ToolStatus::Declared, ToolSource::DesignerProtocol,
        schema(
            JsonValue::Object{{"edits", property("Text edits to apply to the open project.")}})});
    list.push_back(ToolDefinition{
        "set_viewport", "Set the preview viewport the later tools render at.", ToolStatus::Declared,
        ToolSource::DesignerProtocol,
        schema(JsonValue::Object{
            {"viewport", property("Device size, DPR, theme, locale and safe areas.")}})});
    list.push_back(ToolDefinition{
        "snapshot", "Return the harness UiSnapshot: node tree, bounds, roles, text, and focus.",
        ToolStatus::Declared, ToolSource::Harness, schema({})});
    list.push_back(ToolDefinition{
        "query_style", "Return a node's computed style, matched rules and layout box.",
        ToolStatus::Declared, ToolSource::DesignerProtocol,
        schema(JsonValue::Object{{"uiId", property("The decimal UiId of the node to inspect.")}})});
    list.push_back(
        ToolDefinition{"pick", "Return the node at a point in the current preview.",
                       ToolStatus::Declared, ToolSource::DesignerProtocol,
                       schema(JsonValue::Object{{"x", property("Point x in device pixels.")},
                                                {"y", property("Point y in device pixels.")}})});
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
    return ToolResult{
        true, "TESSERA_MCP_TOOL_UNIMPLEMENTED: `" + std::string(tool.name) +
                  "` is declared but not implemented in this build (it maps to " +
                  std::string(source_name(tool.source)) +
                  "). The compile-only subset (open_project, diagnostics, get_ir) ships first; see "
                  "docs/specs/designer-studio-v1.md section 8."};
}

} // namespace tessera::mcp
