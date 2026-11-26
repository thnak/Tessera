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

#include <array>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace cli = tessera::compiler::codegen::cli;
namespace test = scene_dynamic_test;

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

} // namespace

int main() {
    test_props_and_defaults();
    test_children_splice_in_the_caller_scope();
    test_each_instance_has_its_own_state();
    test_nested_instances();
    test_gallery_pages_render("Gallery", "Button", "Gallery", "Presses: 0");
    test_gallery_pages_render("SafeAreaPage", "SafeArea", "SafeAreaPage",
                              "Content stays inside the panel");
    return g_failures == 0 ? 0 : 1;
}
