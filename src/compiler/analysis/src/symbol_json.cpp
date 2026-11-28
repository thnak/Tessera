// SPDX-License-Identifier: MIT

#include "tessera/compiler/analysis/symbol_json.hpp"

#include <string_view>

namespace tessera::compiler::analysis {

namespace {

std::string_view name_of(SymbolKind kind) {
    switch (kind) {
    case SymbolKind::Component:
        return "component";
    case SymbolKind::Hook:
        return "hook";
    case SymbolKind::Helper:
        return "helper";
    case SymbolKind::TypeAlias:
        return "type";
    case SymbolKind::Const:
        return "const";
    case SymbolKind::Import:
        return "import";
    case SymbolKind::CssModule:
        return "cssModule";
    case SymbolKind::Prop:
        return "prop";
    case SymbolKind::PropsParam:
        return "propsParam";
    case SymbolKind::State:
        return "state";
    case SymbolKind::Setter:
        return "setter";
    case SymbolKind::Handler:
        return "handler";
    case SymbolKind::Local:
        return "local";
    case SymbolKind::Param:
        return "param";
    }
    return "local";
}

std::string_view name_of(ReferenceKind kind) {
    switch (kind) {
    case ReferenceKind::Value:
        return "value";
    case ReferenceKind::Type:
        return "type";
    case ReferenceKind::Tag:
        return "tag";
    case ReferenceKind::Attribute:
        return "attribute";
    case ReferenceKind::CssClass:
        return "cssClass";
    case ReferenceKind::Member:
        return "member";
    case ReferenceKind::Export:
        return "export";
    }
    return "value";
}

std::string_view name_of(Unresolved reason) {
    switch (reason) {
    case Unresolved::None:
        return "none";
    case Unresolved::Unbound:
        return "unbound";
    case Unresolved::Builtin:
        return "builtin";
    case Unresolved::Intrinsic:
        return "intrinsic";
    case Unresolved::External:
        return "external";
    case Unresolved::NeedsType:
        return "needsType";
    case Unresolved::ImportUnresolved:
        return "importUnresolved";
    }
    return "unbound";
}

void quote(std::string& out, std::string_view text) {
    out += '"';
    for (const char c : text) {
        const auto u = static_cast<unsigned char>(c);
        if (c == '"' || c == '\\') {
            out += '\\';
            out += c;
        } else if (u < 0x20) {
            static constexpr char kHex[] = "0123456789abcdef";
            out += "\\u00";
            out += kHex[u >> 4];
            out += kHex[u & 0xF];
        } else {
            out += c;
        }
    }
    out += '"';
}

void resolution(std::string& out, const Resolution& value) {
    if (value.resolved()) {
        out += "\"symbol\":";
        quote(out, value.symbol);
    } else {
        out += "\"unresolved\":";
        quote(out, name_of(value.reason));
    }
}

void file_json(std::string& out, const FileIndex& index) {
    out += "{\"file\":" + std::to_string(index.file) +
           ",\"indexed\":" + (index.indexed ? "true" : "false");
    out += ",\"symbols\":[";
    for (std::size_t i = 0; i < index.symbols.size(); ++i) {
        const Symbol& symbol = index.symbols[i];
        out += i == 0 ? "{\"id\":" : ",{\"id\":";
        quote(out, symbol.id);
        out += ",\"kind\":";
        quote(out, name_of(symbol.kind));
        out += ",\"name\":";
        quote(out, symbol.name);
        out += ",\"from\":" + std::to_string(symbol.span.from) +
               ",\"to\":" + std::to_string(symbol.span.to);
        out += ",\"parent\":";
        quote(out, symbol.parent);
        out += std::string(",\"exported\":") + (symbol.exported ? "true" : "false");
        if (symbol.kind == SymbolKind::Import || symbol.kind == SymbolKind::CssModule) {
            out += ",\"target\":{";
            resolution(out, symbol.target);
            out += '}';
        }
        out += '}';
    }
    out += "],\"references\":[";
    for (std::size_t i = 0; i < index.references.size(); ++i) {
        const Reference& reference = index.references[i];
        out += i == 0 ? "{\"kind\":" : ",{\"kind\":";
        quote(out, name_of(reference.kind));
        out += ",\"name\":";
        quote(out, reference.name);
        out += ",\"from\":" + std::to_string(reference.span.from) +
               ",\"to\":" + std::to_string(reference.span.to) + ',';
        resolution(out, reference.resolution);
        out += '}';
    }
    out += "]}";
}

} // namespace

std::string symbols_to_json(const ProjectIndex& index, std::optional<FileId> file) {
    std::string out = "{\"files\":[";
    bool first = true;
    for (const FileId id : index.files()) {
        if (file && *file != id) {
            continue;
        }
        out += first ? "" : ",";
        first = false;
        file_json(out, *index.file(id));
    }
    out += "]}";
    return out;
}

std::string diagnostics_to_json(const ProjectIndex& index, std::optional<FileId> file) {
    const std::vector<Diagnostic> all = index.diagnostics(file);
    std::string out = "{\"files\":[";
    bool first_file = true;
    std::size_t next = 0;
    for (const FileId id : index.files()) {
        if (file && *file != id) {
            continue;
        }
        out += first_file ? "{\"file\":" : ",{\"file\":";
        first_file = false;
        out += std::to_string(id) + ",\"diagnostics\":[";
        bool first = true;
        for (; next < all.size() && all[next].span.file == id; ++next) {
            const Diagnostic& d = all[next];
            out += first ? "{\"code\":" : ",{\"code\":";
            first = false;
            quote(out, d.code);
            out += d.severity == DiagnosticSeverity::Error ? ",\"severity\":\"error\""
                                                           : ",\"severity\":\"warning\"";
            out += ",\"message\":";
            quote(out, d.message);
            out +=
                ",\"from\":" + std::to_string(d.span.from) + ",\"to\":" + std::to_string(d.span.to);
            if (d.related) {
                out += ",\"related\":{\"file\":" + std::to_string(d.related->file) +
                       ",\"from\":" + std::to_string(d.related->from) +
                       ",\"to\":" + std::to_string(d.related->to) + ",\"message\":";
                quote(out, d.related_message);
                out += '}';
            }
            out += '}';
        }
        out += "]}";
    }
    out += "]}";
    return out;
}

std::string_view wire_name(SymbolKind kind) {
    return name_of(kind);
}

std::string_view wire_name(ReferenceKind kind) {
    return name_of(kind);
}

std::string_view wire_name(Unresolved reason) {
    return name_of(reason);
}

void append_json_string(std::string& out, std::string_view text) {
    quote(out, text);
}

std::string analysis_capabilities_json() {
    return "{\"name\":\"analysis\",\"protocol\":1,\"features\":[\"symbols\",\"diagnostics\","
           "\"definition\",\"references\",\"hover\",\"completion\",\"prepareRename\","
           "\"rename\"]}";
}

} // namespace tessera::compiler::analysis
