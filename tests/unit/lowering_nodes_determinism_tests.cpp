// SPDX-License-Identifier: MIT

// Determinism of node lowering (docs/design/jsx-ir-lowering.md section 7): node ids, handler
// numbering, prop/style/event maps and diagnostics are a pure function of the AST and the analysis,
// so lowering twice and on eight threads gives identical text.

#include "lowering_nodes_test_util.hpp"

#include <array>
#include <string>
#include <string_view>
#include <thread>

namespace {

using namespace lowering_nodes_test;

constexpr std::string_view kSource =
    "function A(props: P) {\n"
    "  const [count, setCount] = useState<int>(0);\n"
    "  const r = useRef<Button>(null);\n"
    "  // Note: bind::on_click\n"
    "  const click = (): void => { setCount(1); };\n"
    "  return (\n"
    "    <View className=\"box\" style={{ width: 10, opacity: 0.5 }}>\n"
    "      <Button key=\"go\" ref={r} onClick={click} {...{ disabled: true }}>Go</Button>\n"
    "      <Text>Count: {count}</Text>\n"
    "      <><Image src=\"a.png\" alt=\"A\" /></>\n"
    "      <ErrorBoundary fallback={<Text>oops</Text>}>\n"
    "        <View><Text>ok</Text></View>\n"
    "      </ErrorBoundary>\n"
    "    </View>\n"
    "  );\n"
    "}\n";

std::string describe_source(std::string_view source) {
    const auto fixture = parse(std::string(source));
    const auto result = lower(*fixture);
    if (!result) {
        check(false, "the determinism fixture lowers: " + describe_errors(*fixture, result));
        return "<no IR>";
    }
    std::string out = describe(result->component.root);
    out += " handlers=" + std::to_string(result->component.handlers.size());
    for (const ir::Handler& handler : result->component.handlers) {
        out += " " + handler.id;
    }
    for (const ir::Binding& binding : result->bindings) {
        out += " " + binding.id;
    }
    return out;
}

void test_two_lowerings_agree() {
    const std::string first = describe_source(kSource);
    const std::string second = describe_source(kSource);
    check(first != "<no IR>", "the determinism fixture lowers");
    check_eq(first, second, "lowering twice gives identical text");
    check(!first.empty() && first.back() != '\n', "the description is a single line");
    check(first.find('\r') == std::string::npos, "the description has no CR");
}

void test_same_fixture_lowers_twice() {
    const auto fixture = parse(std::string(kSource));
    const auto first = lower(*fixture);
    const auto second = lower(*fixture);
    check(first.has_value() && second.has_value(), "the same analysis lowers twice");
    if (first.has_value() && second.has_value()) {
        check_eq(describe(first->component.root), describe(second->component.root),
                 "lowering does not consume or change the analysis");
        check(first->component.handlers.size() == second->component.handlers.size(),
              "handler numbering is stable");
    }
}

void test_eight_threads_agree() {
    constexpr std::size_t kThreads = 8;
    std::array<std::string, kThreads> texts;
    std::array<std::thread, kThreads> threads;
    for (std::size_t i = 0; i < kThreads; ++i) {
        threads[i] = std::thread([&texts, i] { texts[i] = describe_source(kSource); });
    }
    for (std::thread& thread : threads) {
        thread.join();
    }
    for (std::size_t i = 1; i < kThreads; ++i) {
        check_eq(texts[i], texts[0], "concurrent lowering agrees with thread 0");
    }
}

} // namespace

int main() {
    test_two_lowerings_agree();
    test_same_fixture_lowers_twice();
    test_eight_threads_agree();
    return finish("lowering nodes determinism tests");
}
