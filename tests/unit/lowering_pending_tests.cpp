// SPDX-License-Identifier: MIT

// Slice 1 of issue #656 (ADR-067, docs/specs/pending-ui-v1.md): the front end of pending UI. A
// <Suspense fallback={...}> element lowers to the IR Suspense node, lazy(() => import(...)) is
// identity, and useTransition() lowers to a transition-marked setter action plus a read-only
// isPending value. The IR is checked by the validator and against a golden JSON file.

#include "lowering_module_test_util.hpp"
#include "tessera/compiler/ids/assign_identity.hpp"
#include "tessera/compiler/ir/validate.hpp"

#include <fstream>
#include <sstream>

namespace {

using namespace lowering_module_test;

constexpr std::string_view kChart = "export function Chart() {\n"
                                    "  return <View><Text>chart</Text></View>;\n"
                                    "}\n";

constexpr std::string_view kPending =
    "import { lazy, Suspense } from \"react\";\n"
    "const Chart = lazy(() => import(\"./Chart\"));\n"
    "export function Dashboard() {\n"
    "  return (\n"
    "    <Suspense fallback={<Text>Loading</Text>}>\n"
    "      <Chart />\n"
    "    </Suspense>\n"
    "  );\n"
    "}\n"
    "export function Search() {\n"
    "  const [query, setQuery] = useState<string>(\"\");\n"
    "  const [isPending, startTransition] = useTransition();\n"
    "  return (\n"
    "    <View>\n"
    "      <Button onClick={() => startTransition(() => setQuery(\"next\"))}>\n"
    "        <Text>Search</Text>\n"
    "      </Button>\n"
    "      {isPending && <Text>Updating</Text>}\n"
    "    </View>\n"
    "  );\n"
    "}\n";

ir::IrModule into_module(lowering::LoweredModule&& lowered) {
    ir::IrModule module;
    module.name = std::move(lowered.name);
    module.components = std::move(lowered.components);
    module.bindings = std::move(lowered.bindings);
    module.stylesheets = std::move(lowered.stylesheets);
    module.types = std::move(lowered.types);
    return module;
}

// The validator needs the numeric identity of slice 10, so it is stamped in place first.
std::string validation_codes(ir::IrModule& module) {
    check(tessera::compiler::ids::assign_identity(module, "pending").has_value(),
          "identity assignment succeeds");
    std::string out;
    for (const auto& diagnostic : ir::validate(module)) {
        out += diagnostic.code + ";";
    }
    return out;
}

std::unique_ptr<ir::IrModule> lowered_module(lowering::LoweredModule&& lowered) {
    return std::make_unique<ir::IrModule>(into_module(std::move(lowered)));
}

std::optional<ir::IrModule> lower_pending() {
    Project project;
    project.add(parse_file("ui/Chart.tsx", std::string(kChart)));
    project.add(parse_file("ui/Pending.tsx", std::string(kPending)));
    auto result = project.lower("pending");
    check(result.has_value(), "the pending project lowers: " + first_message(result));
    if (!result) {
        return std::nullopt;
    }
    return into_module(std::move(*result));
}

const ir::Component* find(const ir::IrModule& module, std::string_view name) {
    for (const ir::Component& component : module.components) {
        if (component.name == name) {
            return &component;
        }
    }
    return nullptr;
}

void test_suspense_and_lazy() {
    auto module = lower_pending();
    if (!module) {
        return;
    }
    check_eq(validation_codes(*module), "", "the lowered module validates");
    const ir::Component* dashboard = find(*module, "Dashboard");
    check(dashboard != nullptr, "Dashboard is present");
    if (dashboard == nullptr) {
        return;
    }
    const ir::Node& root = dashboard->root;
    check_eq(std::string(ir::node_kind_name(root.kind)), "Suspense", "the root is a Suspense node");
    check(root.fallback != nullptr, "the fallback exists");
    if (root.fallback != nullptr) {
        check_eq(std::string(ir::node_kind_name(root.fallback->kind)), "Text", "the fallback kind");
    }
    check(root.children.size() == 1, "one guarded child");
    if (root.children.size() == 1) {
        check_eq(std::string(ir::node_kind_name(root.children[0].kind)), "Component",
                 "lazy is identity: the child is the component itself");
        check_eq(root.children[0].component, "comp::Chart", "the lazy component's id");
    }
    check(root.props.empty() && root.events.empty(), "a Suspense carries no props or events");
}

void test_lazy_outside_suspense_is_identity() {
    Project project;
    project.add(parse_file("ui/Chart.tsx", std::string(kChart)));
    project.add(parse_file("ui/App.tsx",
                           "const Chart = lazy(() => import(\"./Chart\"));\n"
                           "export function App() { return <View><Chart /></View>; }\n"));
    const auto result = project.lower("m");
    check(result.has_value(), "a lazy component outside Suspense lowers: " + first_message(result));
    if (!result) {
        return;
    }
    const ir::Component* app = find_component(*result, "App");
    check(app != nullptr && app->root.children.size() == 1 &&
              app->root.children[0].component == "comp::Chart",
          "the lazy component is used directly");
}

void test_transition_lowering() {
    const auto module = lower_pending();
    if (!module) {
        return;
    }
    const ir::Component* search = find(*module, "Search");
    check(search != nullptr, "Search is present");
    if (search == nullptr) {
        return;
    }
    check(search->handlers.size() == 1, "one inline handler");
    if (search->handlers.size() == 1 && search->handlers[0].actions.size() == 1) {
        const ir::Action& action = search->handlers[0].actions[0];
        check(action.kind == ir::ActionKind::Set, "the transition lowers to a set action");
        check_eq(action.target, "state::query", "the target is the written state");
        check(action.transition, "the set action carries the transition mark");
    } else {
        check(false, "the handler has exactly one action");
    }
    check(search->behaviors.size() == 1, "useTransition is one behaviors entry");
    if (search->behaviors.size() == 1) {
        const ir::Behavior& behavior = search->behaviors[0];
        check_eq(behavior.id, "beh::isPending", "the id is named from the pending binding");
        check_eq(behavior.hook, "useTransition", "the hook name is carried");
        check_eq(behavior.behavior, "transition", "the engine behavior is transition");
        check(behavior.targets.empty() && behavior.args.empty(), "no targets and no args");
        check(behavior.outputs.size() == 1 && behavior.outputs[0].name == "isPending" &&
                  behavior.outputs[0].type == "bool",
              "one bool output");
    }
    const std::string json = describe_component(*search);
    check(json.find("\"transition\": true") != std::string::npos, "the JSON writes the mark");
    check(json.find("\"id\": \"beh::isPending.isPending\"") != std::string::npos,
          "isPending is readable as an IR value");
}

void test_no_pending_syntax_writes_no_pending_fields() {
    const auto fixture = parse_file(
        "ui/Plain.tsx", "export function Plain() {\n"
                        "  const [n, setN] = useState<int>(0);\n"
                        "  return <Button onClick={() => setN(1)}><Text>x</Text></Button>;\n"
                        "}\n");
    const auto source = fixture->module_source();
    const auto result = lowering::lower_module(source, "m");
    check(result.has_value(), "a plain component lowers");
    if (!result) {
        return;
    }
    const std::string json = describe_component(result->components[0]);
    check(json.find("transition") == std::string::npos, "no transition field");
    check(json.find("behaviors") == std::string::npos, "no behaviors field");
}

// A transition is rejected when it has nothing to mark, or when its form is not the one the spec
// accepts, either by the analysis or by the lowering.
bool rejected(const std::string& hooks, const std::string& jsx_text) {
    const std::string text =
        "export function A() {\n  const [q, setQ] = useState<string>(\"\");\n" + hooks +
        "  return " + jsx_text + ";\n}\n";
    const auto fixture = parse_file_raw("ui/A.tsx", text);
    if (!fixture->parsed.ok() || fixture->analyzed.has_errors()) {
        return true;
    }
    const auto source = fixture->module_source();
    return !lowering::lower_module(source, "m").has_value();
}

void test_transition_rejections() {
    const std::string start = "  const [isPending, startTransition] = useTransition();\n";
    check(rejected(start,
                   "<Button onClick={() => startTransition(() => {})}><Text>a</Text></Button>"),
          "an empty transition arrow is rejected");
    check(rejected(start,
                   "<Button onClick={() => startTransition(() => { setQ(\"a\"); setQ(\"b\"); })}>"
                   "<Text>a</Text></Button>"),
          "one setter call per transition in V1");
    check(rejected(start, "<Button onClick={() => startTransition(setQ)}><Text>a</Text></Button>"),
          "the argument must be an arrow");
    check(
        rejected(start,
                 "<Button onClick={() => startTransition((x) => setQ(x))}><Text>a</Text></Button>"),
        "the arrow takes no parameters");
    check(rejected("  const [isPending, startTransition] = useTransition(1);\n", "<View />"),
          "useTransition takes no arguments");
    check(rejected("  const t = useTransition();\n", "<View />"), "useTransition binds the pair");
    check(rejected(
              start + "  const [x, setX] = useState<int>(0);\n",
              "<Button onClick={() => startTransition(() => setX(1), 2)}><Text>a</Text></Button>"),
          "extra arguments are rejected");
}

void test_lazy_rejections() {
    check(rejected("", "<View />") == false, "the baseline form is accepted");
    const char* bad[] = {
        "const Chart = lazy(() => import(\"./Chart\").then((m) => ({ default: m.Chart })));\n",
        "const Chart = lazy(async () => import(\"./Chart\"));\n",
        "const Chart = lazy(() => Chart2);\n",
        "const chart = lazy(() => import(\"./Chart\"));\n",
        "const Chart = lazy(() => import(spec));\n",
    };
    for (const char* line : bad) {
        const auto fixture = parse_file_raw("ui/A.tsx", std::string(line) + kChart.data());
        bool found = false;
        for (const auto& diagnostic : fixture->parsed.diagnostics) {
            found = found || std::string(diagnostic.code) == "TESSERA_REACT_UNSUPPORTED_API";
        }
        check(found, std::string("lazy form rejected with the unsupported-API code: ") + line);
    }
}

void test_suspense_rejections() {
    const auto rejects = [](const std::string& body) {
        const auto fixture =
            parse_file_raw("ui/A.tsx", "export function A() { return " + body + "; }\n");
        if (!fixture->parsed.ok() || fixture->analyzed.has_errors()) {
            return true;
        }
        const auto source = fixture->module_source();
        auto lowered = lowering::lower_module(source, "m");
        if (!lowered) {
            return true;
        }
        // A missing fallback is the validator's rule (TESSERA_IR_ERROR_BOUNDARY), not the
        // lowering's.
        return !validation_codes(*lowered_module(std::move(*lowered))).empty();
    };
    check(rejects("<Suspense><View /></Suspense>"), "Suspense needs a fallback");
    check(rejects("<Suspense fallback={<View />}><View /><View /></Suspense>"),
          "Suspense takes one child");
    check(rejects("<Suspense fallback={<View />} resetKey=\"k\"><View /></Suspense>"),
          "Suspense has no resetKey");
    check(rejects("<Suspense fallback={<View />} className=\"x\"><View /></Suspense>"),
          "Suspense accepts no className");
    check(rejects("<Suspense fallback={<View />}></Suspense>"), "Suspense needs its child");
    check(rejects("<Suspense fallback={<><View /></>}><View /></Suspense>"),
          "the fallback is not a fragment");
    check(rejects("<Suspense fallback={<View />} onError={() => {}}><View /></Suspense>"),
          "Suspense has no onError");
    check(!rejects("<Suspense fallback={<View />}><View /></Suspense>"),
          "the plain form is accepted");
}

// The golden is the lowered pending module as the IR JSON writer prints it. Delete the file to
// regenerate it (the test then fails once, so the new text is reviewed before it is committed).
void test_golden() {
    const auto module = lower_pending();
    if (!module) {
        return;
    }
    const std::string actual = ir::to_text(ir::to_json(*module));
    const std::string path = std::string(TESSERA_IR_GOLDEN_DIR) + "/pending-ui.json";
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        std::ofstream(path, std::ios::binary) << actual;
        check(false, "the golden was missing and has been written: review " + path);
        return;
    }
    std::ostringstream buffer;
    buffer << in.rdbuf();
    check_eq(actual, buffer.str(), "the pending module matches tests/golden/ir/pending-ui.json");
}

} // namespace

int main() {
    test_suspense_and_lazy();
    test_lazy_outside_suspense_is_identity();
    test_transition_lowering();
    test_no_pending_syntax_writes_no_pending_fields();
    test_transition_rejections();
    test_lazy_rejections();
    test_suspense_rejections();
    test_golden();
    return finish("lowering pending tests");
}
