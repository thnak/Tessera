// SPDX-License-Identifier: MIT

// Fixtures shared by the two halves of the MCP project-folder suite: the folder reads in
// `project_tests.cpp` and the `generate` tool in `mcp_project_generate_tests.cpp`. Split when the
// single file passed the 500-line hard limit.
//
// Declarations only -- the definitions are in `project_test_support.cpp` -- because `inline`
// definitions in a header put the fixture project in every translation unit and make the include
// order matter (the helpers below are used by both halves and sit between them in the original).

#pragma once

#include "mcp_test_support.hpp"
#include "session.hpp"

#include <filesystem>
#include <string>
#include <string_view>

namespace mcp_project_test {

namespace fs = std::filesystem;
namespace mcp = tessera::mcp;
namespace harness = tessera::harness;

// The client name every call in the suite sends.
inline constexpr std::string_view kWho = "mcp_project";

void write_file(const fs::path& path, std::string_view text);
std::string read_file(const fs::path& path);

// The fixture project: a config, a broken entry (so the diagnostics document is not empty), a clean
// second component, a stylesheet, a C++ file and a dependency directory the walk must not enter.
fs::path make_project(const fs::path& base, std::string_view name, bool broken);

void open_project(mcp::McpSession& session, const fs::path& root);

// One request/response round trip; a malformed line fails the suite.
mcp::SessionReply send(mcp::McpSession& session, std::string_view line);

// The `file://` URI a client puts in `roots/list` for a directory.
std::string file_uri(const fs::path& directory);

// `sha256:<hex>` of `bytes`, the form the generate tool reports.
std::string sha256_of(std::string_view bytes);

// A JSON string argument, quoted for a request line.
std::string quoted_arg(const std::string& path);

harness::JsonValue listing_of(mcp::McpSession& session);
const harness::JsonValue* file_entry(const harness::JsonValue& listing, std::string_view path);

} // namespace mcp_project_test

// The generate half, defined in `mcp_project_generate_tests.cpp`.
namespace mcp_project_generate_tests {

void test_generate(const std::filesystem::path& base);
void test_generate_refusals(const std::filesystem::path& base);

} // namespace mcp_project_generate_tests
