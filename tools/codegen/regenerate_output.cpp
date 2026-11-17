// SPDX-License-Identifier: MIT

// The output half of the codegen CLI (regenerate_output.hpp): generate the C++ of a lowered module,
// write it, check it or dry-run it, and scaffold the developer classes ADR-040 says are written
// once and never edited. The input half (read, parse, analyse, lower, validate, report) stays in
// regenerate.cpp.

#include "diagnostic_record.hpp"
#include "emit_library_components.hpp"
#include "project_identity.hpp"
#include "regenerate_output.hpp"
#include "tessera/compiler/ids/manifest.hpp"
#include "tessera/compiler/ir/ir_json.hpp"

#include <filesystem>
#include <fstream>
#include <optional>
#include <set>
#include <string_view>
#include <utility>
#include <vector>

namespace tessera::compiler::codegen::cli::internal {

namespace fs = std::filesystem;
namespace ir = tessera::compiler::ir;

namespace {

bool safe_file_name(std::string_view name) {
    return !name.empty() && name.find('/') == std::string_view::npos &&
           name.find('\\') == std::string_view::npos && name.find("..") == std::string_view::npos;
}

bool generate_files(const tessera::compiler::ir::IrModule& module,
                    std::span<const StylesheetEmbed> embeds,
                    const std::optional<EmbeddedTheme>& theme, const Options& options,
                    std::vector<GeneratedFile>& files, Outcome& outcome, std::string_view input) {
    const auto generated = generate(module, embeds, theme, CodegenOptions{options.line_directives});
    if (!generated) {
        const CodegenErrors& errors = generated.error();
        report(outcome, true,
               std::string(input) + ": " +
                   (errors.empty() ? std::string("codegen failed") : errors.front()));
        outcome.exit_code = 1;
        return false;
    }
    files = *generated;
    for (const std::string& warning : codegen_warnings(module, embeds)) {
        report(outcome, false, std::string(input) + ": warning: " + warning);
    }
    return true;
}

bool is_word_char(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

// True when `text` contains `member(` as a whole word, i.e. a declaration, definition or call.
bool defines_member(std::string_view text, std::string_view member) {
    const std::string needle = std::string(member) + "(";
    for (std::size_t at = text.find(needle); at != std::string_view::npos;
         at = text.find(needle, at + 1)) {
        if (at == 0 || !is_word_char(text[at - 1])) {
            return true;
        }
    }
    return false;
}

// ADR-040 decision item 6: the developer files are written once, never edited afterwards. An
// existing developer file is only checked: a binding it does not define yet is reported with the
// stub to add, and a stub marker (`// bind::<id>`) whose binding left the TSX is reported unused.
void handle_developer_files(const tessera::compiler::ir::IrModule& module,
                            const fs::path& output_dir, bool check_only, bool verbose,
                            Outcome& outcome) {
    const auto stubs = scaffold(module);
    if (!stubs) {
        report(outcome, true, "tessera_codegen: " + stubs.error().front());
        outcome.exit_code = 1;
        return;
    }
    std::string error;
    const std::vector<RequiredBinding> required = required_bindings(module, error);
    std::set<std::string> components;
    for (const GeneratedFile& stub : *stubs) {
        components.insert(fs::path(stub.path).stem().string());
    }
    for (const std::string& component : components) {
        const fs::path header = output_dir / (component + ".hpp");
        const fs::path source = output_dir / (component + ".cpp");
        const bool exists = fs::exists(header) || fs::exists(source);
        if (!exists) {
            for (const GeneratedFile& stub : *stubs) {
                if (fs::path(stub.path).stem().string() == component) {
                    outcome.scaffolded.push_back(stub.path);
                }
            }
            if (check_only) {
                continue; // developer files are not generated content: nothing to drift from
            }
            for (const GeneratedFile& stub : *stubs) {
                if (fs::path(stub.path).stem().string() != component) {
                    continue;
                }
                std::ofstream out(output_dir / stub.path, std::ios::binary | std::ios::trunc);
                out << stub.text;
                if (!out) {
                    report(outcome, true,
                           "tessera_codegen: cannot write `" + (output_dir / stub.path).string() +
                               "`");
                    outcome.exit_code = 2;
                    return;
                }
                if (verbose) {
                    report(outcome, false, (output_dir / stub.path).string() + ": scaffolded");
                }
            }
            continue;
        }
        const std::string text =
            read_file(header).value_or("") + "\n" + read_file(source).value_or("");
        std::set<std::string> ids;
        for (const RequiredBinding& binding : required) {
            if (binding.component != component) {
                continue;
            }
            ids.insert(binding.id);
            if (!defines_member(text, binding.member)) {
                report(outcome, false,
                       header.string() + ": warning: TESSERA_CG_MISSING_BINDING: component `" +
                           component + "` needs `" + binding.signature + "` for " + binding.id +
                           " (" + binding.source + "); add it to the developer class");
            }
        }
        std::size_t line_start = 0;
        while (line_start < text.size()) {
            const std::size_t line_end = text.find('\n', line_start);
            std::string_view line(text.data() + line_start,
                                  (line_end == std::string::npos ? text.size() : line_end) -
                                      line_start);
            line_start = line_end == std::string::npos ? text.size() : line_end + 1;
            while (!line.empty() && (line.front() == ' ' || line.front() == '\t')) {
                line.remove_prefix(1);
            }
            constexpr std::string_view kMarker = "// bind::";
            if (line.substr(0, kMarker.size()) != kMarker) {
                continue;
            }
            std::string id(line.substr(3));
            id = id.substr(0, id.find_first_of(" ("));
            if (ids.count(id) == 0) {
                report(outcome, false,
                       header.string() + ": warning: TESSERA_CG_BINDING_UNUSED: `" + id +
                           "` is no longer in the TSX of component `" + component +
                           "`; its developer member is now unused");
            }
        }
    }
}

} // namespace

void write_emitted_file(const tessera::compiler::ir::IrModule& module, const Options& options,
                        Outcome& outcome) {
    const std::string& target_path = options.emit_library_cpp_path.empty()
                                         ? options.emit_ir_path
                                         : options.emit_library_cpp_path;
    const fs::path target(target_path);
    if (target.has_parent_path()) {
        std::error_code directory_error;
        fs::create_directories(target.parent_path(), directory_error);
        if (directory_error) {
            report(outcome, true,
                   "tessera_codegen: cannot create `" + target.parent_path().string() +
                       "`: " + directory_error.message());
            outcome.exit_code = 2;
            return;
        }
    }
    std::string text;
    if (!options.emit_ir_path.empty()) {
        text = ir::write_ir_json(module);
    } else {
        std::string why;
        if (!emit_library_components_cpp(module, target.string(), why)) {
            report(outcome, true, "tessera_codegen: " + why);
            outcome.exit_code = 1;
            return;
        }
        if (options.verbose) {
            report(outcome, false, target.string() + ": written");
        }
        return;
    }
    std::ofstream out(target, std::ios::binary | std::ios::trunc);
    out << text;
    if (!out) {
        report(outcome, true, "tessera_codegen: cannot write `" + target.string() + "`");
        outcome.exit_code = 2;
        return;
    }
    if (options.verbose) {
        report(outcome, false, target.string() + ": written");
    }
    return;
}

void write_generated_output(const tessera::compiler::ir::IrModule& module,
                            std::span<const StylesheetEmbed> embeds,
                            const std::optional<EmbeddedTheme>& theme, const Options& options,
                            const std::string& salt, Outcome& outcome) {
    std::vector<GeneratedFile> files;
    if (!generate_files(module, embeds, theme, options, files, outcome, options.inputs.front())) {
        return;
    }

    if (!salt.empty()) {
        // The manifest resolves the numeric ids to names and sources; it is generated, covered by
        // --check, and has no protected regions.
        files.push_back(
            {"automation.manifest.json", tessera::compiler::ids::write_manifest(module, salt)});
    }

    const fs::path output_dir(options.output_dir);
    std::error_code error;
    if (!options.dry_run) {
        fs::create_directories(output_dir, error);
    }
    if (error) {
        report(outcome, true,
               "tessera_codegen: cannot create output directory `" + output_dir.string() +
                   "`: " + error.message());
        outcome.exit_code = 2;
        return;
    }

    bool drift = false;
    for (const GeneratedFile& file : files) {
        if (!safe_file_name(file.path)) {
            report(outcome, true,
                   "tessera_codegen: refusing an unsafe generated path `" + file.path + "`");
            outcome.exit_code = 2;
            return;
        }
        const fs::path target = output_dir / file.path;
        const std::optional<std::string> existing =
            fs::exists(target) ? read_file(target) : std::nullopt;
        std::string content = file.text;
        if (options.dry_run || options.collect_generated) {
            outcome.generated.push_back(GeneratedEntry{file.path, content,
                                                       !existing ? GeneratedStatus::New
                                                       : *existing == content
                                                           ? GeneratedStatus::Unchanged
                                                           : GeneratedStatus::Changed});
        }
        if (options.dry_run) {
            if (!existing || *existing != content) {
                outcome.drift = true;
            }
            continue;
        }
        if (existing) {
            // The generated base and the manifest are rewritten wholesale: they carry no
            // developer-owned content, only the scaffolded `<Component>.hpp/.cpp` does, and those
            // are handled by handle_developer_files() below and never written here.
            if (*existing == content) {
                if (options.verbose) {
                    report(outcome, false, target.string() + ": unchanged");
                }
                continue;
            }
        }
        if (options.check_only) {
            drift = true;
            report(outcome, true, target.string() + ": out of date");
            continue;
        }
        std::ofstream out(target, std::ios::binary | std::ios::trunc);
        if (!out) {
            report(outcome, true, "tessera_codegen: cannot write `" + target.string() + "`");
            outcome.exit_code = 2;
            return;
        }
        out << content;
        if (options.verbose) {
            report(outcome, false, target.string() + ": written");
        }
    }
    if (outcome.exit_code == 0) {
        handle_developer_files(module, output_dir, options.check_only || options.dry_run,
                               options.verbose, outcome);
    }
    if (drift) {
        outcome.exit_code = 1;
    }
    return;
}

} // namespace tessera::compiler::codegen::cli::internal
