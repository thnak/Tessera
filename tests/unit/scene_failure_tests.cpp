// SPDX-License-Identifier: MIT
//
// The loud half of a rejected source (owner decision: bare `number` is an explicit rule).
// `useState<number>(0)` used to make the native preview CLIs exit 1 with an empty `nodes` array and
// print nothing. These cases pin that:
//
//   * the compiler diagnostic for `number` carries the code, the span of the `number` token and an
//     `int`/`float` hint, and the two accepted spellings (`useState(0)`, `useState<int>(0)`) are
//     clean;
//   * `scene_failure_text()` renders it as `file:line:col: error CODE: message (hint: ...)`;
//   * when a CLI path is given, the real `tessera_scene_preview` prints exactly that on stderr,
//   exits
//     1, and still writes the empty document (the stdout/file and exit-code contract is unchanged).

#include "diagnose.hpp"
#include "scene_failure.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#if !defined(_WIN32)
#include <sys/wait.h>
#endif
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>

namespace {

namespace cli = tessera::compiler::codegen::cli;
namespace fs = std::filesystem;

int failures = 0;

void check(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << "\n";
        ++failures;
    }
}

void check_eq(const std::string& actual, const std::string& expected, std::string_view message) {
    if (actual != expected) {
        std::cerr << "FAIL: " << message << "\n  expected: " << expected
                  << "\n  actual:   " << actual << "\n";
        ++failures;
    }
}

std::string counter_source(std::string_view hook) {
    return "export function Counter() {\n"
           "  const [count, setCount] = " +
           std::string(hook) +
           ";\n"
           "  return <View><Text>{count}</Text></View>;\n"
           "}\n";
}

constexpr std::string_view kExpectedLine =
    "ui/Counter.tsx:2:38: error TESSERA_JSX_TYPE_NUMBER: bare `number` is not a Tessera type "
    "(hint: write `int` or `float`)\n";

std::string read_file(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream text;
    text << in.rdbuf();
    std::string out = text.str();
    // A Windows text-mode stream wrote CRLF; the contract is the lines, not the platform's ending.
    out.erase(std::remove(out.begin(), out.end(), static_cast<char>(13)), out.end());
    return out;
}

void diagnostic_cases() {
    const std::string bad = counter_source("useState<number>(0)");
    const auto records = cli::collect_source_diagnostics(bad, "ui/Counter.tsx");
    check(records.size() == 1, "`useState<number>(0)` reports exactly one diagnostic");
    if (records.size() == 1) {
        const cli::DiagnosticRecord& record = records.front();
        check_eq(record.code, "TESSERA_JSX_TYPE_NUMBER", "the code is the stable one");
        check_eq(record.severity, "error", "it is an error, not a warning");
        check(record.span.line == 2 && record.span.column == 38, "the span is the `number` token");
        check(record.span.length == 6, "the span covers the six bytes of `number`");
        check(record.suggestion.find("`int`") != std::string::npos &&
                  record.suggestion.find("`float`") != std::string::npos,
              "the hint names both Tessera types");
    }

    // The Studio's diagnostics module serializes the same records (`diagnose_source`), so its
    // Problems panel receives this code, span and suggestion (scripts/ci/preview_problems.mjs reads
    // that shape).
    const std::string document = cli::diagnose_source(bad, "ui/Counter.tsx");
    // The writer pretty-prints, so compare with the whitespace removed (the suggestion is checked
    // raw).
    std::string compact;
    for (const char c : document) {
        if (std::isspace(static_cast<unsigned char>(c)) == 0) {
            compact += c;
        }
    }
    check(compact.find("\"code\":\"TESSERA_JSX_TYPE_NUMBER\"") != std::string::npos,
          "the diagnostics document carries the code");
    check(compact.find("\"column\":38") != std::string::npos &&
              compact.find("\"line\":2") != std::string::npos,
          "the diagnostics document carries the span");
    check(document.find("write `int` or `float`") != std::string::npos,
          "the diagnostics document carries the suggestion");

    check_eq(cli::scene_failure_text(bad, "ui/Counter.tsx", "unused"), std::string(kExpectedLine),
             "the printed line is file:line:col: error CODE: message (hint: ...)");

    check(cli::collect_source_diagnostics(counter_source("useState(0)"), "ui/Counter.tsx").empty(),
          "inference (`useState(0)`) is accepted");
    check(cli::collect_source_diagnostics(counter_source("useState<int>(0)"), "ui/Counter.tsx")
              .empty(),
          "a Tessera type hint (`useState<int>(0)`) is accepted");
    check(cli::collect_source_diagnostics(counter_source("useState<float>(0)"), "ui/Counter.tsx")
              .empty(),
          "`useState<float>(0)` is accepted");

    // No front-end diagnostic (a later-stage failure) still prints one line, never nothing.
    check_eq(cli::scene_failure_text(counter_source("useState(0)"), "a.tsx", "layout broke"),
             "a.tsx: error: layout broke" + std::string(1, 10),
             "the fallback line carries the scene error");

    cli::DiagnosticRecord bare;
    bare.code = "TESSERA_X";
    bare.message = "no place";
    check_eq(cli::format_record_line(bare, "f.tsx"), "f.tsx: error TESSERA_X: no place",
             "a record without a span prints the fallback file and no hint clause");
}

#if !defined(TESSERA_CLI_TESTS_NO_SUBPROCESS)
int run_cli(const fs::path& cli_path, const std::string& arguments, const fs::path& log) {
#ifdef _WIN32
    // cmd strips a leading quote from the whole /c command, so invoke through `call`.
    const std::string prefix = "call ";
#else
    const std::string prefix;
#endif
    const std::string command = prefix + "\"" + cli_path.string() + "\" " + arguments + " > \"" +
                                (log.parent_path() / "stdout.log").string() + "\" 2> \"" +
                                log.string() + "\"";
    const int status = std::system(command.c_str());
#ifdef _WIN32
    return status;
#else
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
#endif
}

void cli_case(const fs::path& cli_path, const fs::path& work) {
    std::error_code error;
    fs::remove_all(work, error);
    fs::create_directories(work / "ui");
    const fs::path source = work / "ui" / "Counter.tsx";
    {
        std::ofstream out(source, std::ios::binary | std::ios::trunc);
        out << counter_source("useState<number>(0)");
    }
    const fs::path document = work / "scene.json";
    const fs::path stderr_log = work / "stderr.log";
    const int exit_code = run_cli(cli_path,
                                  "--scene-json \"" + document.string() +
                                      "\" --file ui/Counter.tsx \"" + source.string() + "\"",
                                  stderr_log);
    check(exit_code == 1, "the CLI keeps exit code 1 for a source with no scene");
    check(read_file(document).find("\"nodes\":[]") != std::string::npos,
          "the empty document is still written");
    // The CLI names the file by the path it was given (the document's ids use --file instead).
    std::string expected(kExpectedLine);
    expected.replace(0, std::string_view("ui/Counter.tsx").size(), source.string());
    check_eq(read_file(stderr_log), expected,
             "the CLI prints the diagnostic to stderr and nothing else");
    check(read_file(work / "stdout.log").empty(), "stdout stays empty when a file is the output");
}
#endif

} // namespace

int main(int argc, char** argv) {
    diagnostic_cases();
#if !defined(TESSERA_CLI_TESTS_NO_SUBPROCESS)
    if (argc == 3) {
        cli_case(fs::path(argv[1]).make_preferred(), fs::path(argv[2]));
    } else {
        std::cout << "scene_failure_tests: no CLI given, subprocess case skipped\n";
    }
#else
    (void)argc;
    (void)argv;
#endif
    if (failures != 0) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "scene_failure_tests: ok\n";
    return 0;
}
