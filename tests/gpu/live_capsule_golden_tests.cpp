// SPDX-License-Identifier: MIT
//
// LiveCapsule goldens (issue #968, docs/specs/live-capsule-v1.md section 9): every state with a
// notch, a centred punch-hole and no cutout, on the light, dark and high-contrast Instrument
// themes, plus frames caught part-way through the expand morph under the manual clock. Each image
// is compared with a committed lavapipe baseline in tests/golden/baseline/live_capsule/; a machine
// without lavapipe (or without Vulkan) prints a skip line. Regenerate on a software device only
// (ADR-009):
//
//     live_capsule_golden_tests --update-goldens

#include "tessera/hal/window_cutout_fold.hpp"
#include "tessera/harness/frame_capture.hpp"
#include "tessera/harness/frame_check.hpp"
#include "tessera/ui/capsule/live_capsule.hpp"
#include "tessera/ui/scene/node.hpp"
#include "tessera/ui/scene/paint_data.hpp"
#include "tessera/ui/style/theme_asset.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#ifndef TESSERA_TEST_FONT
#error "TESSERA_TEST_FONT must point at the repository test font"
#endif
#ifndef TESSERA_CAPSULE_GOLDEN_DIR
#error "TESSERA_CAPSULE_GOLDEN_DIR must point at tests/golden/baseline/live_capsule"
#endif
#ifndef TESSERA_CAPSULE_CSS
#error "TESSERA_CAPSULE_CSS must point at packages/tessera-ui/live-capsule.css"
#endif
#ifndef TESSERA_CAPSULE_THEME
#error "TESSERA_CAPSULE_THEME must point at the Instrument theme asset"
#endif

namespace {

using namespace tessera::ui::capsule;
using namespace tessera::harness;
using tessera::ui::Node;

constexpr std::uint32_t kWidth = 360;
constexpr std::uint32_t kHeight = 200;
constexpr float kFrame = 1.0F / 60.0F;

int failures = 0;
int checked = 0;
int skipped = 0;
bool update_goldens = false;

bool expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "live_capsule_golden: " << message << "\n";
        ++failures;
    }
    return condition;
}

std::optional<std::string> read_file(const char* path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return std::nullopt;
    }
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

// Keeps frame.png, the diff and the compare report of a failing case for review.
class DirectorySink final : public ArtifactSink {
public:
    explicit DirectorySink(std::filesystem::path dir)
        : dir_(std::move(dir)) {}

    std::expected<void, ProtocolError> write(std::string_view path,
                                             std::string_view bytes) override {
        std::error_code ec;
        std::filesystem::create_directories(dir_, ec);
        std::ofstream out(dir_ / std::string(path), std::ios::binary | std::ios::trunc);
        out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        return {};
    }

private:
    std::filesystem::path dir_;
};

enum class Form : std::uint8_t { Minimal, Compact, Expanded, MidExpand };

struct Cutout {
    const char* name;
    // Zero width means none.
    tessera::hal::WindowRect rect;
};

constexpr Cutout kCutouts[] = {
    {"none", {0.0F, 0.0F, 0.0F, 0.0F}},
    {"notch", {150.0F, 0.0F, 60.0F, 30.0F}},
    {"punch", {170.0F, 0.0F, 20.0F, 20.0F}},
};

struct Theme {
    const char* name;
    const char* id;
};

constexpr Theme kThemes[] = {
    {"light", "instrument-light"},
    {"dark", "instrument-dark"},
    {"high_contrast", "instrument-high-contrast"},
};

const char* form_name(Form form) {
    switch (form) {
    case Form::Minimal:
        return "minimal";
    case Form::Compact:
        return "compact";
    case Form::Expanded:
        return "expanded";
    case Form::MidExpand:
        return "mid_expand";
    }
    return "";
}

void text_node(Node& parent, tessera::ui::NodeId id, const char* text) {
    Node& node = parent.add_child(std::make_unique<Node>(id));
    node.set_element_kind("Text");
    tessera::ui::PaintData data;
    data.kind = tessera::ui::PaintKind::Text;
    data.text = text;
    node.set_paint_data(std::move(data));
}

void dot(Node& slot) {
    Node& node = slot.add_child(std::make_unique<Node>(tessera::ui::NodeId{7001}));
    node.set_element_kind("View");
    node.set_inline_style("width", "14px");
    node.set_inline_style("height", "14px");
    node.set_inline_style("border-radius", "7px");
    node.set_inline_style("background-color", "var(--accent)");
}

Activity timer(Form form) {
    Activity a;
    a.id = "timer";
    a.label = "Timer running";
    a.form = form == Form::Minimal ? ActivityForm::Minimal : ActivityForm::Compact;
    a.slots.leading_width = 28.0F;
    a.slots.trailing_width = 44.0F;
    a.slots.title = "Timer";
    a.slots.leading = [](Node& slot) { dot(slot); };
    a.slots.trailing = [](Node& slot) { text_node(slot, 7002, "0:42"); };
    a.slots.minimal_icon = [](Node& slot) { dot(slot); };
    a.slots.content = [](Node& body) { text_node(body, 7003, "Kitchen, 0:42 left"); };
    a.slots.card_width = 280.0F;
    a.slots.card_height = 120.0F;
    a.slots.actions.push_back({"Pause", {}});
    a.slots.actions.push_back({"Stop", {}});
    return a;
}

// One case: builds the scene, runs the capsule under the manual clock and compares with the
// baseline.
void run_case(Form form, const Cutout& cutout, const Theme& theme) {
    const std::string name = std::string(form_name(form)) + "_" + cutout.name + "_" + theme.name;

    const auto css = read_file(TESSERA_CAPSULE_CSS);
    const auto theme_json = read_file(TESSERA_CAPSULE_THEME);
    if (!expect(css && theme_json, "the capsule CSS and the theme asset are readable")) {
        return;
    }
    const auto asset = tessera::ui::style::parse_theme_asset(*theme_json);
    if (!expect(asset.has_value(), "the theme asset parses")) {
        return;
    }
    const std::vector<std::uint8_t> blob = tessera::ui::style::serialize_theme_asset(*asset);

    Node root(1);
    root.set_element_kind("View");
    root.set_inline_style("background-color", "var(--window-background)");
    if (cutout.rect.width > 0.0F) {
        // The hardware cutout itself, so the image shows the capsule hugging it.
        Node& hole = root.add_child(std::make_unique<Node>(tessera::ui::NodeId{2}));
        hole.set_element_kind("View");
        hole.set_inline_style("position", "absolute");
        hole.set_inline_style("left", std::to_string(static_cast<int>(cutout.rect.x)) + "px");
        hole.set_inline_style("top", "0px");
        hole.set_inline_style("width", std::to_string(static_cast<int>(cutout.rect.width)) + "px");
        hole.set_inline_style("height",
                              std::to_string(static_cast<int>(cutout.rect.height)) + "px");
        hole.set_inline_style("background-color", "#000000");
        hole.set_inline_style("border-radius", "0px 0px 12px 12px");
    }

    LiveCapsuleHost host;
    host.start(timer(form));
    LiveCapsuleEnv env;
    env.window_width = static_cast<float>(kWidth);
    env.window_height = static_cast<float>(kHeight);
    env.top_inset = 24.0F;
    if (cutout.rect.width > 0.0F) {
        env.cutouts.bounds[0] = cutout.rect;
        env.cutouts.count = 1;
    }

    std::unique_ptr<LiveCapsule> capsule;
    tessera::app::CaptureOptions options;
    options.width = kWidth;
    options.height = kHeight;
    options.font_path = TESSERA_TEST_FONT;
    options.configure_host = [&](tessera::app::AppHost& app) {
        app.register_embedded_theme(blob);
        (void)app.set_theme(theme.id);
        capsule = std::make_unique<LiveCapsule>(host, app.overlays());
        // Opens the overlay; it mounts at the first frame boundary.
        capsule->sync(env, 0.0F);
    };
    const auto prepare = [&] {
        capsule->sync(env, 0.0F); // builds the parts and snaps the size
        if (form == Form::Expanded) {
            capsule->activate(0);
            for (int i = 0; i < 600 && capsule->sync(env, kFrame); ++i) {
            }
        } else if (form == Form::MidExpand) {
            capsule->activate(0);
            for (int i = 0; i < 6; ++i) {
                (void)capsule->sync(env, kFrame);
            }
        }
        // The capture destroys its AppHost before this scope ends, so the capsule must not close
        // its overlay on a dead host. The frames still to come paint what is already mounted.
        capsule->abandon();
    };

    auto frame = capture_scene_frame(CompareMode::Golden, root, *css, options, prepare);
    if (!frame) {
        if (frame.error().unavailable()) {
            ++skipped;
            return;
        }
        expect(false, name + ": capture failed with a render error");
        return;
    }

    const std::filesystem::path baseline =
        std::filesystem::path(TESSERA_CAPSULE_GOLDEN_DIR) / (name + ".png");
    DirectorySink sink(std::filesystem::path("capsule-golden-artifacts") / name);
    FrameCheckRequest request;
    request.mode = CompareMode::Golden;
    request.baseline_path = baseline.string();
    request.update_baseline = update_goldens;
    // Text antialiasing rounds differently across lavapipe/LLVM revisions: the same small budget
    // the batched2d text goldens use (docs/specs/golden-tests-v1.md section 1).
    request.policy = ComparePolicy{1, 0.001};
    const auto result = check_frame(*frame, request, sink);
    ++checked;
    if (!result) {
        expect(false, name + ": frame check failed: " + result.error().message);
    } else if (update_goldens) {
        expect(result->baseline_updated || result->compare.passed(),
               name + ": matches or rewrote the committed baseline");
    } else {
        expect(result->ok(), name + ": the capture matches its baseline");
    }
}

} // namespace

int main(int argc, char** argv) {
    for (int index = 1; index < argc; ++index) {
        if (std::string_view(argv[index]) == "--update-goldens") {
            update_goldens = true;
            continue;
        }
        std::cerr << "live_capsule_golden: unknown argument " << argv[index] << "\n";
        return 2;
    }
    for (const Theme& theme : kThemes) {
        for (const Cutout& cutout : kCutouts) {
            for (const Form form : {Form::Minimal, Form::Compact, Form::Expanded}) {
                run_case(form, cutout, theme);
            }
        }
    }
    // The expand morph caught part-way, in the light theme, for each cutout.
    for (const Cutout& cutout : kCutouts) {
        run_case(Form::MidExpand, cutout, kThemes[0]);
    }
    if (failures != 0) {
        std::cerr << "live_capsule_golden: " << failures << " failure(s)\n";
        return 1;
    }
    if (checked == 0) {
        std::cout << "live_capsule_golden: skipped (no lavapipe or Vulkan device)\n";
        return 0;
    }
    std::cout << "live_capsule_golden: ok (" << checked << " frames)\n";
    return 0;
}
