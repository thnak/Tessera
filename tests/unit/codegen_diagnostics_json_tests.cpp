// SPDX-License-Identifier: MIT

// The machine-readable diagnostics document of the codegen CLI (issue #165,
// docs/specs/compiler-diagnostics-v1.md). Runs the real `cli::run()` in process -- no subprocess
// and no %TEMP%, so it works in a confined sandbox -- over three classes of input:
//
//   - a clean file, which must produce an empty `diagnostics` array and exit 0;
//   - a JSX syntax error and a JSX-subset rejection, whose record must agree with the human text
//     the CLI prints (same code, same 1-based line/column) and carry the documented span fields;
//   - a hand-built IR module with a real `ir::validate` error, covering the `IrDiagnostic` half of
//     the conversion, which valid lowering cannot reach by construction;
//
// plus the document's own invariants: valid JSON, determinism, POSIX separators, no `span` member
// for a message that has no source location, and one diagnosticsVersion.

#include "diagnose.hpp"
#include "diagnostic_record.hpp"
#include "diagnostics_json.hpp"
#include "regenerate.hpp"
#include "tessera/compiler/ir/json_value.hpp"
#include "tessera/compiler/ir/module.hpp"
#include "tessera/compiler/ir/validate.hpp"
#include "tessera/harness/json_parse.hpp"
#include "tessera/harness/json_value.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace fs = std::filesystem;
namespace cli = tessera::compiler::codegen::cli;
namespace ir = tessera::compiler::ir;
namespace harness = tessera::harness;

int failures = 0;

void check(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << "\n";
        ++failures;
    }
}

std::string read_file(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

void write_file(const fs::path& path, std::string_view text) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << text;
}

// The document as a decoded value, or null when the text is not strict JSON. Decoding it with the
// repository's own strict parser (harness::parse_json) is the point: a document with a duplicate
// key, a trailing comma or an invalid escape is not a document the page may consume.
std::optional<harness::JsonValue> parse_document(const fs::path& path) {
    const auto parsed = harness::parse_json(read_file(path));
    if (!parsed) {
        return std::nullopt;
    }
    return *parsed;
}

// The first diagnostic object of a document, or nullptr.
const harness::JsonValue* first_diagnostic(const harness::JsonValue& document) {
    const harness::JsonValue* list = document.find("diagnostics");
    if (list == nullptr || !list->is_array()) {
        return nullptr;
    }
    const harness::JsonValue::Array* items = list->as_array();
    if (items == nullptr || items->empty()) {
        return nullptr;
    }
    return &items->front();
}

std::string string_member(const harness::JsonValue& object, std::string_view key) {
    const harness::JsonValue* value = object.find(key);
    if (value == nullptr) {
        return std::string();
    }
    const std::optional<std::string_view> text = value->as_string();
    return text ? std::string(*text) : std::string();
}

long long integer_member(const harness::JsonValue& object, std::string_view key) {
    const harness::JsonValue* value = object.find(key);
    if (value == nullptr) {
        return -1;
    }
    const std::optional<std::int64_t> integer = value->as_integer();
    return integer ? static_cast<long long>(*integer) : -1;
}

// Runs the CLI in process with one input and `--diagnostics-json`, and returns the document text.
std::string run_diagnostics(const fs::path& input, const fs::path& document_path) {
    cli::Options options;
    options.inputs.push_back(input.string());
    options.diagnostics_json_path = document_path.string();
    const cli::Outcome outcome = cli::run(options);
    // The exit code is asserted by the caller through the returned text's content; this helper only
    // guarantees the document exists so a missing file fails as one clear message.
    check(fs::exists(document_path), "the run wrote the diagnostics document");
    return read_file(document_path);
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: codegen_diagnostics_json_tests WORK_DIR\n";
        return 2;
    }
    // A directory under the build tree, never %TEMP%: a confined sandbox denies child processes
    // writing there, and the caller (CTest) passes a path inside the build directory.
    const fs::path work(argv[1]);
    std::error_code error;
    fs::remove_all(work, error);
    fs::create_directories(work);

    // --- 1. A clean file: an empty array, exit 0, and the primary file named in the document. ---
    {
        const fs::path input = work / "clean" / "Clean.tsx";
        write_file(input, "export function Clean() {\n  return <View />;\n}\n");
        const fs::path document = work / "clean.json";
        cli::Options options;
        options.inputs.push_back(input.string());
        options.diagnostics_json_path = document.string();
        const cli::Outcome outcome = cli::run(options);
        check(outcome.exit_code == 0, "a clean file exits 0");
        const auto parsed = parse_document(document);
        check(parsed.has_value(), "the clean document is strict JSON");
        if (parsed) {
            check(integer_member(*parsed, "diagnosticsVersion") == 1,
                  "the clean document carries diagnosticsVersion 1");
            const harness::JsonValue* list = parsed->find("diagnostics");
            check(list != nullptr && list->is_array() && list->array().empty(),
                  "a clean file has an empty diagnostics array");
            check(string_member(*parsed, "file") == input.generic_string(),
                  "the document names the primary file with POSIX separators");
        }
        check(read_file(document) == run_diagnostics(input, work / "clean-again.json"),
              "two runs of the same input produce the same document");
    }

    // --- 2. A JSX syntax error: the record must match the printed diagnostic. ---
    {
        const fs::path input = work / "syntax" / "Broken.tsx";
        write_file(input, "export function Broken() {\n  return <View>;\n}\n");
        const fs::path document = work / "syntax.json";
        std::string text;
        {
            cli::Options options;
            options.inputs.push_back(input.string());
            options.diagnostics_json_path = document.string();
            const cli::Outcome outcome = cli::run(options);
            check(outcome.exit_code == 1, "a JSX error exits 1");
            // The human form the CLI prints and the record must not diverge: the record is filled
            // where the diagnostic was in hand, and the text is formatted from the same object.
            check(!outcome.messages.empty(), "the run reported at least one message");
            text = outcome.messages.empty() ? std::string() : outcome.messages.front().text;
        }
        const auto parsed = parse_document(document);
        check(parsed.has_value(), "the syntax-error document is strict JSON");
        if (parsed) {
            const harness::JsonValue* diagnostic = first_diagnostic(*parsed);
            check(diagnostic != nullptr, "the syntax-error document lists the diagnostic");
            if (diagnostic != nullptr) {
                const std::string code = string_member(*diagnostic, "code");
                check(!code.empty() && code.rfind("TESSERA_", 0) == 0,
                      "the record carries a stable TESSERA_* code");
                check(string_member(*diagnostic, "severity") == "error",
                      "a syntax error is severity `error`");
                const harness::JsonValue* span = diagnostic->find("span");
                check(span != nullptr, "the record carries a span");
                if (span != nullptr) {
                    // The text is `file:line:column: error[CODE]: message`, so the printed
                    // position is the record's position; comparing them is the acceptance that the
                    // machine-readable and human forms say the same thing.
                    const std::string position = std::to_string(integer_member(*span, "line")) +
                                                 ":" +
                                                 std::to_string(integer_member(*span, "column"));
                    check(text.find(position) != std::string::npos,
                          "the printed diagnostic carries the record's line:column");
                    check(text.find(code) != std::string::npos,
                          "the printed diagnostic carries the record's code");
                    check(integer_member(*span, "line") >= 1 &&
                              integer_member(*span, "column") >= 1,
                          "the span's line and column are 1-based");
                    check(integer_member(*span, "offset") >= 0, "the span carries a byte offset");
                    check(string_member(*span, "file") == input.generic_string(),
                          "the span names the file with POSIX separators");
                    check(string_member(*span, "file").find('\\') == std::string::npos,
                          "no backslash survives into the span's file");
                }
            }
        }
    }

    // --- 3. A JSX-subset rejection that lowers far enough to be a different diagnostic class. ---
    {
        const fs::path input = work / "subset" / "Tag.tsx";
        write_file(input, "export function Tag() {\n  return <NotAThing />;\n}\n");
        const fs::path document = work / "subset.json";
        std::string code;
        {
            cli::Options options;
            options.inputs.push_back(input.string());
            options.diagnostics_json_path = document.string();
            const cli::Outcome outcome = cli::run(options);
            check(outcome.exit_code == 1, "an unknown tag exits 1");
        }
        const auto parsed = parse_document(document);
        check(parsed.has_value(), "the subset document is strict JSON");
        if (parsed) {
            const harness::JsonValue* diagnostic = first_diagnostic(*parsed);
            check(diagnostic != nullptr, "the subset document lists the diagnostic");
            if (diagnostic != nullptr) {
                code = string_member(*diagnostic, "code");
                check(code.rfind("TESSERA_IR_", 0) == 0,
                      "lowering a rejected tag reports a TESSERA_IR_* code");
                const std::string suggestion = string_member(*diagnostic, "suggestion");
                check(!suggestion.empty(), "a diagnosed record keeps its suggestion");
            }
        }
    }

    // --- 4. A message with no source location: no invented span. ---
    {
        // Missing --out with no emission flag is a usage error: it has a message but no source
        // position, and the document must not place it at 1:1 of the input.
        const fs::path input = work / "clean" / "Clean.tsx";
        const fs::path document = work / "usage.json";
        cli::Options options;
        options.inputs.push_back(input.string());
        options.diagnostics_json_path = document.string();
        options.emit_ir_path = "x"; // a second output mode: rejected as a usage error
        const cli::Outcome outcome = cli::run(options);
        check(outcome.exit_code == 2, "two output modes exit 2");
        const auto parsed = parse_document(document);
        check(parsed.has_value(), "the usage-error document is strict JSON");
        if (parsed) {
            const harness::JsonValue* diagnostic = first_diagnostic(*parsed);
            check(diagnostic != nullptr, "the usage error is still recorded");
            if (diagnostic != nullptr) {
                check(diagnostic->find("span") == nullptr,
                      "a message with no location carries no span member");
                check(string_member(*diagnostic, "severity") == "error",
                      "a usage error is severity `error`");
            }
        }
    }

    // --- 5. The IR validator half of the conversion, which valid lowering cannot reach. ---
    {
        // A component with no name fails TESSERA_IR_MALFORMED in `ir::validate`; the record must
        // carry that code and the component span verbatim. `ir::Component` holds owning pointers
        // (Node::fallback/item_template), so it is moved rather than copied.
        ir::IrModule module;
        module.name = "test";
        ir::Component component;
        component.name = ""; // the validator's condition: a component has no name
        component.source.file = "ui/Counter.tsx";
        component.source.line = 3;
        component.source.column = 5;
        component.source.offset = 20;
        component.source.length = 10;
        component.source.end_line = 7;
        component.source.end_column = 2;
        module.components.push_back(std::move(component));

        const std::vector<ir::IrDiagnostic> diagnostics = ir::validate(module);
        check(!diagnostics.empty(), "the hand-built module fails validation");
        if (!diagnostics.empty()) {
            const cli::DiagnosticRecord record = cli::to_record(diagnostics.front());
            check(record.code == diagnostics.front().code, "the record keeps the validator's code");
            check(record.severity == "error", "an IR validation error is severity `error`");
            check(record.message == diagnostics.front().message,
                  "the record keeps the validator's message");
            check(record.span.has_location(), "the validator's span survives the conversion");
            check(record.span.file == "ui/Counter.tsx", "the record keeps the span's file");
            check(record.span.line == 3 && record.span.column == 5,
                  "the record keeps the span's 1-based start");
            check(record.span.offset == 20 && record.span.length == 10,
                  "the record keeps the span's byte offset and length");
            // And it serializes as one entry of a valid document.
            const std::string text = cli::write_diagnostics_json("ui/Counter.tsx", {record});
            const auto parsed = harness::parse_json(text);
            check(parsed.has_value(), "a single-record document is strict JSON");
            if (parsed) {
                const harness::JsonValue* entry = first_diagnostic(*parsed);
                check(entry != nullptr && string_member(*entry, "code") == record.code,
                      "the serialized record round-trips its code");
            }
        }
    }

    // --- 6. A warning keeps its severity and its code. ---
    {
        cli::DiagnosticRecord record;
        record.code = "TESSERA_CG_MISSING_BINDING";
        record.severity = "warning";
        record.message = "component `Demo` needs `void sync()`";
        const std::string text = cli::write_diagnostics_json("ui/Demo.tsx", {record});
        const auto parsed = harness::parse_json(text);
        check(parsed.has_value(), "a warning-only document is strict JSON");
        if (parsed) {
            const harness::JsonValue* entry = first_diagnostic(*parsed);
            check(entry != nullptr && string_member(*entry, "severity") == "warning",
                  "a warning keeps severity `warning`");
            check(entry != nullptr && entry->find("span") == nullptr,
                  "a warning without a location carries no span");
        }
    }

    // --- 7. The in-memory path the preview page uses agrees with the file-driven CLI. ---
    //
    // This is the locally-runnable half of the P1 acceptance. The page calls `diagnose_source()`;
    // the CLI calls `run()` + `--diagnostics-json`. Both now go through the same serializer, so
    // what is left to drift is the *stage order* between the two drivers -- which stage stops first
    // and with which diagnostic. Running both over one corpus and comparing the documents byte for
    // byte is that check, and it runs on this box without emsdk (the Wasm artifact is the same code
    // behind a C export, so the CI gate `wasm_diagnostics_parity.py` then only has to prove the
    // export). The cases are the ones in scripts/ci/wasm_diagnostics_parity.py, kept short here.
    {
        struct Case {
            const char* name;
            const char* file; // project-relative, POSIX: both drivers must see the same string
            const char* source;
            int minimum; // anti-vacuity: a diagnosing case must diagnose
        };
        const Case cases[] = {
            {"clean", "ui/Clear.tsx", "export function Clear() {\n  return <View />;\n}\n", 0},
            {"jsx-lex", "ui/Lex.tsx", "export function Lex() {\n  return <View>}</View>;\n}\n", 1},
            {"jsx-parse", "ui/Parse.tsx", "export function Parse() {\n  return <View></Text>;\n}\n",
             1},
            {"lowering", "ui/Tag.tsx", "export function Tag() {\n  return <NotAThing />;\n}\n", 1},
            {"react-rules", "ui/Hook.tsx",
             "export function Hook() {\n  const v = useAnimatedValue(0, 12);\n  return <View "
             "/>;\n}\n",
             1},
            {"span-not-first-line", "ui/Deep.tsx",
             "// a leading comment\n// and a second one\nexport function Deep() {\n  return <Nope "
             "/>;\n}\n",
             1},
            {"utf8-offset", "ui/Utf8.tsx",
             // "café naïve über": a multi-byte line before the error, so the span's byte offset and
             // its code-point column must both be right. The escapes are split so the following
             // letter is not absorbed into a hex escape.
             "// caf\xC3\xA9 na\xC3\xAFve \xC3\xBC"
             "ber\nexport function Utf8() {\n  return <Nope />;\n}\n",
             1},
        };
        for (const Case& test_case : cases) {
            // The CLI is handed a relative path with its working directory at the case root, so the
            // `file` in every span is the same string the in-memory call passes. The document path
            // is absolute so it is not written inside the tree being compiled.
            const fs::path case_dir = work / "parity" / test_case.name;
            const fs::path source_path = case_dir / test_case.file;
            write_file(source_path, test_case.source);
            const fs::path document = work / "parity" / (std::string(test_case.name) + ".json");

            cli::Options options;
            options.inputs.emplace_back(test_case.file);
            options.diagnostics_json_path = document.string();
            const auto previous = fs::current_path();
            fs::current_path(case_dir);
            const cli::Outcome outcome = cli::run(options);
            fs::current_path(previous);

            check(outcome.exit_code <= 1, "the CLI run of a parity case is not a setup failure");

            const std::string from_cli = read_file(document);
            const std::string from_memory = cli::diagnose_source(test_case.source, test_case.file);
            check(from_cli == from_memory,
                  std::string("the in-memory and CLI documents agree for `") + test_case.name +
                      "`");
            if (from_cli != from_memory) {
                std::cerr << "  cli:    " << from_cli << "\n  memory: " << from_memory << "\n";
            }

            // Anti-vacuity: the case must actually diagnose on both sides, or the comparison above
            // would be "two empty documents match" and prove nothing.
            const auto parsed = harness::parse_json(from_memory);
            check(parsed.has_value(),
                  std::string("the in-memory document of `") + test_case.name + "` is JSON");
            if (parsed) {
                const harness::JsonValue* list = parsed->find("diagnostics");
                const std::size_t count =
                    list != nullptr && list->as_array() != nullptr ? list->as_array()->size() : 0;
                check(static_cast<int>(count) >= test_case.minimum,
                      std::string("`") + test_case.name + "` reports at least " +
                          std::to_string(test_case.minimum) + " diagnostic(s)");
            }
        }
    }

    fs::remove_all(work, error);
    if (failures != 0) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "codegen diagnostics json tests passed\n";
    return 0;
}
