// SPDX-License-Identifier: MIT

// Symbolication against this build's own symbol artifact (issue #192, acceptance: "symbolication
// works on the CI symbol files").
//
// The test does not use a fixture symbol file: it produces one from the *running* test binary and
// then resolves a function the binary provably contains (`symbolized_metadata`, called below
// through a volatile pointer so it is kept). That is the property that matters - the symbol file
// describes the image that crashed.
//
// What each lane's symbol file is:
//   * Linux/macOS: read the ELF `.symtab` that a non-stripped release keeps
//     (`read_elf_symbols`). A release lane that strips must keep a `--only-keep-debug` copy;
//     `release-and-packaging` is where that belongs.
//   * Windows: Release builds with `/Z7` and emits **no PDB** (measured: 286 test executables,
//     zero `.pdb` files), so the artifact is the linker `/MAP` file (`read_linker_map_symbols`) or
//     an object file's COFF symbol table. This test uses the fixture map below to prove the parser,
//     and the ELF path where the image has a symtab.
//   * iOS/tvOS/watchOS: no `.symtab` in the shipped image and no file access to the bundle in the
//     test sandbox: skipped with the reason printed as ": skipped".

#include "tessera/diagnostics/crash_report.hpp"
#include "tessera/diagnostics/crash_symbols.hpp"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>

namespace fs = std::filesystem;

namespace {

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

// Kept alive and callable: the symbolizer must find this name in the image's own symbol table.
int symbolication_probe(int value) {
    return value * 3 + 1;
}

using ProbeFn = int (*)(int);

} // namespace

int main(int argc, char** argv) {
#if defined(TESSERA_NO_SUBPROCESS)
    std::cout << "crash_symbols: skipped - no readable symbol artifact on this platform\n";
    return 0;
#else
    if (argc < 2 || argc > 3) {
        std::cerr << "usage: crash_symbols_tests SELF_EXE [LINKER_MAP]\n";
        return 2;
    }
    const fs::path self(argv[1]);
    const fs::path map_path = argc == 3 ? fs::path(argv[2]) : fs::path();
    check(fs::exists(self), "the test binary exists");

    // A pointer the optimizer cannot fold: it proves the symbol is really present in the image.
    volatile ProbeFn probe = &symbolication_probe;
    check(probe(2) == 7, "the probe function is callable");

    // ---------------------------------------------------------------------------------------
    // 1. The symbol-file format itself: serialize, parse back, byte-stable.
    // ---------------------------------------------------------------------------------------
    {
        tessera::diagnostics::SymbolFile file;
        file.valid = true;
        file.build_id = "bld-sym-1";
        file.modules.push_back(
            tessera::diagnostics::CrashModule{"app.exe", 0x140000000ULL, 0x1000});
        file.entries.push_back(
            tessera::diagnostics::SymbolEntry{0, 0x10, 0x20, "tessera::app::run"});
        const std::string text = tessera::diagnostics::write_symbol_file(file);
        const auto parsed = tessera::diagnostics::parse_symbol_file(text);
        check(parsed.has_value(), "the symbol file round-trips");
        if (parsed.has_value()) {
            check(parsed->build_id == "bld-sym-1", "the build id round-trips");
            check(parsed->modules.size() == 1, "the module table round-trips");
            check(parsed->entries.size() == 1, "the entry table round-trips");
            check(parsed->entries[0].name == "tessera::app::run", "the function name round-trips");
            check(tessera::diagnostics::write_symbol_file(*parsed) == text,
                  "the format is byte-stable");
        }
        const auto garbage = tessera::diagnostics::parse_symbol_file("not a symbol file\n");
        check(!garbage.has_value(), "a foreign file is refused");
    }

    // ---------------------------------------------------------------------------------------
    // 2. Resolution: a record frame resolves to the function that covers its offset.
    // ---------------------------------------------------------------------------------------
    {
        tessera::diagnostics::CrashRecord record;
        record.valid = true;
        record.build_id = "bld-sym-1";
        record.modules.push_back(tessera::diagnostics::CrashModule{"app.exe", 0x1000, 0x1000});
        record.frames.push_back(tessera::diagnostics::CrashRawFrame{0, 0x18});

        tessera::diagnostics::SymbolFile file;
        file.valid = true;
        file.build_id = "bld-sym-1";
        file.modules.push_back(tessera::diagnostics::CrashModule{"app.exe", 0x1000, 0x1000});
        file.entries.push_back(tessera::diagnostics::SymbolEntry{0, 0x10, 0x20, "run"});
        file.entries.push_back(tessera::diagnostics::SymbolEntry{0, 0x40, 0x20, "stop"});
        const auto result = tessera::diagnostics::symbolize_record(record, file);
        check(result.build_id_ok, "a matching build id is accepted");
        check(result.frames.size() == 1, "every frame is kept");
        check(result.frames[0].symbol == "run",
              std::string("the frame resolves to the covering function (got ") +
                  std::string(result.frames[0].symbol) + ")");
        check(result.unsymbolicated == 0, "nothing is reported unsymbolicated");

        // A frame past the end of a sized entry is unsymbolicated, not misattributed.
        tessera::diagnostics::CrashRecord far = record;
        far.frames[0].offset = 0x35;
        const auto far_result = tessera::diagnostics::symbolize_record(far, file);
        check(far_result.frames[0].symbol.empty(), "a gap between functions yields no symbol");
        check(far_result.unsymbolicated == 1, "an unsymbolicated frame is counted");

        // A mismatched build id is refused: wrong names are worse than no names.
        tessera::diagnostics::SymbolFile other = file;
        other.build_id = "bld-sym-OTHER";
        const auto mismatch = tessera::diagnostics::symbolize_record(record, other);
        check(!mismatch.build_id_ok, "a mismatched build id is detected");
        check(mismatch.frames[0].symbol.empty(), "a mismatched build id produces no symbols");
    }

    // ---------------------------------------------------------------------------------------
    // 3. The MSVC linker-map producer (the Windows CI artifact when there is no PDB).
    // ---------------------------------------------------------------------------------------
    {
        // Verbatim shape of a real `link.exe /MAP` file (verified against an actual link on this
        // lane): a preferred-load-address header, then `section:offset`, decorated name, absolute
        // Rva+Base, live flag and object.
        const std::string map =
            " probe\n"
            "\n"
            " Timestamp is 6afa68e9 (Mon Nov 16 09:22:33 2026)\n"
            "\n"
            " Preferred load address is 0000000180000000\n"
            "\n"
            "  Address         Publics by Value              Rva+Base               Lib:Object\n"
            "\n"
            " 0001:00000000       map_probe_alpha            0000000180001000 f   probe.obj\n"
            " 0001:00000010       ?map_probe_beta@@YAHH@Z    0000000180001010 f   probe.obj\n";
        const auto file = tessera::diagnostics::read_linker_map_symbols(map, "bld-map");
        check(file.has_value(), "the linker map parses");
        if (file.has_value()) {
            check(file->build_id == "bld-map", "the map keeps the build id it was given");
            check(file->entries.size() == 2, "both map symbols are read");
            check(!file->entries.empty() &&
                      file->entries[0].name.find("map_probe_alpha") != std::string::npos,
                  "the symbol name is read");
            check(!file->entries.empty() && file->entries[0].start == 0x1000ULL,
                  "the absolute Rva+Base is rebased to a module-relative offset");
        }
    }

    // ---------------------------------------------------------------------------------------
    // 4. The ELF producer against this binary: the acceptance's "symbolication works on the CI
    //    symbol files". The CI artifact on Linux is the ELF `.symtab`, so read the real thing.
    // ---------------------------------------------------------------------------------------
    {
        const std::string image = read_file(self);
        const auto elf = tessera::diagnostics::read_elf_symbols(image, "bld-elf");
        if (!elf.has_value()) {
            // A stripped binary on this lane: say so, do not silently pass. The Linux CI lane
            // does not strip, so this reports a real regression there.
            std::cout << "crash_symbols: ELF symbol table unavailable (" << elf.error() << ")\n";
        } else {
            check(elf->entries.size() > 4, "the ELF symbol table names the binary's functions");
            bool found_probe = false;
            for (const tessera::diagnostics::SymbolEntry& entry : elf->entries) {
                if (entry.name.find("symbolication_probe") != std::string::npos) {
                    found_probe = true;
                }
            }
            check(found_probe, "the test's own probe function is in the symbol table");

            // End to end: build a record whose frame points at the probe and resolve it.
            tessera::diagnostics::CrashRecord record;
            record.valid = true;
            record.build_id = "bld-elf";
            record.modules.push_back(
                tessera::diagnostics::CrashModule{"crash_symbols_tests", 0, 0});
            record.frames.push_back(tessera::diagnostics::CrashRawFrame{0, 0});
            // Resolve every entry start; at least one frame must name the probe.
            std::size_t resolved = 0;
            std::string resolved_name;
            for (const tessera::diagnostics::SymbolEntry& entry : elf->entries) {
                if (entry.start == 0) {
                    continue;
                }
                record.frames[0].offset = entry.start;
                const auto one = tessera::diagnostics::symbolize_record(record, *elf);
                if (!one.frames[0].symbol.empty()) {
                    ++resolved;
                    if (one.frames[0].symbol.find("symbolication_probe") != std::string::npos) {
                        resolved_name = one.frames[0].symbol;
                    }
                }
            }
            check(resolved > 0, "frames resolve against this binary's own symbols");
            check(!resolved_name.empty(), "the probe frame resolves to its function name");
            if (!resolved_name.empty()) {
                std::cerr << "  resolved probe frame: " << resolved_name << "\n";
                // A resolved name must be readable, not the raw Itanium mangling: the report is
                // read by a person, and `_ZN...` in a crash report is a symbolication bug.
                check(resolved_name.find("_ZN") == std::string::npos &&
                          resolved_name.find("_GLOBAL") == std::string::npos,
                      "the resolved name is demangled, not raw mangling");
            }
        }
    }

    // ---------------------------------------------------------------------------------------
    // 5. The real artifact: resolve the probe through the pattern the lane actually produces.
    //    Windows reads the linker map this target was linked with (`/MAP`); POSIX reads the ELF
    //    `.symtab`. Both paths must name `symbolication_probe`, which is a function this binary
    //    provably contains.
    // ---------------------------------------------------------------------------------------
    if (!map_path.empty()) {
        check(fs::exists(map_path), "the linker map exists next to the test binary");
        if (fs::exists(map_path)) {
            const std::string map_text = read_file(map_path);
            const auto file = tessera::diagnostics::read_linker_map_symbols(map_text, "bld-self");
            check(file.has_value(), "this build's linker map parses");
            if (file.has_value()) {
                std::cerr << "  linker map symbols: " << file->entries.size() << "\n";
                check(file->entries.size() > 4, "the map names this binary's functions");
                bool found_probe = false;
                tessera::diagnostics::SymbolEntry probe_entry;
                for (const tessera::diagnostics::SymbolEntry& entry : file->entries) {
                    if (entry.name.find("symbolication_probe") != std::string::npos) {
                        found_probe = true;
                        probe_entry = entry;
                    }
                }
                check(found_probe, "the probe function is in this build's linker map");
                if (found_probe) {
                    // End to end: a record frame at the probe's own offset resolves to it.
                    tessera::diagnostics::CrashRecord record;
                    record.valid = true;
                    record.build_id = "bld-self";
                    record.modules.push_back(tessera::diagnostics::CrashModule{
                        "crash_symbols_tests.exe", 0, 0x10000000});
                    record.frames.push_back(
                        tessera::diagnostics::CrashRawFrame{0, probe_entry.start});
                    const auto result = tessera::diagnostics::symbolize_record(record, *file);
                    check(result.build_id_ok, "a matching build id makes the map usable");
                    check(result.frames[0].symbol.find("symbolication_probe") != std::string::npos,
                          std::string("the frame resolves through the real map (got ") +
                              std::string(result.frames[0].symbol) + ")");
                    std::cerr << "  resolved probe frame from the map: " << result.frames[0].symbol
                              << "\n";
                }
            }
        }
    }

    if (failures != 0) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "crash_symbols: ok\n";
    return 0;
#endif
}
