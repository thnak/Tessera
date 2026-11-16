// SPDX-License-Identifier: MIT

// Producers for the `tessera.symbols.v1` artifact: one per format a lane can actually emit.
//
// ELF `.symtab`: on Linux the linked binary keeps `.symtab` unless it is stripped, which is the
// "ELF debug info" artifact section 3 names. Reading it is a fixed-layout walk (ELF64
// little-endian, the only shape the Linux lane builds) with no binutils dependency.
//
// MSVC linker map: the Windows lane builds Release with `/Z7` (CMakeLists.txt, CMP0141), which puts
// debug info in the objects and produces no PDB for a Release executable - measured on this lane:
// 286 test executables, zero `.pdb` files. What a Windows release *can* ship without a PDB is the
// linker's `/MAP` file, which lists every external symbol with its Rva+Base. That is what
// `read_linker_map_symbols` consumes. A release lane that wants file/line quality
// (PDB + DbgHelp) has to change the build configuration, which is an owner decision recorded in
// the report rather than taken here.

#include "tessera/diagnostics/crash_symbols.hpp"

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace tessera::diagnostics {

namespace {

// ELF64 structures, read field by field so no unaligned cast or struct packing is involved.
struct ElfHeader {
    std::uint64_t shoff = 0;
    std::uint16_t shentsize = 0;
    std::uint16_t shnum = 0;
    std::uint16_t shstrndx = 0;
    bool ok = false;
};

std::uint16_t read_u16(const char* data, std::size_t offset) {
    const auto* bytes = reinterpret_cast<const unsigned char*>(data + offset);
    return static_cast<std::uint16_t>(bytes[0] | (bytes[1] << 8));
}

std::uint32_t read_u32(const char* data, std::size_t offset) {
    const auto* bytes = reinterpret_cast<const unsigned char*>(data + offset);
    return static_cast<std::uint32_t>(bytes[0]) | (static_cast<std::uint32_t>(bytes[1]) << 8) |
           (static_cast<std::uint32_t>(bytes[2]) << 16) |
           (static_cast<std::uint32_t>(bytes[3]) << 24);
}

std::uint64_t read_u64(const char* data, std::size_t offset) {
    return static_cast<std::uint64_t>(read_u32(data, offset)) |
           (static_cast<std::uint64_t>(read_u32(data, offset + 4)) << 32);
}

ElfHeader parse_elf_header(std::string_view image) {
    ElfHeader header;
    if (image.size() < 64 || image[0] != 0x7F || image[1] != 'E' || image[2] != 'L' ||
        image[3] != 'F') {
        return header;
    }
    if (image[4] != 2 || image[5] != 1) {
        return header; // only ELF64 little-endian is produced by the lanes that ship ELF
    }
    header.shoff = read_u64(image.data(), 0x28);
    header.shentsize = read_u16(image.data(), 0x3A);
    header.shnum = read_u16(image.data(), 0x3C);
    header.shstrndx = read_u16(image.data(), 0x3E);
    header.ok = header.shoff != 0 && header.shentsize == 64 && header.shnum != 0;
    return header;
}

// Symbol names in `.symtab` are mangled; this keeps the human-readable part without implementing
// the Itanium demangler: `_ZN7tessera11diagnostics16crash_metadata_json...` becomes
// `tessera::diagnostics::crash_metadata_json`. Only the leading nested-name prefix is decoded,
// which is enough to name a frame; template arguments and the signature are left off.
//
// The prefix carries `N` (nested) and may carry `L` (internal linkage, i.e. an anonymous namespace
// or an internalized Release function) and a `<n>`-style substitution. `L` is skipped so a local
// function still yields its name instead of the raw mangling; `_GLOBAL__N_1` (the anonymous
// namespace itself) and substitution references are dropped rather than printed as noise.
std::string demangle_prefix(std::string_view name) {
    if (name.size() > 2 && name[0] == '_' && name[1] == 'Z') {
        std::size_t cursor = 2;
        std::string out;
        // `N` opens the nested name; `L` marks internal linkage. Both are prefixes to skip.
        while (cursor < name.size() && (name[cursor] == 'N' || name[cursor] == 'L')) {
            ++cursor;
        }
        while (cursor < name.size()) {
            // `E` closes the nested name; `N` opens it and `L` marks internal linkage. The markers
            // can appear between components (`_ZN12_GLOBAL__N_1L19probeEi`), so skip them wherever
            // they occur rather than only at the start.
            if (name[cursor] == 'E') {
                break;
            }
            if (name[cursor] == 'N' || name[cursor] == 'L') {
                ++cursor;
                continue;
            }
            if (name[cursor] < '1' || name[cursor] > '9') {
                break; // a type code (P, i, c, ...): the enclosing function name is complete
            }
            std::size_t length = 0;
            while (cursor < name.size() && name[cursor] >= '0' && name[cursor] <= '9') {
                length = length * 10 + static_cast<std::size_t>(name[cursor] - '0');
                ++cursor;
            }
            if (length == 0 || cursor + length > name.size()) {
                break;
            }
            const std::string_view component = name.substr(cursor, length);
            cursor += length;
            // The anonymous namespace's own name (`_GLOBAL__N_1`) is an implementation detail; keep
            // the function that lives in it.
            if (component.rfind("_GLOBAL__N", 0) == 0) {
                continue;
            }
            if (!out.empty()) {
                out += "::";
            }
            out.append(component);
        }
        if (!out.empty()) {
            return out;
        }
    }
    return std::string(name);
}

} // namespace

std::expected<SymbolFile, std::string> read_elf_symbols(std::string_view image,
                                                        std::string_view build_id) {
    const ElfHeader header = parse_elf_header(image);
    if (!header.ok) {
        return std::unexpected("not an ELF64 little-endian image");
    }
    const std::size_t table_bytes =
        static_cast<std::size_t>(header.shnum) * static_cast<std::size_t>(header.shentsize);
    if (header.shoff + table_bytes > image.size()) {
        return std::unexpected("ELF section table is truncated");
    }
    // Locate `.symtab`, its linked string table and the section-name table.
    std::size_t symtab = SIZE_MAX;
    for (std::size_t index = 0; index < header.shnum; ++index) {
        const std::size_t entry = static_cast<std::size_t>(header.shoff) + index * 64;
        if (read_u32(image.data(), entry + 4) == 2) { // SHT_SYMTAB
            symtab = entry;
            break;
        }
    }
    if (symtab == SIZE_MAX) {
        return std::unexpected("no ELF symbol table: the image is stripped");
    }
    const std::uint64_t sym_offset = read_u64(image.data(), symtab + 0x18);
    const std::uint64_t sym_size = read_u64(image.data(), symtab + 0x20);
    const std::uint32_t str_index = read_u32(image.data(), symtab + 0x28);
    if (sym_offset + sym_size > image.size() || str_index >= header.shnum) {
        return std::unexpected("ELF symbol table is truncated");
    }
    const std::size_t str_header =
        static_cast<std::size_t>(header.shoff) + static_cast<std::size_t>(str_index) * 64;
    const std::uint64_t str_offset = read_u64(image.data(), str_header + 0x18);
    const std::uint64_t str_size = read_u64(image.data(), str_header + 0x20);
    if (str_offset + str_size > image.size()) {
        return std::unexpected("ELF string table is truncated");
    }
    const char* strings = image.data() + str_offset;

    SymbolFile file;
    file.build_id.assign(build_id);
    for (std::uint64_t entry = sym_offset; entry + 24 <= sym_offset + sym_size; entry += 24) {
        const std::uint32_t name_offset = read_u32(image.data(), static_cast<std::size_t>(entry));
        const std::uint64_t value = read_u64(image.data(), static_cast<std::size_t>(entry) + 8);
        const std::uint64_t size = read_u64(image.data(), static_cast<std::size_t>(entry) + 16);
        const unsigned char info =
            static_cast<unsigned char>(image[static_cast<std::size_t>(entry) + 4]);
        const unsigned char type = info & 0xFU;
        const unsigned char binding = info >> 4U;
        // STT_FUNC with a global, weak or local binding. Local (STB_LOCAL == 0) is *kept*: a
        // function in an anonymous namespace, and one the compiler internalized in a Release
        // build, has local binding and is exactly the frame a crash report has to name. Section
        // symbols and file symbols are excluded by the type, and zero-size entries (labels with no
        // extent) by the size test.
        if (type != 2 || binding > 2 || size == 0) {
            continue;
        }
        if (name_offset >= str_size) {
            continue;
        }
        const std::string_view name(strings + name_offset, str_size - name_offset);
        const std::string_view bare = name.substr(0, name.find('\0'));
        if (bare.empty()) {
            continue;
        }
        SymbolEntry symbol;
        symbol.module = 0;
        symbol.start = value;
        symbol.size = size;
        symbol.name = demangle_prefix(bare);
        file.entries.push_back(std::move(symbol));
    }
    if (file.entries.empty()) {
        return std::unexpected("ELF symbol table has no function symbols");
    }
    std::sort(
        file.entries.begin(), file.entries.end(),
        [](const SymbolEntry& left, const SymbolEntry& right) { return left.start < right.start; });
    file.valid = true;
    return file;
}

} // namespace tessera::diagnostics
