// SPDX-License-Identifier: MIT

#include "completion_lex.hpp"
#include "css_scan.hpp"
#include "tessera/compiler/analysis/completion.hpp"
#include "tessera/compiler/analysis/symbol_json.hpp"
#include "utf16_map.hpp"

#include <algorithm>
#include <cctype>
#include <map>
#include <optional>
#include <set>
#include <string_view>

namespace tessera::compiler::analysis {

namespace {

using namespace lex;

class Completer {
public:
    Completer(const ProjectIndex& index, FileId file, const FileIndex& file_index, Caret caret,
              std::uint32_t caret_unit)
        : index_(index),
          file_(file),
          file_index_(file_index),
          caret_(caret),
          caret_unit_(caret_unit) {}

    CompletionList run(const Utf16Map& map) {
        list_.from = map.to_utf16(static_cast<std::uint32_t>(caret_.word));
        list_.to = map.to_utf16(static_cast<std::uint32_t>(caret_.end));
        const std::string_view line = caret_.before_on_line();
        if (import_path(map)) {
            return finish();
        }
        const std::size_t comment = line.find("//");
        if (comment != std::string_view::npos) {
            if (caret_.text.substr(0, caret_.word).ends_with("bind::")) {
                bind_members();
            }
            return finish();
        }
        if (odd_count(line, '"') || odd_count(line, '\'') || odd_count(line, '`')) {
            return finish();
        }
        const std::size_t prev = skip_space_back(caret_.text, caret_.word);
        if (caret_.word > 0 && caret_.text[caret_.word - 1] == '.') {
            css_classes();
            return finish();
        }
        if (is_tag_start(prev)) {
            tags();
            return finish();
        }
        if (const auto tag = open_tag(caret_.text, caret_.word)) {
            if (prev > 0 && caret_.text[prev - 1] == '=') {
                return finish();
            }
            attributes(*tag);
            return finish();
        }
        expression();
        return finish();
    }

private:
    [[nodiscard]] bool is_tag_start(std::size_t prev) const {
        if (caret_.word != prev || prev == 0) {
            return false;
        }
        std::size_t lt = prev - 1;
        if (caret_.text[lt] == '/' && lt > 0 && caret_.text[lt - 1] == '<') {
            --lt;
        }
        if (caret_.text[lt] != '<') {
            return false;
        }
        const std::size_t before = skip_space_back(caret_.text, lt);
        return before == 0 ||
               (!ident_byte(caret_.text[before - 1]) && caret_.text[before - 1] != ')' &&
                caret_.text[before - 1] != ']') ||
               word_before(caret_.text, before) == "return";
    }

    bool import_path(const Utf16Map& map) {
        const std::string_view line = caret_.before_on_line();
        for (const char quote : {'"', '\''}) {
            if (!odd_count(line, quote)) {
                continue;
            }
            const std::size_t open = line_start_of(line) + line.rfind(quote);
            const std::size_t head = skip_space_back(caret_.text, open);
            const std::string_view word = word_before(caret_.text, head);
            if (word != "from" && word != "import") {
                return false;
            }
            std::size_t close = caret_.at;
            while (close < caret_.text.size() && caret_.text[close] != quote &&
                   caret_.text[close] != '\n') {
                ++close;
            }
            list_.context = "importPath";
            list_.from = map.to_utf16(static_cast<std::uint32_t>(open + 1));
            list_.to = map.to_utf16(static_cast<std::uint32_t>(close));
            typed_ = std::string(caret_.text.substr(open + 1, caret_.at - open - 1));
            case_sensitive_ = true;
            const auto self = index_.file(file_);
            const std::string_view from_path = path_of(file_);
            if (self != nullptr && !from_path.empty()) {
                for (const FileId other : index_.files()) {
                    const std::string_view target = path_of(other);
                    if (other != file_ && !target.empty()) {
                        add(specifier(from_path, target), "file", std::string(target));
                    }
                }
            }
            return true;
        }
        return false;
    }

    [[nodiscard]] std::size_t line_start_of(std::string_view line) const {
        return static_cast<std::size_t>(line.data() - caret_.text.data());
    }

    [[nodiscard]] std::string_view path_of(FileId id) const {
        return index_.path(id).value_or(std::string_view());
    }

    void bind_members() {
        list_.context = "bind";
        typed_ = std::string(caret_.prefix());
        std::set<std::string> seen;
        for (const FileId id : index_.files()) {
            const std::string_view text = index_.text(id).value_or(std::string_view());
            for (std::size_t at = text.find("bind::"); at != std::string_view::npos;
                 at = text.find("bind::", at + 1)) {
                std::size_t end = at + 6;
                while (end < text.size() &&
                       (std::isalnum(static_cast<unsigned char>(text[end])) != 0 ||
                        text[end] == '_')) {
                    ++end;
                }
                if (id == file_ && at + 6 == caret_.word) {
                    continue; // the word being typed
                }
                if (end > at + 6) {
                    seen.insert(std::string(text.substr(at + 6, end - at - 6)));
                }
            }
        }
        for (const std::string& id : seen) {
            add(id, "bind", "");
        }
        const std::string enclosing = enclosing_id();
        for (const Symbol& s : file_index_.symbols) {
            if (s.kind == SymbolKind::Handler && !enclosing.empty() && s.parent == enclosing &&
                !seen.contains(snake_case(s.name))) {
                add(snake_case(s.name), "bind", "handler " + s.name);
            }
        }
    }

    void css_classes() {
        list_.context = "member";
        typed_ = std::string(caret_.prefix());
        const std::size_t dot = caret_.word - 1;
        const std::string_view object = word_before(caret_.text, dot);
        for (const Symbol& s : file_index_.symbols) {
            if (s.kind != SymbolKind::CssModule || s.name != object) {
                continue;
            }
            list_.context = "cssClass";
            const auto css = index_.resolve_path(file_, s.import_source);
            if (!css) {
                return;
            }
            const std::string_view text = index_.text(*css).value_or(std::string_view());
            for (const CssClassSite& site : scan_css_classes(text)) {
                add(site.name, "cssClass", std::string(path_of(*css)));
            }
            return;
        }
    }

    void tags() {
        list_.context = "tag";
        typed_ = std::string(caret_.prefix());
        for (const Symbol* s : visible()) {
            const Symbol* decl = index_.declaration_of(s->id);
            const bool component = s->kind == SymbolKind::Component ||
                                   (s->kind == SymbolKind::Import && decl != nullptr &&
                                    decl->kind == SymbolKind::Component) ||
                                   (s->kind == SymbolKind::Import && decl == s &&
                                    std::isupper(static_cast<unsigned char>(s->name[0])) != 0);
            if (component) {
                add(s->name, s->kind == SymbolKind::Import ? "import" : "component",
                    s->kind == SymbolKind::Import ? s->import_source : "");
            }
        }
    }

    void attributes(const OpenTag& tag) {
        list_.context = "attribute";
        typed_ = std::string(caret_.prefix());
        const std::set<std::string> written =
            written_attributes(caret_.text.substr(tag.name_end, caret_.word - tag.name_end));
        const bool intrinsic = std::islower(static_cast<unsigned char>(tag.name[0])) != 0;
        const auto offer = [&](const std::string& name, std::string_view kind, std::string detail) {
            if (!written.contains(name)) {
                add(name, kind, std::move(detail));
            }
        };
        offer("key", "attribute", "");
        if (intrinsic) {
            offer("className", "attribute", "");
            return;
        }
        for (const Symbol* s : visible()) {
            if (s->name != tag.name) {
                continue;
            }
            const Symbol* decl = index_.declaration_of(s->id);
            if (decl == nullptr || decl->kind != SymbolKind::Component) {
                return;
            }
            const FileIndex* owner = index_.file(decl->span.file);
            for (const Symbol& prop : owner->symbols) {
                if (prop.kind == SymbolKind::Prop && prop.parent == decl->id) {
                    offer(prop.name, "prop", decl->name);
                }
            }
            return;
        }
    }

    void expression() {
        list_.context = "expression";
        typed_ = std::string(caret_.prefix());
        for (const Symbol* s : visible()) {
            add(s->name, std::string(wire_name(s->kind)), detail_of(*s));
        }
    }

    [[nodiscard]] std::string detail_of(const Symbol& s) const {
        if (s.kind == SymbolKind::Import) {
            return s.import_source;
        }
        if (const Symbol* parent = index_.symbol(s.parent)) {
            return parent->name;
        }
        return {};
    }

    // The component or hook whose declaration is the last module-scope one at or before the
    // caret. The index keeps no body ranges, so a caret between two declarations counts as being
    // in the earlier one.
    [[nodiscard]] std::string enclosing_id() const {
        const Symbol* last = nullptr;
        for (const Symbol& s : file_index_.symbols) {
            if (s.parent.empty() && s.span.from <= caret_unit_) {
                last = &s;
            }
        }
        return last != nullptr &&
                       (last->kind == SymbolKind::Component || last->kind == SymbolKind::Hook)
                   ? last->id
                   : std::string();
    }

    // Module-scope names and the names of the enclosing component or hook, innermost first.
    [[nodiscard]] std::vector<const Symbol*> visible() const {
        const std::string enclosing = enclosing_id();
        std::vector<const Symbol*> out;
        std::set<std::string> taken;
        const auto take = [&](bool inner) {
            for (const Symbol& s : file_index_.symbols) {
                const bool anonymous_param =
                    s.kind == SymbolKind::Param && s.id.find("/~") != std::string::npos;
                const bool in_scope =
                    inner ? !enclosing.empty() && s.parent == enclosing : s.parent.empty();
                if (in_scope && !anonymous_param && !s.name.empty() &&
                    taken.insert(s.name).second) {
                    out.push_back(&s);
                }
            }
        };
        take(true);
        take(false);
        return out;
    }

    void add(std::string label, std::string_view kind, std::string detail) {
        const bool match =
            case_sensitive_ ? label.starts_with(typed_) : starts_with_icase(label, typed_);
        if (!match || !labels_.insert(label).second) {
            return;
        }
        items_.push_back({std::move(label), std::string(kind), std::move(detail)});
    }

    CompletionList finish() {
        // Exact-case prefix matches first, then by label, so the order is a function of the
        // project and the caret only.
        std::ranges::stable_sort(items_, [&](const CompletionItem& a, const CompletionItem& b) {
            const bool ea = a.label.starts_with(typed_);
            const bool eb = b.label.starts_with(typed_);
            return ea != eb ? ea : a.label < b.label;
        });
        if (items_.size() > kMaxCompletions) {
            items_.resize(kMaxCompletions);
            list_.truncated = true;
        }
        list_.items = std::move(items_);
        return std::move(list_);
    }

    const ProjectIndex& index_;
    FileId file_;
    const FileIndex& file_index_;
    Caret caret_;
    std::uint32_t caret_unit_;
    CompletionList list_;
    std::vector<CompletionItem> items_;
    std::set<std::string> labels_;
    std::string typed_;
    bool case_sensitive_ = false;
};

} // namespace

CompletionList complete(const ProjectIndex& index, FileId file, std::uint32_t offset) {
    const FileIndex* file_index = index.file(file);
    const auto text = index.text(file);
    if (file_index == nullptr || !text || !file_index->indexed) {
        return {};
    }
    const Utf16Map map(*text);
    Caret caret;
    caret.text = *text;
    caret.at = map.to_byte(offset);
    caret.word = caret.at;
    while (caret.word > 0 && ident_byte((*text)[caret.word - 1])) {
        --caret.word;
    }
    caret.end = caret.at;
    while (caret.end < text->size() && ident_byte((*text)[caret.end])) {
        ++caret.end;
    }
    caret.line = line_start(*text, caret.at);
    return Completer(index, file, *file_index, caret, offset).run(map);
}

std::string completion_to_json(const ProjectIndex& index, FileId file, std::uint32_t offset) {
    const CompletionList list = complete(index, file, offset);
    std::string out = "{\"from\":" + std::to_string(list.from) +
                      ",\"to\":" + std::to_string(list.to) + ",\"context\":";
    append_json_string(out, list.context);
    out += ",\"items\":[";
    for (std::size_t i = 0; i < list.items.size(); ++i) {
        const CompletionItem& item = list.items[i];
        out += i == 0 ? "{\"label\":" : ",{\"label\":";
        append_json_string(out, item.label);
        out += ",\"kind\":";
        append_json_string(out, item.kind);
        if (!item.detail.empty()) {
            out += ",\"detail\":";
            append_json_string(out, item.detail);
        }
        out += '}';
    }
    out += std::string("],\"truncated\":") + (list.truncated ? "true" : "false") + "}";
    return out;
}

} // namespace tessera::compiler::analysis
