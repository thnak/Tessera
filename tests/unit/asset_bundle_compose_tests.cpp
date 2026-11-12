// SPDX-License-Identifier: MIT
//
// Composed asset lookup (V1-M2, D10): one deterministic resolution path over several read-only
// bundles, so an app that embeds both a generated vector bundle and a project manifest bundle can
// paint a generated `Path` asset without hand-written glue.
//
// This test pins:
// - single-bundle behavior is unchanged when wrapped in a one-element composition;
// - disjoint bundles are both visible (the gap this closes);
// - the documented precedence: the FIRST bundle in the list that contains the name wins, so a
//   duplicate is shadowed in order and reversing the list reverses the winner;
// - failure is a value: not_found for a name in no bundle, invalid_name for a malformed name
//   (including with an empty list), empty list misses everything, no dereference;
// - the bytes returned come from the winning bundle (content compared, not just presence);
// - end to end: a generated-style bundle plus a manifest-style bundle, composed, resolves a vector
//   asset name and decodes it through load_asset_svg; a same-named manifest asset loses to the
//   generated one in the documented order.

#include "tessera/rendering/path/svg_document_cache.hpp"
#include "tessera/rendering/path/svg_loader.hpp"
#include "tessera/ui/assets/asset_bundle.hpp"
#include "tessera/ui/assets/asset_bundle_compose.hpp"
#include "tessera/ui/assets/asset_svg.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace assets = tessera::ui::assets;

int failures = 0;

void check(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << "\n";
        ++failures;
    }
}

void check_eq(std::string_view actual, std::string_view expected, std::string_view message) {
    if (actual != expected) {
        std::cerr << "FAIL: " << message << "\n  expected: " << expected
                  << "\n  actual:   " << actual << "\n";
        ++failures;
    }
}

struct NamedAsset {
    std::string_view name;
    std::string_view bytes;
    std::string_view mime = "application/octet-stream";
};

// Owns one bundle's table and blob and hands out a borrowed AssetBundle view. Tables are sorted by
// normalized name, the same invariant the generators emit and AssetBundle::find relies on.
struct BundleFixture {
    std::vector<std::uint8_t> data;
    std::vector<assets::AssetEntry> entries;

    [[nodiscard]] assets::AssetBundle view() const noexcept {
        return assets::AssetBundle(
            std::span<const assets::AssetEntry>(entries.data(), entries.size()),
            std::as_bytes(std::span<const std::uint8_t>(data.data(), data.size())));
    }
};

[[nodiscard]] BundleFixture make_bundle(std::span<const NamedAsset> named) {
    std::vector<NamedAsset> sorted(named.begin(), named.end());
    std::sort(sorted.begin(), sorted.end(), [](const NamedAsset& left, const NamedAsset& right) {
        return left.name < right.name;
    });
    BundleFixture fixture;
    for (const NamedAsset& asset : sorted) {
        const auto offset = static_cast<std::uint32_t>(fixture.data.size());
        const auto size = static_cast<std::uint32_t>(asset.bytes.size());
        const auto* begin = reinterpret_cast<const std::uint8_t*>(asset.bytes.data());
        fixture.data.insert(fixture.data.end(), begin, begin + asset.bytes.size());
        fixture.entries.push_back(assets::AssetEntry{asset.name, offset, size, asset.mime});
    }
    return fixture;
}

[[nodiscard]] std::string_view as_text(std::span<const std::byte> bytes) noexcept {
    return std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

[[nodiscard]] std::string_view error_name(assets::AssetError error) noexcept {
    return error == assets::AssetError::not_found ? "not_found" : "invalid_name";
}

constexpr std::string_view kIconSvg =
    "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 24 24\">"
    "<path d=\"M4 4 L20 4 L20 20 Z\" fill=\"#2e8b57\" stroke=\"#102030\" stroke-width=\"2\"/>"
    "</svg>";
constexpr std::string_view kWideSvg =
    "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 48 "
    "48\"><rect width=\"48\" height=\"48\" fill=\"#3366cc\"/></svg>";
constexpr std::string_view kManifestBytes = "not-a-real-font";
constexpr std::string_view kThemeBytes = "{\"color\":\"#334455\"}";
constexpr std::string_view kFontMime = "application/octet-stream";
constexpr std::string_view kSvgMime = "image/svg+xml";

void test_single_bundle_unchanged() {
    const std::array<NamedAsset, 2> named = {
        NamedAsset{"icons/a.svg", kIconSvg, kSvgMime},
        NamedAsset{"meta/readme.txt", "hello", "text/plain"},
    };
    const BundleFixture fixture = make_bundle(named);
    const assets::AssetBundle direct = fixture.view();
    const std::array<assets::AssetBundle, 1> one = {direct};
    const assets::ComposedAssetBundle composed(one);

    const auto direct_hit = direct.find("icons/a.svg");
    const auto composed_hit = composed.find("icons/a.svg");
    check(direct_hit.has_value() && composed_hit.has_value(),
          "single bundle: a hit resolves through the direct bundle and one-element composition");
    if (direct_hit.has_value() && composed_hit.has_value()) {
        check_eq(as_text(composed_hit->bytes), as_text(direct_hit->bytes),
                 "single bundle: composed bytes equal the direct bundle's bytes");
        check_eq(composed_hit->mime, direct_hit->mime, "single bundle: MIME hint is unchanged");
        check_eq(composed_hit->name, "icons/a.svg",
                 "single bundle: the normalized name is returned");
    }
    check(!composed.find("icons/missing.svg").has_value(), "single bundle: a miss is still a miss");
    check_eq(error_name(composed.find("icons/missing.svg").error()), "not_found",
             "single bundle: a miss is not_found");
    check(composed.contains("meta/readme.txt"), "single bundle: contains() reports a hit");
    check(!composed.contains("meta/missing.txt"), "single bundle: contains() reports a miss");
    check(composed.size() == 1 && !composed.empty() && composed.bundles().size() == 1,
          "single bundle: the composition reports its one borrowed bundle");
}

void test_disjoint_bundles() {
    const std::array<NamedAsset, 1> generated_named = {
        NamedAsset{"vector/VectorIcon/2.svg", kIconSvg, kSvgMime},
    };
    const std::array<NamedAsset, 2> manifest_named = {
        NamedAsset{"fonts/app.ttf", kManifestBytes, kFontMime},
        NamedAsset{"theme/theme.json", kThemeBytes, "application/json"},
    };
    const BundleFixture generated = make_bundle(generated_named);
    const BundleFixture manifest = make_bundle(manifest_named);
    const std::array<assets::AssetBundle, 2> bundles = {generated.view(), manifest.view()};
    const assets::ComposedAssetBundle composed(bundles);

    const auto vector = composed.find("vector/VectorIcon/2.svg");
    const auto font = composed.find("fonts/app.ttf");
    const auto theme = composed.find("theme/theme.json");
    check(vector.has_value(), "disjoint: the generated bundle is visible");
    check(font.has_value(), "disjoint: the manifest bundle (fonts) is visible too");
    check(theme.has_value(), "disjoint: the manifest bundle (theme) is visible too");
    if (vector.has_value()) {
        check_eq(as_text(vector->bytes), kIconSvg, "disjoint: vector bytes come from its bundle");
    }
    if (font.has_value()) {
        check_eq(as_text(font->bytes), kManifestBytes,
                 "disjoint: font bytes come from the manifest bundle");
        check_eq(font->mime, kFontMime, "disjoint: the manifest MIME hint is preserved");
    }
    check(composed.size() == 2, "disjoint: both bundles are in the composition");
    check(composed.contains("fonts/app.ttf") && composed.contains("theme/theme.json"),
          "disjoint: contains() sees names from every bundle");
}

void test_precedence_first_bundle_wins() {
    // One name, two different payloads. The order alone decides the winner.
    const std::array<NamedAsset, 1> first_named = {
        NamedAsset{"icons/shared.svg", kIconSvg, kSvgMime},
    };
    const std::array<NamedAsset, 2> second_named = {
        NamedAsset{"icons/shared.svg", kManifestBytes, "application/octet-stream"},
        NamedAsset{"icons/only-second.svg", "second", "text/plain"},
    };
    const BundleFixture first = make_bundle(first_named);
    const BundleFixture second = make_bundle(second_named);

    const std::array<assets::AssetBundle, 2> forward = {first.view(), second.view()};
    const std::array<assets::AssetBundle, 2> reversed = {second.view(), first.view()};

    const auto forward_hit = assets::ComposedAssetBundle(forward).find("icons/shared.svg");
    check(forward_hit.has_value(), "precedence: the duplicate name resolves");
    if (forward_hit.has_value()) {
        check_eq(as_text(forward_hit->bytes), kIconSvg,
                 "precedence: the first bundle's bytes win the duplicate");
        check_eq(forward_hit->mime, kSvgMime, "precedence: the first bundle's MIME hint wins");
    }

    const auto reversed_hit = assets::ComposedAssetBundle(reversed).find("icons/shared.svg");
    check(reversed_hit.has_value(), "precedence: the duplicate still resolves when reversed");
    if (reversed_hit.has_value()) {
        check_eq(as_text(reversed_hit->bytes), kManifestBytes,
                 "precedence: reversing the list reverses the winner");
        check_eq(reversed_hit->mime, "application/octet-stream",
                 "precedence: the reversed winner's MIME hint is used");
    }

    // A name only in the second bundle is not hidden by the first bundle's miss.
    const auto later = assets::ComposedAssetBundle(forward).find("icons/only-second.svg");
    check(later.has_value(), "precedence: a later bundle answers a name the earlier one lacks");
    if (later.has_value()) {
        check_eq(as_text(later->bytes), "second",
                 "precedence: the later bundle's bytes are returned");
    }
}

void test_not_found() {
    const std::array<NamedAsset, 1> named = {
        NamedAsset{"icons/a.svg", kIconSvg, kSvgMime},
    };
    const BundleFixture fixture = make_bundle(named);
    const std::array<assets::AssetBundle, 1> bundles = {fixture.view()};
    const assets::ComposedAssetBundle composed(bundles);

    const auto missing = composed.find("icons/missing.svg");
    check(!missing.has_value(), "not_found: a name in no bundle does not resolve");
    check_eq(error_name(missing.error()), "not_found",
             "not_found: the documented error value is returned");
    check(!composed.contains("icons/MISSING.svg"),
          "not_found: lookup stays case-sensitive across bundles");
    check_eq(error_name(composed.find("icons/MISSING.svg").error()), "not_found",
             "not_found: a differently-cased name is not_found, not invalid_name");
}

void test_empty_list() {
    const std::array<assets::AssetBundle, 0> none = {};
    const assets::ComposedAssetBundle composed(none);

    check(composed.empty() && composed.size() == 0 && composed.bundles().empty(),
          "empty: a zero-bundle composition reports itself empty");
    const auto missing = composed.find("icons/a.svg");
    check(!missing.has_value(), "empty: every name misses");
    check_eq(error_name(missing.error()), "not_found",
             "empty: a miss is not_found, not a dereference");
    check(!composed.contains("icons/a.svg"), "empty: contains() is false");

    const auto invalid = composed.find("../escape.svg");
    check(!invalid.has_value(), "empty: an invalid name still fails");
    check_eq(error_name(invalid.error()), "invalid_name",
             "empty: a malformed name is invalid_name even with no bundles");
}

void test_invalid_name() {
    const std::array<NamedAsset, 1> named = {
        NamedAsset{"icons/a.svg", kIconSvg, kSvgMime},
    };
    const BundleFixture fixture = make_bundle(named);
    const std::array<assets::AssetBundle, 1> bundles = {fixture.view()};
    const assets::ComposedAssetBundle composed(bundles);

    for (const std::string_view name :
         {std::string_view{}, std::string_view{"icons\\a.svg"}, std::string_view{"icons//a.svg"},
          std::string_view{"icons/../a.svg"}, std::string_view{"icons/./a.svg"},
          std::string_view{"/"}}) {
        const auto result = composed.find(name);
        check(!result.has_value(), "invalid_name: a malformed name does not resolve");
        check_eq(error_name(result.error()), "invalid_name",
                 "invalid_name: the malformed request is reported as invalid_name");
    }

    // A single leading slash is normalized, exactly like the underlying bundle.
    const auto normalized = composed.find("/icons/a.svg");
    check(normalized.has_value(), "invalid_name: one leading slash is still normalized");
    if (normalized.has_value()) {
        check_eq(normalized->name, "icons/a.svg", "invalid_name: the normalized name is returned");
    }
}

void test_winning_bytes_are_compared() {
    // Same name and same size in both bundles: only a content comparison detects the shadow.
    const std::array<NamedAsset, 1> first_named = {
        NamedAsset{"icons/tie.svg", "AAAA", "text/plain"},
    };
    const std::array<NamedAsset, 1> second_named = {
        NamedAsset{"icons/tie.svg", "BBBB", "text/plain"},
    };
    const BundleFixture first = make_bundle(first_named);
    const BundleFixture second = make_bundle(second_named);
    const std::array<assets::AssetBundle, 2> bundles = {first.view(), second.view()};

    const auto asset = assets::ComposedAssetBundle(bundles).find("icons/tie.svg");
    check(asset.has_value(), "content: the duplicate resolves");
    if (asset.has_value()) {
        check_eq(as_text(asset->bytes), "AAAA",
                 "content: the returned bytes are the winning bundle's, not merely present");
    }
}

void test_end_to_end_vector_decode() {
    // The generated-style bundle: one literal `AssetEntry` table plus its blob, the exact shape
    // codegen's `<module>::gen::vector_assets::bundle()` and cmake/embed_assets.cmake emit.
    const std::array<NamedAsset, 1> generated_named = {
        NamedAsset{"vector/VectorIcon/2.svg", kIconSvg, kSvgMime},
    };
    // The project manifest bundle: unrelated font/theme bytes, and (deliberately) the same vector
    // name with a different document to prove the precedence rule through the decode chain.
    const std::array<NamedAsset, 3> manifest_named = {
        NamedAsset{"fonts/app.ttf", kManifestBytes, kFontMime},
        NamedAsset{"theme/theme.json", kThemeBytes, "application/json"},
        NamedAsset{"vector/VectorIcon/2.svg", kWideSvg, kSvgMime},
    };
    const BundleFixture generated = make_bundle(generated_named);
    const BundleFixture manifest = make_bundle(manifest_named);

    const std::array<assets::AssetBundle, 2> generated_first = {generated.view(), manifest.view()};
    const std::array<assets::AssetBundle, 2> manifest_first = {manifest.view(), generated.view()};
    const assets::ComposedAssetBundle generated_wins(generated_first);
    const assets::ComposedAssetBundle manifest_wins(manifest_first);

    // The font asset is visible through the same composed lookup: the original gap is closed.
    check(generated_wins.contains("fonts/app.ttf"),
          "end to end: the manifest bundle is visible through the composition");

    if (!tessera::rendering::path::svg_backend_available()) {
        std::cout << "vector decode check skipped: no SVG backend in this build\n";
        return;
    }

    tessera::rendering::path::SvgDocumentCache documents;
    constexpr std::string_view kVectorAsset = "vector/VectorIcon/2.svg";

    const auto generated_document = assets::load_asset_svg(generated_wins, kVectorAsset, documents);
    check(generated_document.has_value(),
          "end to end: the composed lookup decodes the generated vector asset");
    if (generated_document.has_value()) {
        check((*generated_document)->width == 24 && (*generated_document)->height == 24,
              "end to end: the decoded document is the generated 24x24 SVG");
    }
    check(documents.stats().decodes == 1, "end to end: the first decoded asset counts one decode");

    // A second load of the same bytes is a document-cache hit, not another decode.
    const auto repeated = assets::load_asset_svg(generated_wins, kVectorAsset, documents);
    check(repeated.has_value() && documents.stats().decodes == 1,
          "end to end: reloading through the composition is a document-cache hit");

    // Same name, reversed order: the manifest's document must win, proving the composed path feeds
    // the decoder the winning bundle's bytes.
    const auto manifest_document = assets::load_asset_svg(manifest_wins, kVectorAsset, documents);
    check(manifest_document.has_value(),
          "end to end: the reversed composition decodes the shadowing manifest asset");
    if (manifest_document.has_value()) {
        check((*manifest_document)->width == 48 && (*manifest_document)->height == 48,
              "end to end: the decode follows the winner (48x48), not just the lookup");
    }

    // A malformed name is a value all the way through the composed decode helper.
    const auto invalid = assets::load_asset_svg(generated_wins, "vector//bad.svg", documents);
    check(!invalid.has_value() && invalid.error() == assets::AssetSvgError::invalid_name,
          "end to end: a malformed name maps to AssetSvgError::invalid_name");
    const auto missing = assets::load_asset_svg(generated_wins, "vector/missing.svg", documents);
    check(!missing.has_value() && missing.error() == assets::AssetSvgError::not_found,
          "end to end: a name in no bundle maps to AssetSvgError::not_found");
}

} // namespace

int main() {
    test_single_bundle_unchanged();
    test_disjoint_bundles();
    test_precedence_first_bundle_wins();
    test_not_found();
    test_empty_list();
    test_invalid_name();
    test_winning_bytes_are_compared();
    test_end_to_end_vector_decode();
    if (failures != 0) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "asset bundle composition tests passed\n";
    return 0;
}
