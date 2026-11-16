// SPDX-License-Identifier: MIT

// Symbolication for crash reports (docs/specs/crash-reporting-v1.md section 3): turn the
// module-relative frames a crash record stores into function names, using the symbol artifact the
// build produced.
//
// The artifact is a text symbol file in this project's own format, `tessera.symbols.v1`:
//
//     tessera.symbols.v1
//     build=<build id>
//     module=<module basename>|<image base>|<image size>
//     func=<module index>|<start offset>|<size>|<demangled name>
//
// It is produced from whatever the platform has, without a new dependency:
//   * `write_elf_symbols` reads a linked ELF's `.symtab` (the Linux/macOS debug-info artifact);
//   * `write_linker_map_symbols` reads an MSVC `/MAP` file (what a PDB-less Windows release has).
// Both writers are plain byte parsers in this library: no binutils, no DbgHelp, no PDB parser.
//
// A symbol file that does not name the record's build id is *refused* rather than used: a
// mismatched build produces confidently wrong names, which is worse than "unsymbolicated"
// (section 6.1: "Symbol mismatch -> group by module/offset, mark unsymbolicated").

#pragma once

#include "tessera/diagnostics/crash.hpp"
#include "tessera/diagnostics/crash_report.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <string>
#include <string_view>
#include <vector>

namespace tessera::diagnostics {

struct SymbolEntry {
    std::size_t module = 0;
    std::uint64_t start = 0; // module-relative
    std::uint64_t size = 0;  // 0 = unknown; the entry then covers up to the next start
    std::string name;
};

struct SymbolFile {
    bool valid = false;
    std::string build_id;
    std::vector<CrashModule> modules;
    std::vector<SymbolEntry> entries;
};

// Parses the text format above. A malformed line invalidates the file instead of yielding a
// partially-trusted table.
[[nodiscard]] std::expected<SymbolFile, std::string> parse_symbol_file(std::string_view text);

// Serializes a `SymbolFile` (the writer both producers below share).
[[nodiscard]] std::string write_symbol_file(const SymbolFile& file);

// Produces a symbol file from an ELF image's `.symtab`. Returns an error string when the file is
// not an ELF, has no symbol table (stripped), or is truncated. `build_id` is copied into the
// header so a later mismatch is detectable.
[[nodiscard]] std::expected<SymbolFile, std::string> read_elf_symbols(std::string_view image,
                                                                      std::string_view build_id);

// Produces a symbol file from an MSVC linker map (`/MAP`). Reads the "Rva+Base" address and the
// decorated name, leaves `size` at 0 (the map does not carry sizes) and keeps the module index at
// 0, so callers add the module row themselves.
[[nodiscard]] std::expected<SymbolFile, std::string>
read_linker_map_symbols(std::string_view map_text, std::string_view build_id);

// Resolves every frame of `record` against `file`. A frame with no matching entry keeps an empty
// symbol and is counted in `unsymbolicated`. `build_id_ok` is false when the file's build id does
// not match the record's: the caller must report that instead of using the names.
struct SymbolizeResult {
    std::vector<CrashFrame> frames;
    std::size_t unsymbolicated = 0;
    bool build_id_ok = false;
};

[[nodiscard]] SymbolizeResult symbolize_record(const CrashRecord& record, const SymbolFile& file);

// Fills `CrashMetadata`'s frame span from a `SymbolizeResult`. The span points into `frames`, so
// the result must outlive the metadata it feeds.
[[nodiscard]] CrashMetadata symbolized_metadata(const CrashRecord& record,
                                                const SymbolizeResult& result);

} // namespace tessera::diagnostics
