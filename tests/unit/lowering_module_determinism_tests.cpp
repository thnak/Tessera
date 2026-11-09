// SPDX-License-Identifier: MIT

// Determinism of module lowering (docs/design/jsx-ir-lowering.md section 7): module ordering,
// component ids, node ids, handler numbering and binding order are a pure function of the files;
// lowering twice and on eight threads gives identical text.

#include "lowering_module_test_util.hpp"

#include <array>
#include <memory>
#include <string>
#include <string_view>
#include <thread>

namespace {

using namespace lowering_module_test;

constexpr std::string_view kCard =
    "export function Card(props: P) {\n"
    "  return <View className={props.className}><Text>{props.label}</Text></View>;\n"
    "}\n";

constexpr std::string_view kApp =
    "import { Card } from \"./Card\";\n"
    "export function App(props: P) {\n"
    "  const [count, setCount] = useState<int>(0);\n"
    "  const [flag, setFlag] = useState(true);\n"
    "  return (\n"
    "    <View>\n"
    "      {flag && <Card label=\"on\" />}\n"
    "      {flag ? <Card label=\"a\" /> : <Card label=\"b\" />}\n"
    "      {props.rows.map((row, index) => <Card key={row.id} label={row.label} "
    "onPick={() => setCount(1)} />)}\n"
    "    </View>\n"
    "  );\n"
    "}\n";

std::string describe_project() {
    Project project;
    project.add(parse_file("ui/Card.tsx", std::string(kCard)));
    project.add(parse_file("ui/App.tsx", std::string(kApp)));
    const auto result = project.lower("factory");
    check(result.has_value(), "the determinism project lowers");
    if (!result) {
        return "<no IR>";
    }
    return describe(*result);
}

void test_two_lowerings_agree() {
    const std::string first = describe_project();
    const std::string second = describe_project();
    check(first != "<no IR>", "the determinism project lowers");
    check_eq(first, second, "lowering twice gives identical text");
    check(first.find('\r') == std::string::npos, "the description has no CR");
    check(first.find("\"name\": \"App\"") != std::string::npos, "App is in the output");
    check(first.find("\"name\": \"Card\"") != std::string::npos, "Card is in the output");
}

void test_same_project_lowers_twice() {
    auto card = parse_file("ui/Card.tsx", std::string(kCard));
    auto app = parse_file("ui/App.tsx", std::string(kApp));
    Project project;
    project.add(std::move(card));
    project.add(std::move(app));
    const auto first = project.lower("factory");
    const auto second = project.lower("factory");
    check(first.has_value() && second.has_value(), "the same project lowers twice");
    if (first.has_value() && second.has_value()) {
        check_eq(describe(*first), describe(*second), "module lowering is idempotent");
        check(first->components.size() == second->components.size(), "component count is stable");
        if (first->components.size() == 2 && second->components.size() == 2) {
            check_eq(first->components[0].name, second->components[0].name,
                     "component order is stable");
            check_eq(first->components[1].name, second->components[1].name,
                     "component order is stable");
        }
    }
}

void test_eight_threads_agree() {
    constexpr std::size_t kThreads = 8;
    std::array<std::string, kThreads> texts;
    std::array<std::thread, kThreads> threads;
    for (std::size_t i = 0; i < kThreads; ++i) {
        threads[i] = std::thread([&texts, i] { texts[i] = describe_project(); });
    }
    for (std::thread& thread : threads) {
        thread.join();
    }
    for (std::size_t i = 1; i < kThreads; ++i) {
        check_eq(texts[i], texts[0], "concurrent module lowering agrees with thread 0");
    }
}

} // namespace

int main() {
    test_two_lowerings_agree();
    test_same_project_lowers_twice();
    test_eight_threads_agree();
    return finish("lowering module determinism tests");
}
