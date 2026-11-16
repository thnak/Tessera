// SPDX-License-Identifier: MIT

// The MSVC linker-map producer for `tessera.symbols.v1`.
//
// Release builds on the Windows lane use `/Z7` (CMakeLists.txt, CMP0141): debug info is embedded in
// the objects and no PDB is produced for a Release executable - measured on this lane, 286 test
// executables and zero `.pdb` files. The artifact a Windows release *can* ship without a PDB is the
// linker's `/MAP` file, which lists every public symbol with its Rva+Base. This reader turns that
// into the symbol table the resolver consumes; a release lane adds one link option (`/MAP`) to get
// it, which the report states.
//
// A map is only useful together with the image it was linked for, so the caller passes the build id
// and the resolver refuses a mismatch.

#include "tessera/diagnostics/crash_symbols.hpp"

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <cstdint>

namespace tessera::diagnostics {
std::expected<SymbolFile, std::string> read_linker_map_symbols(std::string_view map_text,
                                                               std::string_view build_id) {
    // MSVC /MAP address lines look like this (verified against a real link, not assumed):
    //   0001:00000000       map_probe_alpha            0000000180001000 f   probe.obj
    // The Rva+Base column is a 16-digit *absolute* address because the map records the preferred
    // load address. The parser below converts it to a module-relative offset by subtracting the
    // image base parsed from "Preferred load address is ...", which is what the crash record
    // stores. Lines with no address column (`<absolute>`, `<linker-defined>`) are skipped.
    SymbolFile file;
    file.build_id.assign(build_id);
    std::uint64_t image_base = 0;
    std::size_t cursor = 0;
    while (cursor < map_text.size()) {
        const std::size_t line_end = map_text.find('\n', cursor);
        const std::string_view line =
            map_text.substr(cursor, line_end == std::string_view::npos ? std::string_view::npos
                                                                       : line_end - cursor);
        cursor = line_end == std::string_view::npos ? map_text.size() : line_end + 1;
        if (line.find("Preferred load address is ") != std::string_view::npos) {
            const std::size_t begin = line.find("is ") + 3;
            std::uint64_t parsed_base = 0;
            const auto result =
                std::from_chars(line.data() + begin, line.data() + line.size(), parsed_base, 16);
            if (result.ec == std::errc{}) {
                image_base = parsed_base;
            }
            continue;
        }
        // A real map indents every address line, so trim before looking for `section:offset`.
        std::size_t first = 0;
        while (first < line.size() && (line[first] == ' ' || line[first] == '\t')) {
            ++first;
        }
        const std::string_view trimmed = line.substr(first);
        const std::size_t colon = trimmed.find(':');
        if (colon == std::string_view::npos || colon == 0 || colon > 8) {
            continue;
        }
        bool section_digits = true;
        for (std::size_t index = 0; index < colon; ++index) {
            const char byte = trimmed[index];
            if (byte < '0' || byte > '9') {
                section_digits = false;
                break;
            }
        }
        if (!section_digits) {
            continue;
        }
        // Fields after the section:offset column: decorated name, Rva+Base, live flag, object.
        std::size_t scan = colon + 1;
        auto next_field = [&]() -> std::string_view {
            while (scan < trimmed.size() && (trimmed[scan] == ' ' || trimmed[scan] == '\t')) {
                ++scan;
            }
            const std::size_t begin = scan;
            while (scan < trimmed.size() && trimmed[scan] != ' ' && trimmed[scan] != '\t') {
                ++scan;
            }
            return trimmed.substr(begin, scan - begin);
        };
        (void)next_field(); // section:offset remainder
        const std::string_view decorated = next_field();
        const std::string_view address_text = next_field();
        if (decorated.empty() || address_text.empty()) {
            continue;
        }
        std::uint64_t address = 0;
        const std::size_t parsed = static_cast<std::size_t>(
            std::from_chars(address_text.data(), address_text.data() + address_text.size(), address,
                            16)
                .ptr -
            address_text.data());
        if (parsed != address_text.size()) {
            continue;
        }
        SymbolEntry symbol;
        symbol.module = 0;
        // The map records the preferred-load-address absolute value; the crash record stores a
        // module-relative offset, so rebase here (a map without the header line keeps absolute
        // values, which is useless for matching, so such lines are skipped instead).
        if (image_base == 0 || address < image_base) {
            continue;
        }
        symbol.start = address - image_base;
        symbol.size = 0;
        // `??_C@` is a string literal and `__` a C symbol; only `?a...` function manglings are
        // useful for naming a frame, and a literal would otherwise shadow the real function (both
        // contain the same text).
        std::string_view name = decorated;
        if (name.size() >= 2 && name[0] == '?' && name[1] == '?') {
            continue;
        }
        if (name.size() >= 2 && name[0] == '_' && name[1] == '_') {
            name.remove_prefix(2);
        } else if (!name.empty() && name.front() == '?') {
            name.remove_prefix(1);
        }
        // MSVC mangling: `symbolication_probe@?A0x...@@YAHH@Z`. The name components before the
        // first `@` are the qualified name, innermost first; `?A0x...` is the anonymous namespace.
        std::string readable;
        std::string_view rest = name;
        std::size_t at = rest.find('@');
        if (at == std::string_view::npos) {
            // Undecorated (a C symbol): the whole field is the name.
            readable.assign(name);
        }
        while (at != std::string_view::npos && at != 0) {
            const std::string_view part = rest.substr(0, at);
            if (!part.empty() && part.front() == '?') {
                break; // the `?` type/signature code: the readable components end here
            }
            if (!readable.empty()) {
                readable.insert(0, "::");
            }
            readable.insert(0, std::string(part));
            rest.remove_prefix(at + 1);
            at = rest.find('@');
        }
        if (readable.empty()) {
            continue;
        }
        symbol.name = std::move(readable);
        file.entries.push_back(std::move(symbol));
    }
    if (file.entries.empty()) {
        return std::unexpected("linker map has no address lines");
    }
    std::sort(
        file.entries.begin(), file.entries.end(),
        [](const SymbolEntry& left, const SymbolEntry& right) { return left.start < right.start; });
    file.valid = true;
    return file;
}

} // namespace tessera::diagnostics
