// SPDX-License-Identifier: MIT

// The fixtures shared by the `tessera-mcp` project-folder tests (issue #406 and the `generate` tool
// of issue #407): one fixture project on disk, the JSON-RPC calls over a real `McpSession`, and the
// entry point of each case. The cases themselves are split by responsibility across translation
// units (the project folder in project_tests.cpp, `generate` in project_generate_tests.cpp) so no
// one file outgrows the size rules; the executable and its ctest name stay the same.

#pragma once

#include "mcp_test_support.hpp"
#include "regenerate.hpp"
#include "tessera/harness/json_parse.hpp"
#include "tessera/schema/project_files.hpp"

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace mcp_project {

namespace fs = std::filesystem;
namespace harness = tessera::harness;
namespace cli = tessera::compiler::codegen::cli;
namespace mcp = tessera::mcp;

using mcp_test::call;
using mcp_test::decode;
using mcp_test::expect;
using mcp_test::result_is_error;
using mcp_test::result_text;
using mcp_test::string_at;

// The prefix every message of these cases carries.
inline constexpr std::string_view kWho = "mcp_project";

void write_file(const fs::path& path, std::string_view text);
std::string read_file(const fs::path& path);

// The fixture project: a config, an entry that may be broken (so the diagnostics document is not
// empty), a clean second component, a stylesheet, a C++ file and a dependency directory the walk
// must not enter.
fs::path make_project(const fs::path& base, std::string_view name, bool broken);

mcp::SessionReply send(mcp::McpSession& session, std::string_view line);

// A session with the handshake done and `root` opened through the `open_project` tool.
void open_project(mcp::McpSession& session, const fs::path& root);

harness::JsonValue listing_of(mcp::McpSession& session);
const harness::JsonValue* file_entry(const harness::JsonValue& listing, std::string_view path);

// The file URI a client would put in `roots/list` for a directory.
std::string file_uri(const fs::path& directory);

// `generate` (issue #407) helpers.
std::string sha256_of(std::string_view bytes);
bool exists_dir(const fs::path& path);
harness::JsonValue generate_call(mcp::McpSession& session, std::string_view arguments);
std::vector<std::string> strings_at(const harness::JsonValue& object, std::string_view key);

// The project-folder cases: `list_files`, the folder-wide `diagnostics` / `get_ir`, the three ways
// the folder is chosen, and the named refusals.
void project_listing(const fs::path& base);
void project_folder_diagnostics_parity(const fs::path& base);
void project_folder_ir(const fs::path& base);
void project_root_resolution(const fs::path& base);
void project_refusals(const fs::path& base);
void project_process_in_project_directory(const fs::path& executable, const fs::path& base);

// The `generate` cases (issue #407).
void project_generate(const fs::path& base);
void project_generate_refusals(const fs::path& base);

} // namespace mcp_project
