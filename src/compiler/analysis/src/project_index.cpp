// SPDX-License-Identifier: MIT

#include "file_indexer.hpp"
#include "tessera/compiler/analysis/project_index.hpp"
#include "utf16_map.hpp"

#include <algorithm>
#include <set>

namespace tessera::compiler::analysis {

namespace {

constexpr int kMaxImportDepth = 8;

bool ends_with(std::string_view text, std::string_view suffix) {
    return text.size() >= suffix.size() && text.substr(text.size() - suffix.size()) == suffix;
}

bool is_source_path(std::string_view path) {
    return path.empty() || ends_with(path, ".tsx") || ends_with(path, ".ts");
}

// Joins `specifier` onto the directory of `from` and folds `.` and `..`.
std::string normalize(std::string_view from, std::string_view specifier) {
    std::vector<std::string> parts;
    auto push = [&parts](std::string_view path) {
        std::size_t pos = 0;
        while (pos <= path.size()) {
            const std::size_t slash = std::min(path.find('/', pos), path.size());
            const std::string_view part = path.substr(pos, slash - pos);
            if (part == "..") {
                if (!parts.empty()) {
                    parts.pop_back();
                }
            } else if (!part.empty() && part != ".") {
                parts.emplace_back(part);
            }
            pos = slash + 1;
        }
    };
    const std::size_t slash = from.rfind('/');
    push(slash == std::string_view::npos ? std::string_view() : from.substr(0, slash));
    push(specifier);
    std::string out;
    for (const auto& part : parts) {
        out += out.empty() ? "" : "/";
        out += part;
    }
    return out;
}

} // namespace

ProjectIndex ProjectIndex::build(std::vector<SourceFile> files) {
    ProjectIndex index;
    for (SourceFile& file : files) {
        index.put_file(std::move(file));
    }
    index.resolve_project();
    return index;
}

void ProjectIndex::put_file(SourceFile file) {
    const FileId id = file.id;
    Entry entry;
    entry.index = index_file(id, file.text, is_source_path(file.path));
    entry.source = std::move(file);
    entries_[id] = std::move(entry);
}

void ProjectIndex::set_file(SourceFile file) {
    put_file(std::move(file));
    resolve_project();
}

void ProjectIndex::remove_file(FileId file) {
    entries_.erase(file);
    resolve_project();
}

bool ProjectIndex::apply_edits(std::span<const TextEdit> edits) {
    std::map<FileId, std::string> work;
    for (const TextEdit& edit : edits) {
        auto it = work.find(edit.file);
        if (it == work.end()) {
            const auto entry = entries_.find(edit.file);
            if (entry == entries_.end()) {
                return false;
            }
            it = work.emplace(edit.file, entry->second.source.text).first;
        }
        std::string& text = it->second;
        const Utf16Map map(text);
        const std::uint64_t end = static_cast<std::uint64_t>(edit.offset) + edit.length;
        if (end > map.to_utf16(static_cast<std::uint32_t>(text.size()))) {
            return false;
        }
        const std::uint32_t from = map.to_byte(edit.offset);
        const std::uint32_t to = map.to_byte(static_cast<std::uint32_t>(end));
        text.replace(from, to - from, edit.new_text);
    }
    for (auto& [id, text] : work) {
        SourceFile source = entries_[id].source;
        source.text = std::move(text);
        put_file(std::move(source));
    }
    resolve_project();
    return true;
}

const FileIndex* ProjectIndex::file(FileId file) const {
    const auto it = entries_.find(file);
    return it == entries_.end() ? nullptr : &it->second.index;
}

std::optional<std::string_view> ProjectIndex::text(FileId file) const {
    const auto it = entries_.find(file);
    if (it == entries_.end()) {
        return std::nullopt;
    }
    return std::string_view(it->second.source.text);
}

const Symbol* ProjectIndex::symbol(std::string_view id) const {
    const auto it = by_id_.find(std::string(id));
    if (it == by_id_.end()) {
        return nullptr;
    }
    return &entries_.at(it->second.first).index.symbols[it->second.second];
}

std::vector<FileId> ProjectIndex::files() const {
    std::vector<FileId> out;
    out.reserve(entries_.size());
    for (const auto& [id, entry] : entries_) {
        out.push_back(id);
    }
    return out;
}

std::optional<FileId> ProjectIndex::resolve_path(FileId from, std::string_view specifier) const {
    if (!specifier.starts_with(".")) {
        return std::nullopt;
    }
    const auto origin = entries_.find(from);
    if (origin == entries_.end()) {
        return std::nullopt;
    }
    const std::string base = normalize(origin->second.source.path, specifier);
    for (const char* suffix : {"", ".tsx", ".ts", "/index.tsx", "/index.ts"}) {
        if (const auto found = paths_.find(base + suffix); found != paths_.end()) {
            return found->second;
        }
    }
    return std::nullopt;
}

const Symbol* ProjectIndex::exported(FileId file, std::string_view name, int depth) const {
    const auto entry = entries_.find(file);
    if (entry == entries_.end() || depth > kMaxImportDepth) {
        return nullptr;
    }
    const FileIndex& index = entry->second.index;
    const auto top_level = [&](std::string_view wanted, bool require_export) -> const Symbol* {
        for (const Symbol& symbol : index.symbols) {
            if (symbol.name == wanted && symbol.parent.empty() &&
                (symbol.exported || !require_export) && symbol.kind != SymbolKind::Param &&
                symbol.kind != SymbolKind::Prop) {
                return &symbol;
            }
        }
        return nullptr;
    };
    if (const Symbol* direct = top_level(name, true)) {
        return direct;
    }
    for (const ReExport& re_export : index.re_exports) {
        if (re_export.exported != name) {
            continue;
        }
        if (re_export.source.empty()) {
            return top_level(re_export.name, false);
        }
        if (const auto target = resolve_path(file, re_export.source)) {
            return exported(*target, re_export.name, depth + 1);
        }
    }
    return nullptr;
}

const Symbol* ProjectIndex::follow(const Symbol* symbol) const {
    for (int depth = 0; symbol != nullptr && symbol->kind == SymbolKind::Import &&
                        symbol->target.resolved() && depth < kMaxImportDepth;
         ++depth) {
        symbol = this->symbol(symbol->target.symbol);
    }
    return symbol;
}

void ProjectIndex::resolve_project() {
    by_id_.clear();
    paths_.clear();
    for (auto& [id, entry] : entries_) {
        paths_.emplace(entry.source.path, id);
        for (std::size_t i = 0; i < entry.index.symbols.size(); ++i) {
            by_id_[entry.index.symbols[i].id] = {id, i};
            Symbol& symbol = entry.index.symbols[i];
            if (symbol.kind == SymbolKind::Import || symbol.kind == SymbolKind::CssModule) {
                symbol.target = Resolution{{}, Unresolved::External};
            }
        }
        for (Reference& reference : entry.index.references) {
            if (!reference.via.empty() && reference.kind == ReferenceKind::Attribute) {
                reference.resolution = Resolution{{}, Unresolved::NeedsType};
            } else if (!reference.via.empty() && reference.kind == ReferenceKind::Export) {
                reference.resolution = Resolution{{}, Unresolved::ImportUnresolved};
            }
        }
    }
    for (auto& [id, entry] : entries_) {
        for (Symbol& symbol : entry.index.symbols) {
            if (symbol.kind != SymbolKind::Import || !symbol.import_source.starts_with(".")) {
                continue;
            }
            symbol.target = Resolution{{}, Unresolved::ImportUnresolved};
            if (const auto target = resolve_path(id, symbol.import_source)) {
                if (const Symbol* found = exported(*target, symbol.import_name, 0)) {
                    symbol.target = Resolution{found->id, {}};
                }
            }
        }
    }
    // Imports are settled, so references that need another file can follow them.
    for (auto& [id, entry] : entries_) {
        for (Reference& reference : entry.index.references) {
            if (reference.via.empty()) {
                continue;
            }
            if (reference.kind == ReferenceKind::Export) {
                if (const auto target = resolve_path(id, reference.via)) {
                    if (const Symbol* found = exported(*target, reference.name, 0)) {
                        reference.resolution = Resolution{found->id, {}};
                    }
                }
            } else if (reference.kind == ReferenceKind::Attribute) {
                const Symbol* tag = this->symbol(reference.via);
                const Symbol* component = follow(tag);
                if (component == nullptr || component->kind != SymbolKind::Component) {
                    if (tag != nullptr && tag->kind == SymbolKind::Import &&
                        !tag->target.resolved()) {
                        reference.resolution.reason = tag->target.reason;
                    }
                    continue;
                }
                const FileIndex& owner = entries_.at(by_id_.at(component->id).first).index;
                for (const Symbol& prop : owner.symbols) {
                    if (prop.kind == SymbolKind::Prop && prop.parent == component->id &&
                        prop.name == reference.name) {
                        reference.resolution = Resolution{prop.id, {}};
                        break;
                    }
                }
                if (!reference.resolution.resolved() &&
                    (reference.name == "key" || reference.name == "ref")) {
                    reference.resolution.reason = Unresolved::Builtin;
                }
            }
        }
    }
}

std::optional<Located> ProjectIndex::locate(FileId file, std::uint32_t offset) const {
    const FileIndex* index = this->file(file);
    if (index == nullptr) {
        return std::nullopt;
    }
    // A caret at the end of a name still names it, but a name that starts there wins.
    for (const bool at_end : {false, true}) {
        const auto hits = [&](const Span& span) {
            return span.to > span.from && span.from <= offset &&
                   (at_end ? offset == span.to : offset < span.to);
        };
        for (const Symbol& symbol : index->symbols) {
            if (hits(symbol.span)) {
                return Located{symbol.id, true, std::nullopt};
            }
        }
        for (const Reference& reference : index->references) {
            if (hits(reference.span)) {
                return Located{reference.resolution.symbol, false, reference};
            }
        }
    }
    return std::nullopt;
}

std::vector<Occurrence> ProjectIndex::references_to(std::string_view target,
                                                    bool include_declaration) const {
    std::set<std::string> ids{std::string(target)};
    for (const auto& [id, entry] : entries_) {
        for (const Symbol& symbol : entry.index.symbols) {
            if (symbol.kind != SymbolKind::Import) {
                continue;
            }
            const Symbol* end = follow(&symbol);
            if (end != nullptr && end != &symbol && end->id == target) {
                ids.insert(symbol.id);
            }
        }
    }
    std::vector<Occurrence> out;
    for (const auto& [id, entry] : entries_) {
        if (include_declaration) {
            for (const Symbol& symbol : entry.index.symbols) {
                if (ids.contains(symbol.id)) {
                    out.push_back(Occurrence{symbol.span, ReferenceKind::Value, true});
                }
            }
        }
        for (const Reference& reference : entry.index.references) {
            if (ids.contains(reference.resolution.symbol)) {
                out.push_back(Occurrence{reference.span, reference.kind, false});
            }
        }
    }
    std::ranges::stable_sort(out, [](const Occurrence& a, const Occurrence& b) {
        return a.span.file != b.span.file ? a.span.file < b.span.file : a.span.from < b.span.from;
    });
    return out;
}

} // namespace tessera::compiler::analysis
