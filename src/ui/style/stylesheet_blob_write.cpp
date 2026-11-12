// SPDX-License-Identifier: MIT

#include "blob_io.hpp"
#include "tessera/ui/style/stylesheet_blob.hpp"

#include <cstddef>

namespace tessera::ui::style {
namespace {

using blob::Writer;

void write_declarations(Writer& out, const std::vector<Declaration>& declarations) {
    out.count(declarations.size());
    for (const Declaration& declaration : declarations) {
        out.str(declaration.property);
        out.str(declaration.value);
        out.boolean(declaration.important);
        out.u64(declaration.offset);
    }
}

void write_selector_list(Writer& out, const SelectorList& list);

void write_compound(Writer& out, const CompoundSelector& compound) {
    out.str(compound.tag);
    out.str(compound.id);
    out.count(compound.classes.size());
    for (const std::string& name : compound.classes) {
        out.str(name);
    }
    out.count(compound.attributes.size());
    for (const AttributeSelector& attribute : compound.attributes) {
        out.str(attribute.name);
        out.u8(static_cast<std::uint8_t>(attribute.op));
        out.str(attribute.value);
        out.boolean(attribute.case_insensitive);
    }
    out.count(compound.pseudos.size());
    for (const PseudoClass& pseudo : compound.pseudos) {
        out.str(pseudo.name);
        out.str(pseudo.argument);
        out.boolean(pseudo.pseudo_element);
        out.boolean(pseudo.nested != nullptr);
        if (pseudo.nested != nullptr) {
            write_selector_list(out, *pseudo.nested);
        }
    }
}

void write_selector_list(Writer& out, const SelectorList& list) {
    out.count(list.selectors.size());
    for (const ComplexSelector& complex : list.selectors) {
        out.count(complex.compounds.size());
        for (const CompoundSelector& compound : complex.compounds) {
            write_compound(out, compound);
        }
        out.count(complex.combinators.size());
        for (const Combinator combinator : complex.combinators) {
            out.u8(static_cast<std::uint8_t>(combinator));
        }
    }
}

void write_media(Writer& out, const MediaQueryList& list) {
    out.count(list.queries.size());
    for (const MediaQuery& query : list.queries) {
        out.boolean(query.negated);
        out.str(query.type);
        out.count(query.conditions.size());
        for (const MediaCondition& condition : query.conditions) {
            out.u8(static_cast<std::uint8_t>(condition.feature));
            out.u8(static_cast<std::uint8_t>(condition.comparison));
            out.str(condition.value);
        }
    }
}

void write_container(Writer& out, const ContainerQuery& query) {
    out.str(query.name);
    out.count(query.conditions.size());
    for (const ContainerCondition& condition : query.conditions) {
        out.u8(static_cast<std::uint8_t>(condition.axis));
        out.u8(static_cast<std::uint8_t>(condition.op));
        out.f64(condition.value);
    }
}

void write_item(Writer& out, const PreparedItem& item) {
    out.u8(static_cast<std::uint8_t>(item.kind));
    out.count(item.media.size());
    for (const MediaQueryList& list : item.media) {
        write_media(out, list);
    }
    switch (item.kind) {
    case PreparedItemKind::Rule:
        write_selector_list(out, item.selectors);
        write_declarations(out, item.declarations);
        out.u32(item.layer);
        out.count(item.containers.size());
        for (const ContainerQuery& query : item.containers) {
            write_container(out, query);
        }
        break;
    case PreparedItemKind::Layer:
        out.count(item.layer_refs.size());
        for (const std::uint32_t ref : item.layer_refs) {
            out.u32(ref);
        }
        break;
    case PreparedItemKind::Container:
        write_container(out, item.container);
        break;
    case PreparedItemKind::Keyframes:
        out.str(item.keyframes.name);
        out.count(item.keyframes.steps.size());
        for (const KeyframeStep& step : item.keyframes.steps) {
            out.f64(step.offset);
            write_declarations(out, step.declarations);
        }
        break;
    }
}

void put_u32(std::vector<std::uint8_t>& bytes, std::uint32_t value) {
    for (int i = 0; i < 4; ++i) {
        bytes.push_back(static_cast<std::uint8_t>(value >> (8 * i)));
    }
}

void put_u64(std::vector<std::uint8_t>& bytes, std::uint64_t value) {
    for (int i = 0; i < 8; ++i) {
        bytes.push_back(static_cast<std::uint8_t>(value >> (8 * i)));
    }
}

} // namespace

std::vector<std::uint8_t> serialize_stylesheet(const PreparedStyleSheet& prepared) {
    // The body is written first so the string table fills in order of first use; the table is then
    // emitted ahead of it.
    Writer out;
    out.count(prepared.layers.size());
    for (const std::string& layer : prepared.layers) {
        out.str(layer);
    }
    out.count(prepared.diagnostics.size());
    for (const StyleDiagnostic& diagnostic : prepared.diagnostics) {
        out.u8(static_cast<std::uint8_t>(diagnostic.kind));
        out.u64(diagnostic.offset);
        out.str(diagnostic.property);
        out.str(diagnostic.message);
    }
    out.count(prepared.items.size());
    for (const PreparedItem& item : prepared.items) {
        write_item(out, item);
    }

    std::vector<std::uint8_t> payload;
    put_u32(payload, static_cast<std::uint32_t>(out.table().size()));
    for (const std::string& value : out.table()) {
        put_u32(payload, static_cast<std::uint32_t>(value.size()));
        payload.insert(payload.end(), value.begin(), value.end());
    }
    payload.insert(payload.end(), out.body().begin(), out.body().end());

    std::vector<std::uint8_t> bytes;
    bytes.reserve(blob::kHeaderSize + payload.size());
    bytes.insert(bytes.end(), std::begin(blob::kMagic), std::end(blob::kMagic));
    put_u32(bytes, kStylesheetBlobFormatVersion);
    put_u32(bytes, kStylesheetEngineSchemaVersion);
    put_u64(bytes, prepared.source_hash);
    put_u32(bytes, static_cast<std::uint32_t>(payload.size()));
    bytes.insert(bytes.end(), payload.begin(), payload.end());
    return bytes;
}

} // namespace tessera::ui::style
