// SPDX-License-Identifier: MIT

// The shared fixtures of the `tessera-mcp` project-folder tests (project_fixtures.hpp): the fixture
// project on disk and the JSON-RPC calls the cases make through a real session.

#include "project_fixtures.hpp"
#include "tessera/ui/automation/sha256.hpp"

#include <fstream>
#include <iostream>
#include <sstream>

namespace mcp_project {

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

mcp::SessionReply send(mcp::McpSession& session, std::string_view line) {
    auto reply = session.handle_line(line);
    if (!reply) {
        std::cerr << kWho << ": the session could not handle a line\n";
        ++mcp_test::failures;
        return {};
    }
    return std::move(*reply);
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

std::string file_uri(const fs::path& directory) {
    const std::string path = fs::absolute(directory).generic_string();
    return path.front() == '/' ? "file://" + path : "file:///" + path;
}

std::string sha256_of(std::string_view bytes) {
    return "sha256:" + tessera::ui::automation::to_hex(tessera::ui::automation::sha256(bytes));
}

bool exists_dir(const fs::path& path) {
    std::error_code error;
    return fs::is_directory(path, error);
}

harness::JsonValue generate_call(mcp::McpSession& session, std::string_view arguments) {
    return call(session, "generate", arguments, kWho);
}

std::vector<std::string> strings_at(const harness::JsonValue& object, std::string_view key) {
    std::vector<std::string> out;
    const harness::JsonValue* list = object.find(key);
    if (list != nullptr && list->as_array() != nullptr) {
        for (const harness::JsonValue& item : *list->as_array()) {
            if (item.is_string()) {
                out.emplace_back(*item.as_string());
            }
        }
    }
    return out;
}

} // namespace mcp_project
