// SPDX-License-Identifier: MIT

#include "css_scan.hpp"
#include "tessera/compiler/analysis/rename.hpp"
#include "tessera/compiler/analysis/symbol_json.hpp"
#include "utf16_map.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <map>
#include <memory>
#include <set>

namespace tessera::compiler::analysis {

namespace {

constexpr std::array<std::string_view, 50> kReserved = {
    "await",     "break",  "case",     "catch",  "class",      "const",   "continue",  "debugger",
    "default",   "delete", "do",       "else",   "enum",       "export",  "extends",   "false",
    "finally",   "for",    "function", "if",     "implements", "import",  "in",        "instanceof",
    "interface", "let",    "new",      "null",   "package",    "private", "protected", "public",
    "return",    "static", "super",    "switch", "this",       "throw",   "true",      "try",
    "typeof",    "var",    "void",     "while",  "with",       "yield",   "undefined", "arguments",
    "eval",      "NaN"};

Refusal refuse(std::string code, std::string message) {
    return Refusal{"TESSERA_ANALYSIS_RENAME_" + std::move(code), std::move(message)};
}

// The text of spans, with one UTF-16 table per file.
class Texts {
public:
    explicit Texts(const ProjectIndex& index)
        : index_(index) {}

    [[nodiscard]] std::string_view slice(const Span& span) {
        const auto text = index_.text(span.file);
        if (!text) {
            return {};
        }
        auto& map = maps_[span.file];
        if (!map) {
            map = std::make_unique<Utf16Map>(*text);
        }
        const auto from = map->to_byte(span.from);
        const auto to = map->to_byte(span.to);
        return to >= from ? text->substr(from, to - from) : std::string_view();
    }

private:
    const ProjectIndex& index_;
    std::map<FileId, std::unique_ptr<Utf16Map>> maps_;
};

bool generated(std::string_view path, std::string_view text) {
    for (std::size_t i = 0; i <= path.size();) {
        const auto j = path.find('/', i);
        const auto end = j == std::string_view::npos ? path.size() : j;
        const auto part = path.substr(i, end - i);
        if (part == "gen" || part == "generated" || part == "__generated__" ||
            (end == path.size() && (part.find(".gen.") != std::string_view::npos ||
                                    part.find(".generated.") != std::string_view::npos))) {
            return true;
        }
        i = end + 1;
    }
    const auto head = text.substr(0, 512);
    return head.find("@generated") != std::string_view::npos ||
           head.find("DO NOT EDIT") != std::string_view::npos;
}

bool plain_identifier(std::string_view name, bool allow_dollar) {
    if (name.empty() || std::isdigit(static_cast<unsigned char>(name[0])) != 0) {
        return false;
    }
    return std::ranges::all_of(name, [&](char c) {
        return (std::isalnum(static_cast<unsigned char>(c)) != 0 &&
                static_cast<unsigned char>(c) < 0x80) ||
               c == '_' || (allow_dollar && c == '$');
    });
}

struct Plan {
    std::string name;
    std::string kind;
    Span range;
    const Symbol* decl = nullptr; // null for a CSS class
    std::vector<Span> sites;      // every span that gets the new name
    FileId css_file = 0;          // a CSS class: the style sheet
    std::vector<CssClassSite> css_classes;
};

struct Planned {
    std::optional<Plan> plan;
    Refusal refusal;
};

Planned planned(Refusal r) {
    return Planned{std::nullopt, std::move(r)};
}

std::optional<Refusal> generated_site(const ProjectIndex& index, const std::set<FileId>& files) {
    for (const FileId id : files) {
        if (generated(index.path(id).value_or(std::string_view()),
                      index.text(id).value_or(std::string_view()))) {
            return refuse("GENERATED",
                          "the rename would edit generated code (" +
                              std::string(index.path(id).value_or("a generated file")) +
                              "); edit its source instead");
        }
    }
    return std::nullopt;
}

Planned plan_css_class(const ProjectIndex& index, const Reference& reference, Texts& texts) {
    const Symbol* binding = index.symbol(reference.via);
    if (binding == nullptr) {
        return planned(refuse("UNRESOLVED", "the class has no style sheet binding"));
    }
    const auto css = index.resolve_path(binding->span.file, binding->import_source);
    if (!css) {
        return planned(refuse(
            "EXTERNAL", "the style sheet " + binding->import_source +
                            " is not part of the project, so its selectors cannot be renamed"));
    }
    Plan plan;
    plan.name = reference.name;
    plan.kind = "cssClass";
    plan.range = reference.span;
    plan.css_file = *css;
    plan.css_classes = scan_css_classes(index.text(*css).value_or(std::string_view()));
    std::set<FileId> files{*css};
    for (const CssClassSite& site : plan.css_classes) {
        if (site.name == plan.name) {
            plan.sites.push_back(Span{*css, site.from, site.to});
        }
    }
    if (plan.sites.empty()) {
        return planned(refuse("EXTERNAL", "the class ." + plan.name + " is not a selector of " +
                                              binding->import_source));
    }
    for (const FileId id : index.files()) {
        for (const Symbol& s : index.file(id)->symbols) {
            if (s.kind != SymbolKind::CssModule || index.resolve_path(id, s.import_source) != css) {
                continue;
            }
            for (const Reference& use : index.file(id)->references) {
                if (use.kind == ReferenceKind::CssClass && use.via == s.id &&
                    use.name == plan.name) {
                    plan.sites.push_back(use.span);
                    files.insert(id);
                }
            }
        }
    }
    for (const Span& site : plan.sites) {
        if (texts.slice(site) != plan.name) {
            return planned(refuse("ALIAS", "a use of ." + plan.name + " is spelled differently"));
        }
    }
    if (const auto bad = generated_site(index, files)) {
        return planned(*bad);
    }
    return Planned{std::move(plan), {}};
}

Planned make_plan(const ProjectIndex& index, FileId file, std::uint32_t offset, Texts& texts) {
    const auto located = index.locate(file, offset);
    if (!located) {
        return planned(refuse("NO_NAME", "the caret is not on a name"));
    }
    const Symbol* decl = nullptr;
    Span range;
    if (located->declaration) {
        decl = index.declaration_of(located->symbol);
        range = index.symbol(located->symbol)->span;
    } else {
        const Reference& reference = *located->reference;
        range = reference.span;
        if (!reference.resolution.resolved()) {
            switch (reference.resolution.reason) {
            case Unresolved::External:
                if (reference.kind == ReferenceKind::CssClass) {
                    return plan_css_class(index, reference, texts);
                }
                return planned(
                    refuse("EXTERNAL", reference.name + " is declared outside the project"));
            case Unresolved::ImportUnresolved:
                return planned(
                    refuse("EXTERNAL",
                           reference.name + " is imported from a file the project does not hold"));
            case Unresolved::NeedsType:
                return planned(
                    refuse("UNRESOLVED",
                           "a member access cannot be renamed without the type of its receiver"));
            default:
                return planned(refuse(
                    "UNRESOLVED", reference.name + " is not declared in the project (a builtin, an "
                                                   "intrinsic tag or an unbound name)"));
            }
        }
        decl = index.declaration_of(reference.resolution.symbol);
    }
    if (decl == nullptr) {
        return planned(refuse("UNRESOLVED", "the name has no declaration"));
    }
    if (decl->kind == SymbolKind::Import) {
        return planned(refuse("EXTERNAL", decl->name + " is imported from " + decl->import_source +
                                              ", which the project does not declare"));
    }
    Plan plan;
    plan.name = decl->name;
    plan.kind = std::string(wire_name(decl->kind));
    plan.range = range;
    plan.decl = decl;
    std::set<FileId> files{decl->span.file};
    for (const Occurrence& o : index.references_to(decl->id, true)) {
        plan.sites.push_back(o.span);
        files.insert(o.span.file);
    }
    for (const Span& site : plan.sites) {
        if (texts.slice(site) != plan.name) {
            return planned(
                refuse("ALIAS", "a use of " + plan.name +
                                    " is spelled differently (an `as` alias); rename it by hand"));
        }
    }
    if (const auto bad = generated_site(index, files)) {
        return planned(*bad);
    }
    return Planned{std::move(plan), {}};
}

std::optional<Refusal> check_name(const ProjectIndex& index, const Plan& plan,
                                  std::string_view name) {
    const bool css = plan.decl == nullptr;
    if (!plain_identifier(name, !css)) {
        return refuse("INVALID_NAME", "\"" + std::string(name) + "\" is not an identifier");
    }
    if (!css && std::ranges::find(kReserved, name) != kReserved.end()) {
        return refuse("RESERVED", "\"" + std::string(name) + "\" is a reserved word");
    }
    if (name == plan.name) {
        return refuse("NO_CHANGE", "the new name is the current name");
    }
    if (css) {
        const bool taken = std::ranges::any_of(
            plan.css_classes, [&](const CssClassSite& s) { return s.name == name; });
        return taken ? std::optional(refuse("COLLISION", "the style sheet already has a class ." +
                                                             std::string(name)))
                     : std::nullopt;
    }
    const Symbol& decl = *plan.decl;
    if (decl.kind == SymbolKind::Component &&
        std::isupper(static_cast<unsigned char>(name[0])) == 0) {
        return refuse("INVALID_NAME", "a component name starts with an upper case letter");
    }
    if (decl.kind == SymbolKind::Hook &&
        !(name.starts_with("use") && name.size() > 3 &&
          std::isupper(static_cast<unsigned char>(name[3])) != 0)) {
        return refuse("INVALID_NAME", "a hook name is `use` followed by an upper case letter");
    }
    std::set<FileId> files{decl.span.file};
    for (const Span& s : plan.sites) {
        files.insert(s.file);
    }
    const auto collides = [&](const Symbol& s) { return s.name == name && s.id != decl.id; };
    if (decl.parent.empty()) {
        for (const FileId id : files) {
            for (const Symbol& s : index.file(id)->symbols) {
                if (collides(s)) {
                    return refuse("COLLISION",
                                  "\"" + std::string(name) + "\" is already declared in " +
                                      std::string(index.path(id).value_or("the file")));
                }
            }
        }
        if (decl.kind == SymbolKind::Component) {
            for (const FileId id : index.files()) {
                for (const Symbol& s : index.file(id)->symbols) {
                    if (s.kind == SymbolKind::Component && s.parent.empty() && s.name == name) {
                        return refuse("COLLISION",
                                      "a component named " + std::string(name) +
                                          " already exists in " +
                                          std::string(index.path(id).value_or("the project")));
                    }
                }
            }
        }
    } else {
        for (const Symbol& s : index.file(decl.span.file)->symbols) {
            if (collides(s) && (s.parent == decl.parent || s.parent.empty())) {
                return refuse("COLLISION",
                              "\"" + std::string(name) + "\" is already declared in this scope");
            }
        }
    }
    return std::nullopt;
}

void member(std::string& out, std::string_view key, std::string_view value) {
    out += ",\"";
    out += key;
    out += "\":";
    append_json_string(out, value);
}

void refusal_json(std::string& out, const Refusal& r) {
    out += "\"refusal\":{\"code\":";
    append_json_string(out, r.code);
    member(out, "message", r.message);
    out += '}';
}

} // namespace

PrepareRename prepare_rename(const ProjectIndex& index, FileId file, std::uint32_t offset) {
    Texts texts(index);
    Planned p = make_plan(index, file, offset, texts);
    PrepareRename out;
    if (!p.plan) {
        out.refusal = std::move(p.refusal);
        return out;
    }
    out.ok = true;
    out.range = p.plan->range;
    out.placeholder = p.plan->name;
    out.kind = p.plan->kind;
    return out;
}

RenameResult rename_symbol(const ProjectIndex& index, FileId file, std::uint32_t offset,
                           std::string_view new_name) {
    Texts texts(index);
    Planned p = make_plan(index, file, offset, texts);
    RenameResult out;
    if (!p.plan) {
        out.refusal = std::move(p.refusal);
        return out;
    }
    if (auto bad = check_name(index, *p.plan, new_name)) {
        out.refusal = std::move(bad);
        return out;
    }
    std::ranges::sort(p.plan->sites, [](const Span& a, const Span& b) {
        return a.file != b.file ? a.file < b.file : a.from > b.from;
    });
    const auto last = std::ranges::unique(p.plan->sites);
    p.plan->sites.erase(last.begin(), last.end());
    for (const Span& site : p.plan->sites) {
        out.edits.push_back(
            TextEdit{site.file, site.from, site.to - site.from, std::string(new_name)});
    }
    return out;
}

std::string prepare_rename_to_json(const ProjectIndex& index, FileId file, std::uint32_t offset) {
    const PrepareRename result = prepare_rename(index, file, offset);
    if (!result.ok) {
        std::string out = "{\"prepare\":null,";
        refusal_json(out, *result.refusal);
        return out + '}';
    }
    std::string out = "{\"prepare\":{\"from\":" + std::to_string(result.range.from) +
                      ",\"to\":" + std::to_string(result.range.to);
    member(out, "placeholder", result.placeholder);
    member(out, "kind", result.kind);
    return out + "}}";
}

std::string rename_to_json(const ProjectIndex& index, FileId file, std::uint32_t offset,
                           std::string_view new_name) {
    const RenameResult result = rename_symbol(index, file, offset, new_name);
    std::string out = "{\"edits\":[";
    for (std::size_t i = 0; i < result.edits.size(); ++i) {
        const TextEdit& e = result.edits[i];
        out += std::string(i == 0 ? "{" : ",{") + "\"file\":" + std::to_string(e.file) +
               ",\"offset\":" + std::to_string(e.offset) +
               ",\"length\":" + std::to_string(e.length);
        member(out, "newText", e.new_text);
        out += '}';
    }
    out += ']';
    if (result.refusal) {
        out += ',';
        refusal_json(out, *result.refusal);
    }
    return out + '}';
}

} // namespace tessera::compiler::analysis
