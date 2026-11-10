// SPDX-License-Identifier: MIT

// IR writer/validator coverage for the constructs lowering slices 6 to 9 emit: conditions, loop
// templates (`item`/`index`/`source`/`key`), Component references with slot children, text runs and
// template parts, style/className/events, bindings and the ErrorBoundary fallback.
//
// Round-trip: write -> parse the JSON into the generic document model -> write again must be
// byte-identical. A full IR JSON reader is still deferred (jsx-ir-lowering decision 15), so the
// round-trip verifies the canonical text (sorted keys, fixed formatting), not a re-parsed IR model.
// The writer goldens are tests/golden/ir/{counter,rich}.json; update them deliberately with
// `ir_json_tests --write` after a reviewed writer change (see docs/specs/ir-v1.md).

#include "ir_samples.hpp"
#include "ir_test_util.hpp"
#include "tessera/compiler/ir/ir_json.hpp"
#include "tessera/compiler/ir/validate.hpp"

#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace ir_test;

// --- a minimal generic JSON parser for the round-trip check (test only) -----------------------

class JsonParser {
public:
    explicit JsonParser(std::string_view text)
        : text_(text) {}

    ir::JsonValue parse() {
        ir::JsonValue root = value();
        space();
        if (pos_ != text_.size()) {
            fail("trailing text");
        }
        return root;
    }

private:
    [[noreturn]] void fail(std::string_view what) const {
        throw std::runtime_error("JSON parse: " + std::string(what) + " at " +
                                 std::to_string(pos_));
    }

    char peek() const {
        return pos_ < text_.size() ? text_[pos_] : '\0';
    }

    void space() {
        while (pos_ < text_.size() && std::isspace(static_cast<unsigned char>(text_[pos_])) != 0) {
            ++pos_;
        }
    }

    bool consume(std::string_view token) {
        if (text_.substr(pos_, token.size()) != token) {
            return false;
        }
        pos_ += token.size();
        return true;
    }

    void expect(std::string_view token) {
        if (!consume(token)) {
            fail("expected " + std::string(token));
        }
    }

    ir::JsonValue value() {
        space();
        switch (peek()) {
        case '{':
            return object();
        case '[':
            return array();
        case '"':
            return ir::JsonValue::string(string());
        case 't':
            expect("true");
            return ir::JsonValue::boolean(true);
        case 'f':
            expect("false");
            return ir::JsonValue::boolean(false);
        case 'n':
            expect("null");
            return ir::JsonValue{};
        default:
            return number();
        }
    }

    ir::JsonValue object() {
        expect("{");
        ir::JsonValue json = ir::JsonValue::object();
        space();
        if (consume("}")) {
            return json;
        }
        while (true) {
            space();
            const std::string key = string();
            space();
            expect(":");
            json.set(key, value());
            space();
            if (consume("}")) {
                return json;
            }
            expect(",");
        }
    }

    ir::JsonValue array() {
        expect("[");
        ir::JsonValue json = ir::JsonValue::array();
        space();
        if (consume("]")) {
            return json;
        }
        while (true) {
            json.push(value());
            space();
            if (consume("]")) {
                return json;
            }
            expect(",");
        }
    }

    static int hex(char c) {
        if (c >= '0' && c <= '9') {
            return c - '0';
        }
        if (c >= 'a' && c <= 'f') {
            return c - 'a' + 10;
        }
        if (c >= 'A' && c <= 'F') {
            return c - 'A' + 10;
        }
        return -1;
    }

    std::string string() {
        expect("\"");
        std::string out;
        while (true) {
            if (pos_ >= text_.size()) {
                fail("unterminated string");
            }
            const char c = text_[pos_++];
            if (c == '"') {
                return out;
            }
            if (c != '\\') {
                out.push_back(c);
                continue;
            }
            if (pos_ >= text_.size()) {
                fail("unterminated escape");
            }
            const char escaped = text_[pos_++];
            switch (escaped) {
            case '"':
                out.push_back('"');
                break;
            case '\\':
                out.push_back('\\');
                break;
            case '/':
                out.push_back('/');
                break;
            case 'b':
                out.push_back('\b');
                break;
            case 'f':
                out.push_back('\f');
                break;
            case 'n':
                out.push_back('\n');
                break;
            case 'r':
                out.push_back('\r');
                break;
            case 't':
                out.push_back('\t');
                break;
            case 'u': {
                int code = 0;
                for (int i = 0; i < 4; ++i) {
                    if (pos_ >= text_.size()) {
                        fail("short \\u escape");
                    }
                    const int digit = hex(text_[pos_++]);
                    if (digit < 0) {
                        fail("bad \\u escape");
                    }
                    code = code * 16 + digit;
                }
                if (code > 0x7F) {
                    fail("the writer only escapes control characters as \\u00XX");
                }
                out.push_back(static_cast<char>(code));
                break;
            }
            default:
                fail("bad escape");
            }
        }
    }

    ir::JsonValue number() {
        const std::size_t start = pos_;
        if (peek() == '-') {
            ++pos_;
        }
        while (pos_ < text_.size() && std::isdigit(static_cast<unsigned char>(text_[pos_])) != 0) {
            ++pos_;
        }
        bool real = false;
        if (peek() == '.') {
            real = true;
            ++pos_;
            while (pos_ < text_.size() &&
                   std::isdigit(static_cast<unsigned char>(text_[pos_])) != 0) {
                ++pos_;
            }
        }
        if (peek() == 'e' || peek() == 'E') {
            real = true;
            ++pos_;
            if (peek() == '+' || peek() == '-') {
                ++pos_;
            }
            while (pos_ < text_.size() &&
                   std::isdigit(static_cast<unsigned char>(text_[pos_])) != 0) {
                ++pos_;
            }
        }
        if (pos_ == start) {
            fail("expected a value");
        }
        const std::string token(text_.substr(start, pos_ - start));
        if (real) {
            return ir::JsonValue::number(std::strtod(token.c_str(), nullptr));
        }
        return ir::JsonValue::integer(std::strtoll(token.c_str(), nullptr, 10));
    }

    std::string_view text_;
    std::size_t pos_ = 0;
};

std::string canonical(std::string_view text) {
    const ir::JsonValue parsed = JsonParser(text).parse();
    return ir::to_text(parsed);
}

// --- helpers ----------------------------------------------------------------------------------

std::string codes_of(const ir::IrModule& module) {
    std::string out;
    for (const auto& diagnostic : ir::validate(module)) {
        out += diagnostic.code + ";";
    }
    return out;
}

bool contains_code(const std::string& codes, std::string_view code) {
    return codes.find(code) != std::string::npos;
}

ir::Component& list(ir::IrModule& module) {
    return module.components[0];
}

// List.root children: [0] Component row, [1] ErrorBoundary.
ir::Node& row(ir::IrModule& module) {
    return list(module).root.children[0];
}
ir::Node& boundary(ir::IrModule& module) {
    return list(module).root.children[1];
}
ir::Node& label(ir::IrModule& module) {
    return boundary(module).children[0];
}

constexpr const char* kCondition = "TESSERA_IR_CONDITION;";
constexpr const char* kLoop = "TESSERA_IR_LOOP;";
constexpr const char* kBoundary = "TESSERA_IR_ERROR_BOUNDARY;";
constexpr const char* kMalformed = "TESSERA_IR_MALFORMED;";

void test_round_trip() {
    ir::IrModule rich = make_rich_module();
    const std::string text = ir::write_ir_json(rich);
    check_eq(canonical(text), text, "the rich module JSON round-trips through the document model");
    const std::string counter = ir::write_ir_json(make_counter_module());
    check_eq(canonical(counter), counter, "the Counter JSON round-trips");
    check_eq(ir::write_ir_json(rich), ir::write_ir_json(make_rich_module()),
             "a fresh module writes the same bytes");
}

void test_writer_covers_slice_constructs() {
    const std::string text = ir::write_ir_json(make_rich_module());
    const auto has = [&](std::string_view needle) {
        return text.find(needle) != std::string::npos;
    };
    check(has("\"condition\": {"), "condition is written as an expression object");
    check(has("\"loop\": {") && has("\"kind\": \"loop\""), "loop object and kind");
    check(has("\"item\": \"item\"") && has("\"index\": \"index\""), "loop item and index");
    check(has("\"source\": {"), "loop source is an expression object");
    check(has("\"key\": {"), "an expression key is written");
    check(has("\"component\": \"comp::Row\"") && has("\"kind\": \"Component\""),
          "component reference");
    check(has("\"children\": ["), "children are written (including the Component slot child)");
    check(has("\"kind\": \"Slot\""), "the static children slot is written as a Slot node");
    check(has("\"fallback\": {"), "ErrorBoundary fallback");
    check(has("\"text\": {"), "Text content is an expression object");
    check(has("\"style\": {") && has("\"className\": \"root\""), "style and className props");
    check(has("\"events\": {") && has("\"onPick\": \"handler::1\""), "events");
    check(has("\"deps\": [") && has("\"cleanup\": \"bind::on_sync_cleanup\""),
          "effect deps and cleanup");
    check(!has("\"body\""), "a loop has no body");
}

void test_validator_new_invariants() {
    {
        ir::IrModule module = make_valid_module();
        std::vector<ir::TemplatePart> parts;
        parts.push_back(ir::text_part("x"));
        label(module).condition = ir::make_template(std::move(parts));
        check(contains_code(codes_of(module), kCondition), "a template cannot be a condition");
    }
    {
        ir::IrModule module = make_valid_module();
        label(module).condition = ir::make_literal(std::int64_t{1});
        check(contains_code(codes_of(module), kCondition), "a non-bool literal condition");
    }
    {
        ir::IrModule module = make_valid_module();
        list(module).root.text = ir::make_literal(std::string("x"));
        check(contains_code(codes_of(module), kMalformed), "text only on a Text node");
    }
    {
        ir::IrModule module = make_valid_module();
        row(module).style.emplace("width", ir::make_literal(std::string("1px")));
        check(contains_code(codes_of(module), kMalformed), "a Component node has props, not style");
    }
    {
        ir::IrModule module = make_valid_module();
        boundary(module).fallback->condition = ir::make_ref("state::n");
        check(contains_code(codes_of(module), kBoundary),
              "an ErrorBoundary fallback is not conditional");
    }
    {
        ir::IrModule module = make_valid_module();
        row(module).loop->index = row(module).loop->item;
        check(contains_code(codes_of(module), kLoop), "a loop index differs from the item");
    }
    {
        ir::IrModule module = make_valid_module();
        row(module).loop->item = "row.x";
        check(contains_code(codes_of(module), kLoop), "a loop item is a local name");
    }
}

void test_component_slot_children_are_valid() {
    ir::IrModule module = make_valid_module();
    ir::Node child;
    child.kind = ir::NodeKind::View;
    child.id = "node::90";
    child.ui_id = "90";
    child.source = span(260, 10, 18, 3, 18, 13);
    row(module).children.push_back(std::move(child));
    check_eq(codes_of(module), "", "a Component node may carry the caller's slot children");
}

void test_binding_kind_invariants() {
    {
        ir::IrModule module = make_valid_module();
        for (ir::Binding& binding : module.bindings) {
            if (binding.id == "bind::on_pick") {
                binding.kind = ir::BindingKind::Effect;
            }
        }
        check(contains_code(codes_of(module), "TESSERA_IR_BINDING;"),
              "an event must name an event binding");
    }
    {
        ir::IrModule module = make_valid_module();
        for (ir::Binding& binding : module.bindings) {
            if (binding.id == "bind::on_sync") {
                binding.kind = ir::BindingKind::Event;
            }
        }
        check(contains_code(codes_of(module), "TESSERA_IR_BINDING;"),
              "an effect must name an effect binding");
    }
}

void test_valid_module_is_clean() {
    check_eq(codes_of(make_valid_module()), "", "the valid sample validates");
}

void test_output_is_deterministic_across_threads() {
    constexpr std::size_t kThreads = 8;
    std::array<std::string, kThreads> texts;
    std::array<std::thread, kThreads> threads;
    for (std::size_t i = 0; i < kThreads; ++i) {
        threads[i] = std::thread([&texts, i] {
            const ir::IrModule module = make_rich_module();
            texts[i] = ir::write_ir_json(module) + codes_of(module);
        });
    }
    for (std::thread& thread : threads) {
        thread.join();
    }
    for (std::size_t i = 1; i < kThreads; ++i) {
        check_eq(texts[i], texts[0], "concurrent writer/validator runs agree with thread 0");
    }
    check_eq(ir::write_ir_json(make_rich_module()), ir::write_ir_json(make_rich_module()),
             "two writes of the same IR agree");
}

} // namespace

int main() {
    test_round_trip();
    test_writer_covers_slice_constructs();
    test_validator_new_invariants();
    test_component_slot_children_are_valid();
    test_binding_kind_invariants();
    test_valid_module_is_clean();
    test_output_is_deterministic_across_threads();
    return finish("ir construct tests");
}
