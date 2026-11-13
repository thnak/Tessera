// SPDX-License-Identifier: MIT

#include "benchmark_paths.hpp"
#include "tessera/ui/style/compiled_stylesheet.hpp"
#include "tessera/ui/style/prepared_stylesheet.hpp"
#include "tessera/ui/style/stylesheet_blob.hpp"
#include "tessera/ui/style/tokenizer.hpp"

#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace tessera::benchmark {
namespace {

namespace style = tessera::ui::style;

constexpr std::uint64_t kFnvOffset = 14695981039346656037ULL;
constexpr std::uint64_t kFnvPrime = 1099511628211ULL;

// Order-sensitive FNV-1a: every value is separated so "ab"+"c" and "a"+"bc" differ.
void mix_text(std::uint64_t& hash, std::string_view text) {
    for (const char character : text) {
        hash ^= static_cast<unsigned char>(character);
        hash *= kFnvPrime;
    }
    hash ^= 0xFFU;
    hash *= kFnvPrime;
}

void mix_number(std::uint64_t& hash, std::uint64_t value) {
    for (int byte = 0; byte < 8; ++byte) {
        hash ^= (value >> (8 * byte)) & 0xFFU;
        hash *= kFnvPrime;
    }
}

std::uint64_t fingerprint(const style::CompiledStyleSheet& sheet) {
    std::uint64_t hash = kFnvOffset;
    mix_number(hash, sheet.rules().size());
    for (const style::CompiledRule& rule : sheet.rules()) {
        mix_number(hash, rule.source_order);
        mix_number(hash, rule.layer);
        mix_number(hash, static_cast<std::uint64_t>(rule.origin));
        mix_number(hash, rule.containers.size());
        mix_number(hash, rule.declarations.size());
        for (const style::Declaration& declaration : rule.declarations) {
            mix_text(hash, declaration.property);
            mix_text(hash, declaration.value);
            mix_number(hash, declaration.important ? 1U : 0U);
        }
    }
    mix_number(hash, sheet.layer_order().size());
    for (const auto& [name, slot] : sheet.layer_order()) {
        mix_text(hash, name);
        mix_number(hash, slot);
    }
    mix_number(hash, sheet.keyframes().size());
    for (const style::KeyframeBlock& block : sheet.keyframes()) {
        mix_text(hash, block.name);
        mix_number(hash, block.steps.size());
        for (const style::KeyframeStep& step : block.steps) {
            mix_number(hash, step.declarations.size());
            for (const style::Declaration& declaration : step.declarations) {
                mix_text(hash, declaration.property);
                mix_text(hash, declaration.value);
            }
        }
    }
    mix_number(hash, sheet.container_queries().size());
    mix_number(hash, sheet.diagnostics().size());
    for (const style::StyleDiagnostic& diagnostic : sheet.diagnostics()) {
        mix_number(hash, static_cast<std::uint64_t>(diagnostic.kind));
        mix_text(hash, diagnostic.property);
        mix_text(hash, diagnostic.message);
    }
    return hash;
}

std::size_t declaration_count(const style::CompiledStyleSheet& sheet) {
    std::size_t count = 0;
    for (const style::CompiledRule& rule : sheet.rules()) {
        count += rule.declarations.size();
    }
    return count;
}

PathCounters counters(const style::CompiledStyleSheet& sheet, std::size_t bytes_read,
                      const Cost& prepare, const Cost& resolve) {
    PathCounters result;
    result.bytes_read = bytes_read;
    result.prepare = prepare;
    result.resolve = resolve;
    result.fingerprint = fingerprint(sheet);
    result.rules = sheet.rules().size();
    result.selectors = sheet.index().size();
    result.declarations = declaration_count(sheet);
    result.diagnostics = sheet.diagnostics().size();
    result.keyframes = sheet.keyframes().size();
    result.layers = sheet.layer_order().size();
    result.container_queries = sheet.container_queries().size();
    return result;
}

struct ResolvedSheet {
    style::CompiledStyleSheet sheet;
    Cost cost;
};

// Resolves `prepared` and reports the shared resolve stage. The registry is moved in rather than
// copied, so the measured stage is the sheet build and not a registry copy.
ResolvedSheet resolve(const style::PreparedStyleSheet& prepared, style::PropertyRegistry registry) {
    std::optional<style::CompiledStyleSheet> sheet;
    const Cost cost = measure([&] {
        sheet.emplace(prepared, style::Origin::Author, std::move(registry), style::MediaContext{});
    });
    return ResolvedSheet{std::move(*sheet), cost};
}

std::optional<PathComparison> run_once(std::string_view css) {
    PathComparison result;
    result.css_bytes = css.size();
    result.css_tokens = style::Tokenizer(css).tokenize().size();

    // Setup, not measured: exactly the bytes codegen embeds, produced by the serializer the CLI
    // uses (docs/specs/compiled-stylesheet-v1.md section 9). Producing them is build-time work.
    const std::vector<std::uint8_t> blob =
        style::serialize_stylesheet(style::prepare_stylesheet(css));
    result.blob_bytes = blob.size();

    // PropertyRegistry construction is a fixed engine cost both paths pay, so it happens outside
    // the measured phases and is moved into the resolve stage.
    style::PropertyRegistry blob_registry;
    style::PropertyRegistry parse_registry;

    std::optional<style::PreparedStyleSheet> loaded;
    std::string load_error;
    const Cost load = measure([&] {
        auto decoded = style::load_stylesheet_blob(blob);
        if (!decoded) {
            load_error = decoded.error().message;
            return;
        }
        loaded = std::move(*decoded);
    });
    if (!loaded) {
        std::fprintf(stderr,
                     "stylesheet_benchmark: the serialized stylesheet does not load (%s); the blob "
                     "format or the engine schema changed\n",
                     load_error.c_str());
        return std::nullopt;
    }
    ResolvedSheet blob_sheet = resolve(*loaded, std::move(blob_registry));

    std::optional<style::PreparedStyleSheet> prepared;
    const Cost prepare = measure([&] { prepared = style::prepare_stylesheet(css); });
    // The two paths must see the same prepared sheet, not merely equivalent ones: the blob has to
    // be exactly what serializing this stylesheet produces, which is what codegen emits.
    if (style::serialize_stylesheet(*prepared) != blob) {
        std::fprintf(stderr,
                     "stylesheet_benchmark: the embedded blob is not the serialization of the "
                     "parsed stylesheet\n");
        return std::nullopt;
    }
    ResolvedSheet parse_sheet = resolve(*prepared, std::move(parse_registry));

    result.blob = counters(blob_sheet.sheet, blob.size(), load, blob_sheet.cost);
    result.parse = counters(parse_sheet.sheet, css.size(), prepare, parse_sheet.cost);
    return result;
}

bool same_counters(const PathCounters& lhs, const PathCounters& rhs) {
    return lhs.bytes_read == rhs.bytes_read && lhs.fingerprint == rhs.fingerprint &&
           lhs.rules == rhs.rules && lhs.selectors == rhs.selectors &&
           lhs.declarations == rhs.declarations && lhs.diagnostics == rhs.diagnostics &&
           lhs.keyframes == rhs.keyframes && lhs.layers == rhs.layers &&
           lhs.container_queries == rhs.container_queries &&
           lhs.prepare.allocations == rhs.prepare.allocations &&
           lhs.prepare.allocations_bytes == rhs.prepare.allocations_bytes &&
           lhs.resolve.allocations == rhs.resolve.allocations &&
           lhs.resolve.allocations_bytes == rhs.resolve.allocations_bytes;
}

} // namespace

std::optional<PathComparison> run_paths(std::string_view css) {
    const std::optional<PathComparison> first = run_once(css);
    if (!first) {
        return std::nullopt;
    }
    // Determinism: every counter is a pure function of the input and the binary, so a second run
    // must reproduce it exactly. A mismatch means the benchmark picked up a clock, a random source
    // or filesystem ordering.
    const std::optional<PathComparison> second = run_once(css);
    if (!second || first->css_bytes != second->css_bytes ||
        first->css_tokens != second->css_tokens || first->blob_bytes != second->blob_bytes ||
        !same_counters(first->blob, second->blob) || !same_counters(first->parse, second->parse)) {
        std::fprintf(stderr, "stylesheet_benchmark: counters are not reproducible across runs\n");
        return std::nullopt;
    }
    return first;
}

bool same_cascade(const PathComparison& comparison) {
    return comparison.blob.fingerprint == comparison.parse.fingerprint &&
           comparison.blob.rules == comparison.parse.rules &&
           comparison.blob.selectors == comparison.parse.selectors &&
           comparison.blob.declarations == comparison.parse.declarations &&
           comparison.blob.diagnostics == comparison.parse.diagnostics &&
           comparison.blob.keyframes == comparison.parse.keyframes &&
           comparison.blob.layers == comparison.parse.layers &&
           comparison.blob.container_queries == comparison.parse.container_queries;
}

} // namespace tessera::benchmark
