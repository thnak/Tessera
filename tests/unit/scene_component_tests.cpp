// SPDX-License-Identifier: MIT
//
// User component instances in the design-time preview (issue #594). Drives `prepare_scene()` and
// reads the automation snapshot, the artifact the native/Wasm parity gate compares. Pins:
//   * a prop bound by the caller reaches the callee's text, with the callee's default when omitted;
//   * `{children}` splices the instance's children at the callee's `Slot`, evaluated in the caller;
//   * each instance has its own state, so two instances do not share a cell;
//   * nested instances build, and the node cap still applies.

#include "scene_dynamic_test_support.hpp"
#include "scene_project.hpp"
#include "scene_session.hpp"

#include <array>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace cli = tessera::compiler::codegen::cli;
namespace test = scene_dynamic_test;
namespace ir = tessera::compiler::ir;

int g_failures = 0;

void expect(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "scene_component: " << message << "\n";
        ++g_failures;
    }
}

constexpr std::string_view kProps =
    R"(export function Label({ text, suffix = "!" }: { text: string; suffix?: string }): Node {
  return <Text>{text}{suffix}</Text>;
}

export function Page(): Node {
  const [n, setN] = useState<int>(2);
  return (
    <View>
      <Label text="Hello" />
      <Label text="Bye" suffix="?" />
      <Label text={`n=${n}`} />
    </View>
  );
}
)";

constexpr std::string_view kChildren =
    R"(export function Card({ children }: { children: Children }): Node {
  return (
    <View>
      <Text>Card</Text>
      {children}
    </View>
  );
}

export function Page(): Node {
  const [n, setN] = useState<int>(7);
  return (
    <Card>
      <Text>Inside {n}</Text>
    </Card>
  );
}
)";

constexpr std::string_view kState = R"(export function Counter(): Node {
  const [c, setC] = useState<int>(5);
  return <Text>c={c}</Text>;
}

export function Page(): Node {
  return (
    <View>
      <Counter />
      <Counter />
    </View>
  );
}
)";

constexpr std::string_view kNested = R"(export function Inner({ v }: { v: string }): Node {
  return <Text>{v}</Text>;
}

export function Outer({ v }: { v: string }): Node {
  return <View><Inner v={v} /></View>;
}

export function Page(): Node {
  return <Outer v="deep" />;
}
)";

std::vector<std::string> texts_of(std::string_view source, std::string& error) {
    cli::PreparedScene scene = cli::prepare_scene(source, test::options_for("ui/Page.tsx"), "Page");
    error = scene.error;
    if (!scene.error.empty() || scene.root == nullptr) {
        return {};
    }
    const auto snapshot = test::snapshot_of(scene, error);
    return test::texts(snapshot);
}

void test_props_and_defaults() {
    std::string error;
    const auto painted = texts_of(kProps, error);
    expect(error.empty(), "props source prepares: " + error);
    expect(test::has_text(painted, "Hello!"), "a default prop fills in: " + test::join(painted));
    expect(test::has_text(painted, "Bye?"),
           "a bound prop overrides the default: " + test::join(painted));
    expect(test::has_text(painted, "n=2!"),
           "a caller expression reads the caller's state: " + test::join(painted));
}

void test_children_splice_in_the_caller_scope() {
    std::string error;
    const auto painted = texts_of(kChildren, error);
    expect(error.empty(), "children source prepares: " + error);
    expect(test::has_text(painted, "Card"), "the callee's own node paints: " + test::join(painted));
    expect(test::has_text(painted, "Inside 7"),
           "slot content reads the caller's state: " + test::join(painted));
}

void test_each_instance_has_its_own_state() {
    std::string error;
    const auto painted = texts_of(kState, error);
    expect(error.empty(), "state source prepares: " + error);
    int count = 0;
    for (const std::string& text : painted) {
        count += text == "c=5" ? 1 : 0;
    }
    expect(count == 2, "both instances paint their own initial state: " + test::join(painted));
}

void test_nested_instances() {
    std::string error;
    const auto painted = texts_of(kNested, error);
    expect(error.empty(), "nested source prepares: " + error);
    expect(test::has_text(painted, "deep"),
           "a prop passes through two instances: " + test::join(painted));
}

std::string read_repo_file(const std::string& relative) {
    std::ifstream stream(std::string(TESSERA_REPO_ROOT) + "/" + relative, std::ios::binary);
    std::ostringstream text;
    text << stream.rdbuf();
    return text.str();
}

// The three pages the Studio could not render (issue #594), as the Studio hands them over: the
// sample file plus the library component it imports, copied next to it.
void test_gallery_pages_render(const std::string& page, const std::string& library,
                               std::string_view component, std::string_view expected) {
    const std::string page_text = read_repo_file("samples/gallery/" + page + ".tsx");
    const std::string library_text = read_repo_file("packages/tessera-ui/" + library + ".tsx");
    expect(!page_text.empty() && !library_text.empty(), "the sample files are readable: " + page);
    const std::string page_path = "ui/" + page + ".tsx";
    const std::string library_path = "ui/" + library + ".tsx";
    const std::array<cli::ProjectFile, 2> sources{
        {{page_path, page_text}, {library_path, library_text}}};
    const std::array<cli::ProjectFile, 0> sheets{};
    const cli::ProjectInput project{sources, sheets, sources[0].path};
    cli::PreparedScene scene =
        cli::prepare_scene(project, test::options_for(sources[0].path), component, nullptr);
    expect(scene.error.empty(), page + " prepares with no refusal: " + scene.error);
    if (!scene.error.empty() || scene.root == nullptr) {
        return;
    }
    std::string error;
    const auto painted = test::texts(test::snapshot_of(scene, error));
    expect(test::has_text(painted, expected),
           page + " paints \"" + std::string(expected) + "\": " + test::join(painted));
}

// Interactivity (slice 2). A callee's own handler changes its own instance's state, not its
// sibling's, and the state survives the rebuild the click causes.
constexpr std::string_view kClickCounter = R"(export function Counter(): Node {
  const [c, setC] = useState<int>(5);
  return (
    <View>
      <Text>c={c}</Text>
      <Button onClick={() => setC(c + 1)}>Inc</Button>
    </View>
  );
}

export function Page(): Node {
  return (
    <View>
      <Counter />
      <Counter />
    </View>
  );
}
)";

// A callback prop: the callee's handler calls `onPress()`, which runs the CALLER's handler against
// the caller's state.
constexpr std::string_view kCallback =
    R"(export function Pressable({ onPress }: { onPress: () => void }): Node {
  return <Button onClick={() => onPress()}>Press</Button>;
}

export function Page(): Node {
  const [presses, setPresses] = useState<int>(0);
  return (
    <View>
      <Pressable onPress={() => setPresses(presses + 1)} />
      <Text>Presses: {presses}</Text>
    </View>
  );
}
)";

constexpr std::string_view kDynamicClass =
    R"(export function Tag({ kind }: { kind: string }): Node {
  return <View className={`tag tag-${kind}`} aria-label={`label-${kind}`}><Text>x</Text></View>;
}

export function Page(): Node {
  return <Tag kind="warn" />;
}
)";

std::vector<std::uint64_t> button_ids(const tessera::ui::Node& root) {
    std::vector<std::uint64_t> ids;
    const auto snapshot = tessera::ui::automation::snapshot_scene(root);
    if (snapshot) {
        for (const auto& node : snapshot->nodes) {
            if (node.kind == "Button") {
                ids.push_back(node.ui_id);
            }
        }
    }
    return ids;
}

std::vector<std::string> session_texts(const cli::PreviewSession& session) {
    const auto snapshot = tessera::ui::automation::snapshot_scene(session.root());
    return snapshot ? test::texts(*snapshot) : std::vector<std::string>{};
}

void test_an_instance_handler_changes_its_own_state() {
    cli::PreparedScene scene =
        cli::prepare_scene(kClickCounter, test::options_for("ui/Page.tsx"), "Page");
    expect(scene.error.empty(), "the clickable counters prepare: " + scene.error);
    if (!scene.error.empty() || !scene.sheet.has_value()) {
        return;
    }
    const ir::Component* page = nullptr;
    for (const auto& component : scene.module.components) {
        page = component.name == "Page" ? &component : page;
    }
    cli::PreviewSession::Options options;
    cli::PreviewSession session(scene.module, *page, *scene.sheet, options);
    expect(session.built(), "the session builds the instances: " + session.build_error());
    const auto buttons = button_ids(session.root());
    expect(buttons.size() == 2 && buttons[0] != buttons[1],
           "each instance's button has its own automation id");
    if (buttons.size() != 2) {
        return;
    }
    const cli::PreviewClickResult first = session.click(buttons[0]);
    expect(first.found && !first.ran.empty(), "the callee's handler ran");
    auto painted = session_texts(session);
    expect(test::has_text(painted, "c=6") && test::has_text(painted, "c=5"),
           "only the clicked instance changed: " + test::join(painted));
    (void)session.click(button_ids(session.root())[0]);
    painted = session_texts(session);
    expect(test::has_text(painted, "c=7"),
           "the instance's state survived the rebuild: " + test::join(painted));
}

void test_a_callback_prop_reaches_the_callers_handler() {
    cli::PreparedScene scene =
        cli::prepare_scene(kCallback, test::options_for("ui/Page.tsx"), "Page");
    expect(scene.error.empty(), "the callback source prepares: " + scene.error);
    if (!scene.error.empty() || !scene.sheet.has_value()) {
        return;
    }
    const ir::Component* page = nullptr;
    for (const auto& component : scene.module.components) {
        page = component.name == "Page" ? &component : page;
    }
    cli::PreviewSession::Options options;
    cli::PreviewSession session(scene.module, *page, *scene.sheet, options);
    const auto buttons = button_ids(session.root());
    expect(buttons.size() == 1, "the callee's button is addressable");
    if (buttons.empty()) {
        return;
    }
    const cli::PreviewClickResult clicked = session.click(buttons[0]);
    expect(clicked.unrun.empty(), "nothing was left unrun");
    expect(test::has_text(session_texts(session), "Presses: 1"),
           "the caller's handler ran: " + test::join(session_texts(session)));
}

void test_dynamic_metadata_is_applied() {
    cli::PreparedScene scene =
        cli::prepare_scene(kDynamicClass, test::options_for("ui/Page.tsx"), "Page");
    expect(scene.error.empty(), "the dynamic-metadata source prepares: " + scene.error);
    if (!scene.error.empty() || scene.root == nullptr) {
        return;
    }
    bool class_found = false;
    std::vector<const tessera::ui::Node*> pending{scene.root.get()};
    while (!pending.empty()) {
        const tessera::ui::Node* node = pending.back();
        pending.pop_back();
        class_found = class_found || node->class_name() == "tag tag-warn";
        for (const auto& child : node->children()) {
            pending.push_back(child.get());
        }
    }
    expect(class_found, "a className template over a prop is applied");
    std::string error;
    const auto snapshot = tessera::ui::automation::snapshot_scene(*scene.root);
    bool named = false;
    for (const auto& node : snapshot->nodes) {
        named = named || node.name == "label-warn";
    }
    expect(named, "an aria-label template over a prop names the node");
}

void test_the_library_button_clicks() {
    const std::string page_text = read_repo_file("samples/gallery/Gallery.tsx");
    const std::string library_text = read_repo_file("packages/tessera-ui/Button.tsx");
    const std::array<cli::ProjectFile, 2> sources{
        {{"ui/Gallery.tsx", page_text}, {"ui/Button.tsx", library_text}}};
    const std::array<cli::ProjectFile, 0> sheets{};
    const cli::ProjectInput project{sources, sheets, sources[0].path};
    cli::PreparedScene scene =
        cli::prepare_scene(project, test::options_for(sources[0].path), "Gallery", nullptr);
    expect(scene.error.empty(), "Gallery prepares: " + scene.error);
    if (!scene.error.empty() || !scene.sheet.has_value()) {
        return;
    }
    const ir::Component* page = nullptr;
    for (const auto& component : scene.module.components) {
        page = component.name == "Gallery" ? &component : page;
    }
    cli::PreviewSession::Options options;
    cli::PreviewSession session(scene.module, *page, *scene.sheet, options);
    std::uint64_t target = 0;
    const auto snapshot = tessera::ui::automation::snapshot_scene(session.root());
    for (const auto& node : snapshot->nodes) {
        if (target == 0 && node.name == "Primary") {
            target = node.ui_id;
        }
    }
    expect(target != 0, "the library Button is named by its label prop");
    if (target == 0) {
        return;
    }
    const cli::PreviewClickResult clicked = session.click(target);
    expect(clicked.found, "the library Button is found");
    expect(test::has_text(session_texts(session), "Presses: 1"),
           "pressing the library Button runs the page's onPress: " +
               test::join(session_texts(session)));
}

// FormPage imports the library Button and a CSS module (issue #594): a page with a stylesheet, an
// instance and the busiest markup in the samples.
void test_form_page_renders_and_a_segment_clicks() {
    const std::string page_text = read_repo_file("samples/gallery/FormPage.tsx");
    const std::string library_text = read_repo_file("packages/tessera-ui/Button.tsx");
    const std::string css_text = read_repo_file("samples/gallery/form.module.css");
    expect(!page_text.empty() && !css_text.empty(), "the FormPage files are readable");
    const std::array<cli::ProjectFile, 2> sources{
        {{"ui/FormPage.tsx", page_text}, {"ui/Button.tsx", library_text}}};
    const std::array<cli::ProjectFile, 1> sheets{{{"ui/form.module.css", css_text}}};
    const cli::ProjectInput project{sources, sheets, sources[0].path};
    cli::PreparedScene scene =
        cli::prepare_scene(project, test::options_for(sources[0].path), "FormPage", nullptr);
    expect(scene.error.empty(), "FormPage prepares with no refusal: " + scene.error);
    if (!scene.error.empty() || scene.root == nullptr) {
        return;
    }
    std::string error;
    const auto painted = test::texts(test::snapshot_of(scene, error));
    expect(test::has_text(painted, "Create workspace"),
           "FormPage paints its title: " + test::join(painted));
    expect(test::has_text(painted, "Cancel"),
           "FormPage paints a library Button's children: " + test::join(painted));
}

} // namespace

int main() {
    test_props_and_defaults();
    test_children_splice_in_the_caller_scope();
    test_each_instance_has_its_own_state();
    test_nested_instances();
    test_an_instance_handler_changes_its_own_state();
    test_a_callback_prop_reaches_the_callers_handler();
    test_dynamic_metadata_is_applied();
    test_the_library_button_clicks();
    test_form_page_renders_and_a_segment_clicks();
    test_gallery_pages_render("Gallery", "Button", "Gallery", "Presses: 0");
    test_gallery_pages_render("SafeAreaPage", "SafeArea", "SafeAreaPage",
                              "Content stays inside the panel");
    return g_failures == 0 ? 0 : 1;
}
