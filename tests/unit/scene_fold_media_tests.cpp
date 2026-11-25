// SPDX-License-Identifier: MIT
//
// The preview half of ADR-074: the Studio's simulated fold answers `@media (device-posture)` and
// the viewport-segment features through the same `FoldInfo` the layout uses and the same
// `with_fold` the app host calls. A posture change re-resolves the running tree's cascade in place
// only when an answer flips, and a document built at a posture starts with its rules applied.

#include "scene_canvas_session.hpp"
#include "scene_layout.hpp"
#include "scene_prepare.hpp"
#include "scene_stylesheet.hpp"
#include "tessera/hal/fold_posture.hpp"
#include "tessera/ui/scene/node.hpp"
#include "tessera/ui/style/media_query.hpp"
#include "tessera/ui/style/scene_style.hpp"

#include <iostream>
#include <string>
#include <string_view>

namespace {

namespace cli = tessera::compiler::codegen::cli;
namespace hal = tessera::hal;
namespace ui = tessera::ui;
namespace style = tessera::ui::style;

int failures = 0;

bool expect(bool condition, const std::string_view message) {
    if (!condition) {
        std::cerr << "scene_fold_media: " << message << "\n";
        ++failures;
    }
    return condition;
}

constexpr std::string_view kPage = R"(import "./Preview.css";
export function Page(): Node {
  return (
    <View className="page">
      <Text>Title</Text>
    </View>
  );
}
)";

constexpr std::string_view kBase = "rgb(20, 40, 60)";
constexpr std::string_view kFolded = "rgb(30, 160, 90)";
constexpr std::string_view kTwoPanes = "rgb(200, 30, 30)";

constexpr std::string_view kCss = R"(
.page { width: 100%; height: 100%; background-color: rgb(20, 40, 60); }
@media (device-posture: folded) { .page { background-color: rgb(30, 160, 90); } }
@media (vertical-viewport-segments: 2) { .page { background-color: rgb(200, 30, 30); } }
)";

cli::CanvasSessionSource source_at(hal::FoldPosture posture) {
    cli::CanvasSessionSource source;
    source.text = kPage;
    source.file = "ui/Page.tsx";
    source.salt = "preview";
    source.stylesheet = kCss;
    source.width = 800.0f;
    source.height = 600.0f;
    source.dpi_scale = 1.0f;
    source.posture = posture;
    return source;
}

const ui::Node* find_page(const ui::Node& node) {
    if (node.class_name() == "page") {
        return &node;
    }
    for (const auto& child : node.children()) {
        if (const ui::Node* found = find_page(*child)) {
            return found;
        }
    }
    return nullptr;
}

std::string background_of(const cli::CanvasSceneSession& session, const ui::Node& node) {
    style::SceneStyleTree tree(node);
    const style::ComputedStyle computed = tree.compute(session.sheet(), node);
    const std::string* value = computed.find("background-color");
    return value != nullptr ? *value : std::string();
}

// The context a preview resolves against equals the one the app host builds from the same fold, so
// a previewed foldable and a device cannot answer a fold query differently.
void test_preview_context_is_the_hosts_mapping() {
    for (const hal::FoldPosture posture : {hal::FoldPosture::None, hal::FoldPosture::Flat,
                                           hal::FoldPosture::Book, hal::FoldPosture::Tabletop}) {
        const style::MediaContext preview =
            cli::preview_media_context(1000.0f, 700.0f, 1.0f, posture);
        const style::MediaContext hosted =
            style::with_fold(style::MediaContext{}, hal::fold_info_for(posture, 1000, 700));
        const std::string name{hal::fold_posture_name(posture)};
        expect(preview.device_posture == hosted.device_posture &&
                   preview.horizontal_viewport_segments == hosted.horizontal_viewport_segments &&
                   preview.vertical_viewport_segments == hosted.vertical_viewport_segments,
               "the preview and the host agree on the fold answers for posture `" + name + "`");
    }
    const style::MediaContext book =
        cli::preview_media_context(1000.0f, 700.0f, 1.0f, hal::FoldPosture::Book);
    expect(book.device_posture == style::MediaPosture::Folded &&
               book.horizontal_viewport_segments == 2 && book.vertical_viewport_segments == 1,
           "a book posture is folded with two horizontal segments");
    const style::MediaContext plain = cli::preview_media_context(1000.0f, 700.0f, 1.0f);
    expect(plain.device_posture == style::MediaPosture::Continuous &&
               plain.horizontal_viewport_segments == 1 && plain.vertical_viewport_segments == 1,
           "the default is no fold");
}

// A document started at a posture has the rules for that posture on its first build.
void test_start_at_a_posture() {
    cli::CanvasSceneSession plain;
    expect(plain.start(source_at(hal::FoldPosture::None)).empty(), "the plain page starts");
    const ui::Node* plain_page = plain.root() != nullptr ? find_page(*plain.root()) : nullptr;
    expect(plain_page != nullptr && background_of(plain, *plain_page) == kBase,
           "no fold resolves the base rule");

    cli::CanvasSceneSession book;
    expect(book.start(source_at(hal::FoldPosture::Book)).empty(), "the book page starts");
    const ui::Node* book_page = book.root() != nullptr ? find_page(*book.root()) : nullptr;
    expect(book_page != nullptr && background_of(book, *book_page) == kFolded,
           "a book posture resolves @media (device-posture: folded) on the first build");

    cli::CanvasSceneSession tabletop;
    expect(tabletop.start(source_at(hal::FoldPosture::Tabletop)).empty(),
           "the tabletop page starts");
    const ui::Node* tabletop_page =
        tabletop.root() != nullptr ? find_page(*tabletop.root()) : nullptr;
    expect(
        tabletop_page != nullptr && background_of(tabletop, *tabletop_page) == kTwoPanes,
        "a tabletop posture resolves the vertical-segments rule, which comes later in the sheet");
}

// A posture change re-resolves the running tree in place, once per answer that flips.
void test_set_posture_re_resolves_in_place() {
    cli::CanvasSceneSession session;
    expect(session.start(source_at(hal::FoldPosture::None)).empty(), "the page starts");
    const ui::Node* page = session.root() != nullptr ? find_page(*session.root()) : nullptr;
    if (page == nullptr) {
        expect(false, "the running tree has a page node");
        return;
    }
    expect(background_of(session, *page) == kBase, "the tree starts unfolded");
    const std::uint64_t started = session.cascade_generation();

    expect(session.set_posture(hal::FoldPosture::Book), "folding runs a layout pass");
    expect(session.cascade_generation() == started + 1, "folding re-resolves the cascade once");
    expect(background_of(session, *page) == kFolded, "the running tree resolves the folded rule");

    // Book to tabletop is folded to folded, so the posture answer does not move; the vertical
    // segment count does, and that rule is in the sheet, so the cascade is stale once more.
    expect(session.set_posture(hal::FoldPosture::Tabletop), "turning the hinge runs a layout pass");
    expect(session.cascade_generation() == started + 2,
           "a flipped segment answer re-resolves the cascade");
    expect(background_of(session, *page) == kTwoPanes, "and the vertical-segments rule applies");

    expect(session.set_posture(hal::FoldPosture::Flat), "unfolding runs a layout pass");
    expect(session.cascade_generation() == started + 3, "unfolding re-resolves once");
    expect(background_of(session, *page) == kBase, "unfolding restores the base rule");

    // The posture already in force is not a change.
    expect(!session.set_posture(hal::FoldPosture::Flat), "the same posture runs no pass");
    expect(session.cascade_generation() == started + 3, "and rebuilds nothing");
}

} // namespace

int main() {
    test_preview_context_is_the_hosts_mapping();
    test_start_at_a_posture();
    test_set_posture_re_resolves_in_place();
    if (failures == 0) {
        std::cout << "scene_fold_media: ok\n";
    }
    return failures == 0 ? 0 : 1;
}
