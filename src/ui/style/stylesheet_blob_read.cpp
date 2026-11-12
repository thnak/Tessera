// SPDX-License-Identifier: MIT

#include "blob_io.hpp"
#include "tessera/ui/style/stylesheet_blob.hpp"

#include <memory>
#include <utility>

namespace tessera::ui::style {
namespace {

using blob::Reader;

std::vector<Declaration> read_declarations(Reader& in) {
    std::vector<Declaration> declarations;
    const std::size_t n = in.count();
    for (std::size_t i = 0; i < n && !in.failed(); ++i) {
        Declaration declaration;
        declaration.property = in.str();
        declaration.value = in.str();
        declaration.important = in.boolean();
        declaration.offset = static_cast<std::size_t>(in.u64());
        declarations.push_back(std::move(declaration));
    }
    return declarations;
}

SelectorList read_selector_list(Reader& in, int depth);

CompoundSelector read_compound(Reader& in, int depth) {
    CompoundSelector compound;
    compound.tag = in.str();
    compound.id = in.str();
    const std::size_t classes = in.count();
    for (std::size_t i = 0; i < classes && !in.failed(); ++i) {
        compound.classes.push_back(in.str());
    }
    const std::size_t attributes = in.count();
    for (std::size_t i = 0; i < attributes && !in.failed(); ++i) {
        AttributeSelector attribute;
        attribute.name = in.str();
        attribute.op = static_cast<AttributeOperator>(in.enumeration(6));
        attribute.value = in.str();
        attribute.case_insensitive = in.boolean();
        compound.attributes.push_back(std::move(attribute));
    }
    const std::size_t pseudos = in.count();
    for (std::size_t i = 0; i < pseudos && !in.failed(); ++i) {
        PseudoClass pseudo;
        pseudo.name = in.str();
        pseudo.argument = in.str();
        pseudo.pseudo_element = in.boolean();
        if (in.boolean()) {
            pseudo.nested = std::make_shared<const SelectorList>(read_selector_list(in, depth + 1));
        }
        compound.pseudos.push_back(std::move(pseudo));
    }
    return compound;
}

SelectorList read_selector_list(Reader& in, int depth) {
    SelectorList list;
    if (depth > blob::kMaxSelectorDepth) {
        in.fail();
        return list;
    }
    const std::size_t selectors = in.count();
    for (std::size_t i = 0; i < selectors && !in.failed(); ++i) {
        ComplexSelector complex;
        const std::size_t compounds = in.count();
        for (std::size_t c = 0; c < compounds && !in.failed(); ++c) {
            complex.compounds.push_back(read_compound(in, depth));
        }
        const std::size_t combinators = in.count();
        for (std::size_t c = 0; c < combinators && !in.failed(); ++c) {
            complex.combinators.push_back(static_cast<Combinator>(in.enumeration(3)));
        }
        // Matching walks combinators alongside compounds, so the counts must line up.
        if (complex.compounds.empty() ||
            complex.combinators.size() + 1 != complex.compounds.size()) {
            in.fail();
        }
        list.selectors.push_back(std::move(complex));
    }
    return list;
}

MediaQueryList read_media(Reader& in) {
    MediaQueryList list;
    const std::size_t queries = in.count();
    for (std::size_t i = 0; i < queries && !in.failed(); ++i) {
        MediaQuery query;
        query.negated = in.boolean();
        query.type = in.str();
        const std::size_t conditions = in.count();
        for (std::size_t c = 0; c < conditions && !in.failed(); ++c) {
            MediaCondition condition;
            condition.feature = static_cast<MediaFeature>(in.enumeration(5));
            condition.comparison = static_cast<MediaComparison>(in.enumeration(2));
            condition.value = in.str();
            query.conditions.push_back(std::move(condition));
        }
        list.queries.push_back(std::move(query));
    }
    return list;
}

ContainerQuery read_container(Reader& in) {
    ContainerQuery query;
    query.name = in.str();
    const std::size_t conditions = in.count();
    for (std::size_t i = 0; i < conditions && !in.failed(); ++i) {
        ContainerCondition condition;
        condition.axis = static_cast<ContainerAxis>(in.enumeration(1));
        condition.op = static_cast<ContainerOp>(in.enumeration(4));
        condition.value = in.f64();
        query.conditions.push_back(condition);
    }
    return query;
}

PreparedItem read_item(Reader& in, std::size_t layer_count) {
    PreparedItem item;
    item.kind = static_cast<PreparedItemKind>(in.enumeration(3));
    const std::size_t media = in.count();
    for (std::size_t i = 0; i < media && !in.failed(); ++i) {
        item.media.push_back(read_media(in));
    }
    switch (item.kind) {
    case PreparedItemKind::Rule: {
        item.selectors = read_selector_list(in, 0);
        item.declarations = read_declarations(in);
        item.layer = in.index(layer_count, true);
        const std::size_t containers = in.count();
        for (std::size_t i = 0; i < containers && !in.failed(); ++i) {
            item.containers.push_back(read_container(in));
        }
        break;
    }
    case PreparedItemKind::Layer: {
        const std::size_t refs = in.count();
        for (std::size_t i = 0; i < refs && !in.failed(); ++i) {
            item.layer_refs.push_back(in.index(layer_count, false));
        }
        break;
    }
    case PreparedItemKind::Container:
        item.container = read_container(in);
        break;
    case PreparedItemKind::Keyframes: {
        item.keyframes.name = in.str();
        const std::size_t steps = in.count();
        for (std::size_t i = 0; i < steps && !in.failed(); ++i) {
            KeyframeStep step;
            step.offset = in.f64();
            step.declarations = read_declarations(in);
            item.keyframes.steps.push_back(std::move(step));
        }
        break;
    }
    }
    return item;
}

std::unexpected<BlobError> error(BlobErrorCode code, std::string message) {
    return std::unexpected(BlobError{code, std::move(message)});
}

} // namespace

std::expected<PreparedStyleSheet, BlobError>
load_stylesheet_blob(std::span<const std::uint8_t> bytes,
                     std::optional<std::uint64_t> expected_hash) {
    if (bytes.size() < blob::kHeaderSize) {
        return error(BlobErrorCode::Truncated, "stylesheet blob is shorter than its header");
    }
    Reader header(bytes.first(blob::kHeaderSize));
    for (const char magic : blob::kMagic) {
        if (header.u8() != static_cast<std::uint8_t>(magic)) {
            return error(BlobErrorCode::BadMagic, "not a stylesheet blob");
        }
    }
    const std::uint32_t format = header.u32();
    if (format != kStylesheetBlobFormatVersion) {
        return error(BlobErrorCode::UnsupportedFormatVersion,
                     "stylesheet blob format version " + std::to_string(format) +
                         " is not supported (expected " +
                         std::to_string(kStylesheetBlobFormatVersion) + ")");
    }
    const std::uint32_t schema = header.u32();
    if (schema != kStylesheetEngineSchemaVersion) {
        return error(BlobErrorCode::EngineSchemaMismatch,
                     "stylesheet blob engine schema " + std::to_string(schema) +
                         " does not match the linked engine (" +
                         std::to_string(kStylesheetEngineSchemaVersion) + ")");
    }
    const std::uint64_t hash = header.u64();
    const std::uint32_t payload_size = header.u32();
    if (expected_hash.has_value() && *expected_hash != hash) {
        return error(BlobErrorCode::HashMismatch, "stylesheet blob content hash differs");
    }
    if (payload_size != bytes.size() - blob::kHeaderSize) {
        return error(BlobErrorCode::Truncated, "stylesheet blob payload size does not match");
    }

    Reader in(bytes.subspan(blob::kHeaderSize));
    std::vector<std::string> table;
    const std::size_t strings = in.count();
    for (std::size_t i = 0; i < strings && !in.failed(); ++i) {
        table.push_back(in.raw_string());
    }
    in.set_table(std::move(table));

    PreparedStyleSheet prepared;
    prepared.source_hash = hash;
    const std::size_t layers = in.count();
    for (std::size_t i = 0; i < layers && !in.failed(); ++i) {
        prepared.layers.push_back(in.str());
    }
    const std::size_t diagnostics = in.count();
    for (std::size_t i = 0; i < diagnostics && !in.failed(); ++i) {
        StyleDiagnostic diagnostic;
        diagnostic.kind = static_cast<StyleDiagnosticKind>(in.enumeration(3));
        diagnostic.offset = static_cast<std::size_t>(in.u64());
        diagnostic.property = in.str();
        diagnostic.message = in.str();
        prepared.diagnostics.push_back(std::move(diagnostic));
    }
    const std::size_t items = in.count();
    for (std::size_t i = 0; i < items && !in.failed(); ++i) {
        prepared.items.push_back(read_item(in, prepared.layers.size()));
    }
    if (in.failed()) {
        return error(BlobErrorCode::Corrupt, "stylesheet blob is truncated or malformed");
    }
    if (in.remaining() != 0) {
        return error(BlobErrorCode::Corrupt, "stylesheet blob has trailing bytes");
    }
    return prepared;
}

} // namespace tessera::ui::style
