// SPDX-License-Identifier: MIT

// The fixture project and the two listing helpers of the MCP project-folder suite. Defined in one
// translation unit so both halves of the suite share them (see `project_test_support.hpp`).

#include "project_test_support.hpp"
#include "tessera/ui/automation/sha256.hpp"

#include <fstream>
#include <sstream>
#include <utility>

namespace mcp_project_test {

using mcp_test::call;
using mcp_test::decode;
using mcp_test::expect;
using mcp_test::result_is_error;
using mcp_test::result_text;
using mcp_test::string_at;

void write_file(const fs::path& path, std::string_view text) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << text;
}

std::string read_file(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

// The fixture project: a config, a broken entry (so the diagnostics document is not empty), a clean
// second component, a stylesheet, a C++ file and a dependency directory the walk must not enter.
fs::path make_project(const fs::path& base, std::string_view name, bool broken) {
    const fs::path root = base / name;
    fs::remove_all(root);
    write_file(root / "tessera.json",
               R"({"app":{"namespace":"mcp.fixture"},"entry":"ui/App.tsx"})");
    write_file(root / "ui/App.tsx", broken ? "export function App() {\n  return <NotAThing />;\n}\n"
                                           : "export function App() {\n  return <View />;\n}\n");
    write_file(root / "ui/Ok.tsx", "export function Ok() {\n  return <View />;\n}\n");
    write_file(root / "ui/app.css", ".a { color: red; }\n");
    write_file(root / "src/Impl.cpp", "int main() { return 0; }\n");
    write_file(root / "node_modules/dep/index.tsx", "export function Dep() { return <View />; }\n");
    return root;
}

[[maybe_unused]] std::string quoted_arg(const std::string& path) {
    return "\"" + path + "\"";
}

mcp::SessionReply send(mcp::McpSession& session, std::string_view line) {
    auto reply = session.handle_line(line);
    if (!reply) {
        std::cerr << kWho << ": the session could not handle a line\n";
        ++mcp_test::failures;
        return {};
    }
    return std::move(*reply);
}

harness::JsonValue listing_of(mcp::McpSession& session) {
    const harness::JsonValue response = call(session, "list_files", "{}", kWho);
    return decode(result_text(response), kWho);
}

const harness::JsonValue* file_entry(const harness::JsonValue& listing, std::string_view path) {
    const harness::JsonValue* files = listing.find("files");
    if (files == nullptr || files->as_array() == nullptr) {
        return nullptr;
    }
    for (const harness::JsonValue& file : *files->as_array()) {
        if (string_at(file, "path") == path) {
            return &file;
        }
    }
    return nullptr;
}

void open_project(mcp::McpSession& session, const fs::path& root) {
    const std::string args = R"({"path":")" + std::string([&] {
                                 std::string escaped;
                                 for (const char c : root.generic_string()) {
                                     if (c == '"' || c == '\\') {
                                         escaped.push_back('\\');
                                     }
                                     escaped.push_back(c);
                                 }
                                 return escaped;
                             }()) +
                             R"("})";
    const harness::JsonValue response = call(session, "open_project", args, kWho);
    expect(!result_is_error(response), "mcp_project: the fixture project opens");
}

// The file URI a client would put in `roots/list` for a directory.
std::string file_uri(const fs::path& directory) {
    const std::string path = fs::absolute(directory).generic_string();
    return path.front() == '/' ? "file://" + path : "file:///" + path;
}

std::string sha256_of(std::string_view bytes) {
    return "sha256:" + tessera::ui::automation::to_hex(tessera::ui::automation::sha256(bytes));
}

} // namespace mcp_project_test
