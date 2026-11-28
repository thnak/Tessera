// SPDX-License-Identifier: MIT

// `tessera fmt` command behaviour: --check lists and fails, a plain run rewrites, a refused file
// exits 2, and a second run changes nothing.

#include "fmt_run.hpp"
#include "source_format_test_support.hpp"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace source_format_test {

namespace fs = std::filesystem;

namespace {

void write_file(const fs::path& path, const std::string& text) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << text;
}

std::string read_file(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

int run(const std::vector<std::string>& args, std::string& out, std::string& err) {
    std::ostringstream o;
    std::ostringstream e;
    const int code = tessera::fmt_cli::run_fmt(args, o, e);
    out = o.str();
    err = e.str();
    return code;
}

} // namespace

void run_cli_tests() {
    const fs::path dir = fs::current_path() / "tessera_fmt_cli_test";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir / "sub", ec);
    const std::string messy = "export function A(){return <View/>;}\n";
    const std::string canonical = "export function A() {\n  return <View />;\n}\n";
    write_file(dir / "a.tsx", messy);
    write_file(dir / "sub" / "b.tsx", canonical);
    write_file(dir / "notes.txt", "not formatted\n");

    std::string out;
    std::string err;
    check(run({"--check", dir.string()}, out, err) == 1, "--check exits 1 when a file differs");
    check(out.find("a.tsx") != std::string::npos && out.find("b.tsx") == std::string::npos,
          "--check lists only the files that differ");
    check(read_file(dir / "a.tsx") == messy, "--check changes nothing");

    check(run({dir.string()}, out, err) == 0, "a plain run succeeds");
    check(read_file(dir / "a.tsx") == canonical, "a plain run rewrites the file");
    check(read_file(dir / "notes.txt") == "not formatted\n", "only .tsx files are touched");
    check(run({"--check", dir.string()}, out, err) == 0, "--check passes after formatting");

    write_file(dir / "sub" / "bad.tsx", "export function A( {\n");
    check(run({"--check", dir.string()}, out, err) == 2, "a refused file exits 2");
    check(err.find("TESSERA_FMT_SYNTAX") != std::string::npos &&
              err.find("bad.tsx") != std::string::npos,
          "the refusal is reported with the file and the code");
    check(run({(dir / "missing.tsx").string()}, out, err) == 2, "a missing path exits 2");
    check(run({"--bogus"}, out, err) == 2, "an unknown option exits 2");

    write_file(dir / "edit.tsx", "const a = 'x';\n");
    check(run({"--edits-json", (dir / "edit.tsx").string()}, out, err) == 0,
          "--edits-json succeeds");
    check(out.find("\"edits\":[{\"offset\":10,\"length\":1,\"newText\":\"\\\"\"}") !=
                  std::string::npos &&
              out.find("\"diagnostics\":[]") != std::string::npos,
          "--edits-json prints the edit list: " + out);
    check(run({"--edits-json"}, out, err) == 2, "--edits-json without a file exits 2");
    fs::remove_all(dir, ec);
}

} // namespace source_format_test
