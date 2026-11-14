// SPDX-License-Identifier: MIT
//
// End-to-end contract for `tessera icons add` (ADR-047 item 8): the pinned fixture sets are the
// only input (no network), referenced icons become deterministic vector assets, unreferenced icons
// are tree-shaken, attribution is generated, and two runs produce byte-identical output that
// matches the checked-in golden. The license/subset gate contract is covered by
// icons_gate_tests.cpp.

#include "icon_import.hpp"
#include "icon_set.hpp"
#include "icons_test_support.hpp"

#include <algorithm>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace fs = std::filesystem;
namespace icons = tessera::icons;
using namespace tessera::icons::test_support;

// The golden comparison runs in both directions: every generated file must match the checked-in
// bytes, and neither tree may hold a file the other does not.
[[nodiscard]] bool matches_golden(const icons::ImportResult& result, const fs::path& out_dir,
                                  const fs::path& golden_dir) {
    bool ok = true;
    for (const icons::EmittedFile& file : result.files) {
        const std::optional<std::string> text = read_text(golden_dir / file.path);
        if (!text) {
            ok = expect(false, "golden is missing " + file.path) && ok;
            continue;
        }
        if (*text != file.bytes) {
            ok = expect(false, "golden differs at " + file.path) && ok;
        }
    }
    std::vector<std::string> expected;
    for (const icons::EmittedFile& file : result.files) {
        expected.push_back(file.path);
    }
    std::sort(expected.begin(), expected.end());
    ok =
        expect(list_files(golden_dir) == expected, "golden directory has extra or missing files") &&
        ok;
    ok = expect(list_files(out_dir) == expected, "written output has extra or missing files") && ok;
    return ok;
}

} // namespace

int main() {
    bool ok = true;
    const fs::path golden_root = TESSERA_ICONS_GOLDEN_DIR;
    const fs::path out_a = fs::temp_directory_path() / "tessera_icons_out_a";
    const fs::path out_b = fs::temp_directory_path() / "tessera_icons_out_b";
    const fs::path out_mono = fs::temp_directory_path() / "tessera_icons_mono";
    std::error_code error;
    fs::remove_all(out_a, error);
    fs::remove_all(out_b, error);
    fs::remove_all(out_mono, error);

    // Five references across three sets; the tabler set also contains four unreferenced icons.
    std::vector<std::string> references{"tabler:home", "tabler:house", "tabler:search",
                                        "mdi:account", "ccby:chart"};
    const auto basic = icons::import_icons(fixture_request(references));
    ok = expect(basic.has_value(), "the basic import failed") && ok;
    if (!basic) {
        std::cerr << "icons_import: " << basic.error() << "\n";
        return 1;
    }

    // Tree-shaking: only the referenced icons, plus an alias, are emitted.
    ok = expect(basic->icons.size() == 5, "expected exactly 5 tree-shaken icons") && ok;
    ok = expect(find_file(*basic, "icons/tabler/settings.svg") == nullptr,
                "an unreferenced icon was emitted") &&
         ok;
    ok = expect(find_file(*basic, "icons/mdi/account-plus.svg") == nullptr,
                "an unreferenced icon was emitted") &&
         ok;
    const auto tabler = icons::load_icon_set(TESSERA_ICONS_FIXTURE_DIR, "tabler");
    ok = expect(tabler.has_value() && tabler->icons.size() == 7,
                "the pinned tabler fixture should hold 6 icons plus 1 alias") &&
         ok;
    ok =
        expect(find_file(*basic, "icons/tabler/home.svg") != nullptr, "home was not emitted") && ok;
    ok = expect(find_file(*basic, "icons/tabler/house.svg") != nullptr,
                "the alias was not emitted") &&
         ok;
    if (const icons::EmittedFile* home = find_file(*basic, "icons/tabler/home.svg");
        home != nullptr) {
        if (const icons::EmittedFile* house = find_file(*basic, "icons/tabler/house.svg");
            house != nullptr) {
            ok =
                expect(home->bytes == house->bytes, "the alias body differs from its parent") && ok;
        }
    }

    // The vector asset is the engine's static-icon form: an SVG document with a viewBox and the
    // original body, which load_svg() decodes into PathShape geometry.
    const icons::EmittedFile* home = find_file(*basic, "icons/tabler/home.svg");
    ok = expect(home != nullptr &&
                    contains(home->bytes, "<svg xmlns=\"http://www.w3.org/2000/svg\""),
                "the vector asset is not a standalone SVG") &&
         ok;
    ok = expect(home != nullptr && contains(home->bytes, "viewBox=\"0 0 24 24\""),
                "the vector asset lost its viewBox") &&
         ok;
    ok = expect(home != nullptr && contains(home->bytes, "stroke=\"currentColor\""),
                "currentColor must survive so the paint pass can tint it") &&
         ok;
    ok = expect(home != nullptr && home->bytes.back() == '\n',
                "generated files end with a newline") &&
         ok;

    // The manifest is what tessera_embed_assets consumes; the header maps name -> asset.
    const icons::EmittedFile* manifest = find_file(*basic, "icons.manifest");
    ok = expect(manifest != nullptr &&
                    contains(manifest->bytes, "icons/tabler/home.svg = icons/tabler/home.svg"),
                "the embed manifest is missing an asset") &&
         ok;
    const icons::EmittedFile* header = find_file(*basic, "icons.g.hpp");
    ok = expect(header != nullptr &&
                    contains(header->bytes,
                             "{\"tabler:home\", \"icons/tabler/home.svg\", 24, 24, false}"),
                "the generated header is missing an icon entry") &&
         ok;

    // Attribution: every set, its license, the copyright holder and the CC-BY requirement.
    const icons::EmittedFile* attribution = find_file(*basic, "ICON_LICENSES.txt");
    ok = expect(attribution != nullptr, "attribution was not generated") && ok;
    if (attribution != nullptr) {
        ok = expect(contains(attribution->bytes, "tabler (Tabler Icons)"),
                    "attribution is missing a set") &&
             ok;
        ok = expect(contains(attribution->bytes, "License: MIT"),
                    "attribution is missing a license") &&
             ok;
        ok = expect(contains(attribution->bytes, "Copyright: Pawel Kuna"),
                    "attribution is missing the copyright holder") &&
             ok;
        ok = expect(contains(attribution->bytes,
                             "Attribution required: icons by CC-BY Icons authors under CC-BY-4.0"),
                    "attribution is missing the CC-BY requirement") &&
             ok;
        ok = expect(contains(attribution->bytes, "Icons: home, house, search"),
                    "attribution is missing the imported icon names") &&
             ok;
    }

    // Determinism: a second run produces identical bytes, both match the checked-in golden, and the
    // written tree checks against itself.
    const auto written_a = icons::write_files(out_a.string(), basic->files);
    ok = expect(written_a.has_value(), "writing the first run failed") && ok;
    const auto again = icons::import_icons(fixture_request(references));
    ok = expect(again.has_value(), "the second import failed") && ok;
    if (again) {
        ok = expect(again->files.size() == basic->files.size(),
                    "the two runs emitted a different number of files") &&
             ok;
        for (std::size_t i = 0; i < basic->files.size() && i < again->files.size(); ++i) {
            ok = expect(basic->files[i].path == again->files[i].path,
                        "the two runs disagree on the file order") &&
                 ok;
            ok = expect(basic->files[i].bytes == again->files[i].bytes,
                        "the two runs disagree on " + basic->files[i].path) &&
                 ok;
        }
        const auto written_b = icons::write_files(out_b.string(), again->files);
        ok = expect(written_b.has_value(), "writing the second run failed") && ok;
        ok = expect(list_files(out_a) == list_files(out_b), "the two runs wrote different files") &&
             ok;
    }
    ok = expect(icons::check_files(out_a.string(), basic->files).has_value(),
                "the written output does not check against itself") &&
         ok;
    ok = matches_golden(*basic, out_a, golden_root / "basic") && ok;

    // Monochrome replaces the palette with currentColor; without the flag the palette is preserved.
    icons::ImportRequest monochrome = fixture_request({"ccby:chart"});
    monochrome.monochrome = true;
    const auto monochrome_result = icons::import_icons(monochrome);
    ok = expect(monochrome_result.has_value(), "the monochrome import failed") && ok;
    if (monochrome_result) {
        const icons::EmittedFile* chart = find_file(*monochrome_result, "icons/ccby/chart.svg");
        ok = expect(chart != nullptr && !contains(chart->bytes, "#3366cc"),
                    "monochrome did not replace the palette") &&
             ok;
        ok = expect(chart != nullptr && contains(chart->bytes, "fill=\"currentColor\""),
                    "monochrome did not paint with currentColor") &&
             ok;
        const auto wrote = icons::write_files(out_mono.string(), monochrome_result->files);
        ok = expect(wrote.has_value(), "writing the monochrome run failed") && ok;
        ok = matches_golden(*monochrome_result, out_mono, golden_root / "monochrome") && ok;
    }
    const auto palette = icons::import_icons(fixture_request({"ccby:chart"}));
    ok = expect(palette.has_value(), "the palette import failed") && ok;
    if (palette) {
        const icons::EmittedFile* chart = find_file(*palette, "icons/ccby/chart.svg");
        ok = expect(chart != nullptr && contains(chart->bytes, "#3366cc"),
                    "the palette must be preserved without --monochrome") &&
             ok;
    }

    if (!ok) {
        return 1;
    }
    std::cout << "icons_import: ok\n";
    return 0;
}
