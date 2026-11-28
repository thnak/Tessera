// SPDX-License-Identifier: MIT

// Symbol and span index (issue #570): declarations, references, UTF-16 spans, project resolution,
// incremental update equals a fresh build, and the invariants over the samples and the corpus.

#include "jsx_test_util.hpp"
#include "tessera/compiler/analysis/project_index.hpp"
#include "tessera/compiler/analysis/symbol_json.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>

using namespace tessera::compiler::analysis;
using jsx_test::check;
using jsx_test::check_eq;

namespace {

SourceFile make(FileId id, std::string path, std::string text) {
    return SourceFile{id, std::move(path), std::move(text)};
}

const Symbol* find_symbol(const ProjectIndex& index, FileId file, std::string_view name,
                          SymbolKind kind) {
    const FileIndex* f = index.file(file);
    if (f == nullptr) {
        return nullptr;
    }
    for (const Symbol& symbol : f->symbols) {
        if (symbol.name == name && symbol.kind == kind) {
            return &symbol;
        }
    }
    return nullptr;
}

std::vector<const Reference*> refs_named(const ProjectIndex& index, FileId file,
                                         std::string_view name, ReferenceKind kind) {
    std::vector<const Reference*> out;
    for (const Reference& reference : index.file(file)->references) {
        if (reference.name == name && reference.kind == kind) {
            out.push_back(&reference);
        }
    }
    return out;
}

constexpr const char* kCounter = R"(export function Counter() {
  const [count, setCount] = useState<int>(0);
  // Note: bind::add
  const add = (): void => {
    setCount(count + 2);
  };
  return (
    <View>
      <Text>{count}</Text>
      <Button onClick={add}>Add</Button>
      <Button onClick={(e) => setCount(0)}>Reset</Button>
      <div className="x">{missing}</div>
    </View>
  );
}
)";

void test_declarations_and_references() {
    const ProjectIndex index = ProjectIndex::build({make(0, "Counter.tsx", kCounter)});
    const FileIndex* file = index.file(0);
    check(file != nullptr && file->indexed, "counter is indexed");
    const Symbol* count = find_symbol(index, 0, "count", SymbolKind::State);
    const Symbol* set_count = find_symbol(index, 0, "setCount", SymbolKind::Setter);
    const Symbol* add = find_symbol(index, 0, "add", SymbolKind::Handler);
    const Symbol* component = find_symbol(index, 0, "Counter", SymbolKind::Component);
    check(count && set_count && add && component, "state, setter, handler and component declared");
    if (!(count && set_count && add && component)) {
        return;
    }
    check_eq(count->id, "f0:Counter/count", "state id is path based");
    check_eq(add->id, "f0:Counter/add", "handler id");
    check(component->exported, "component exported");
    check_eq(count->parent, component->id, "state parent is the component");
    const std::string text = kCounter;
    check_eq(text.substr(count->span.from, count->span.to - count->span.from), "count",
             "state span");
    const auto count_refs = index.references_to(count->id, false);
    check(count_refs.size() == 2, "count is read twice");
    check(index.references_to(count->id, true).size() == 3, "plus the declaration");
    check(index.references_to(set_count->id, false).size() == 2, "setCount used in two handlers");
    const auto add_refs = refs_named(index, 0, "add", ReferenceKind::Value);
    check(add_refs.size() == 1 && add_refs[0]->resolution.symbol == add->id, "add resolves");
    // The arrow parameter is in an anonymous scope with a stable id.
    check(find_symbol(index, 0, "e", SymbolKind::Param) != nullptr, "arrow parameter declared");
    check_eq(find_symbol(index, 0, "e", SymbolKind::Param)->id, "f0:Counter/~2/e",
             "anonymous scope id");
    // Markers: a library tag, an intrinsic tag, an unbound name and a hook.
    const auto view = refs_named(index, 0, "View", ReferenceKind::Tag);
    check(view.size() == 2 && view[0]->resolution.reason == Unresolved::Unbound,
          "library tag is unbound");
    const auto div = refs_named(index, 0, "div", ReferenceKind::Tag);
    check(div.size() == 2 && div[0]->resolution.reason == Unresolved::Intrinsic, "intrinsic tag");
    const auto missing = refs_named(index, 0, "missing", ReferenceKind::Value);
    check(missing.size() == 1 && missing[0]->resolution.reason == Unresolved::Unbound,
          "unbound name");
    const auto hook = refs_named(index, 0, "useState", ReferenceKind::Value);
    check(hook.size() == 1 && hook[0]->resolution.reason == Unresolved::Builtin, "hook is builtin");
    const auto type = refs_named(index, 0, "int", ReferenceKind::Type);
    check(type.size() == 1 && type[0]->resolution.reason == Unresolved::Builtin, "builtin type");
    const auto attr = refs_named(index, 0, "className", ReferenceKind::Attribute);
    check(attr.size() == 1 && attr[0]->resolution.reason == Unresolved::Intrinsic,
          "intrinsic attribute");
}

void test_locate() {
    const ProjectIndex index = ProjectIndex::build({make(0, "Counter.tsx", kCounter)});
    const std::string text = kCounter;
    const auto offset = static_cast<std::uint32_t>(text.find("{count}") + 2);
    const auto hit = index.locate(0, offset);
    check(hit && !hit->declaration && hit->symbol == "f0:Counter/count",
          "reference locates its declaration");
    const auto decl = index.locate(0, static_cast<std::uint32_t>(text.find("count, setCount") + 1));
    check(decl && decl->declaration && decl->symbol == "f0:Counter/count",
          "declaration locates itself");
    const auto none = index.locate(0, 0);
    check(!none || none->symbol.empty() || none->declaration,
          "keyword offset names nothing unresolved");
    const auto bad = index.locate(9, 0);
    check(!bad, "unknown file");
}

void test_cross_file() {
    const std::string button = "type P = { label: string };\n"
                               "export function Button({ label, tone }: P) {\n"
                               "  return <button>{label}</button>;\n}\n";
    const std::string page = "import { Button as B } from \"./ui/Button\";\n"
                             "import { Nope } from \"./ui/Nope\";\n"
                             "import styles from \"./Page.module.css\";\n"
                             "export function Page() {\n"
                             "  return <div className={styles.card}><B label=\"x\" tone=\"y\" "
                             "color=\"z\" key=\"k\" /><Nope /></div>;\n}\n";
    const ProjectIndex index =
        ProjectIndex::build({make(1, "src/Page.tsx", page), make(2, "src/ui/Button.tsx", button),
                             make(3, "src/Page.module.css", ".card{}")});
    check(!index.file(3)->indexed, "css file is stored but not indexed");
    const Symbol* alias = find_symbol(index, 1, "B", SymbolKind::Import);
    const Symbol* target = find_symbol(index, 2, "Button", SymbolKind::Component);
    check(alias && target && alias->target.symbol == target->id,
          "import resolves to the exported component");
    const Symbol* nope = find_symbol(index, 1, "Nope", SymbolKind::Import);
    check(nope && nope->target.reason == Unresolved::ImportUnresolved, "missing file is marked");
    const auto label = refs_named(index, 1, "label", ReferenceKind::Attribute);
    const Symbol* label_prop = find_symbol(index, 2, "label", SymbolKind::Prop);
    check(label.size() == 1 && label_prop && label[0]->resolution.symbol == label_prop->id,
          "attribute resolves to the prop of the imported component");
    check(refs_named(index, 1, "tone", ReferenceKind::Attribute)[0]->resolution.resolved(),
          "second prop");
    check(refs_named(index, 1, "color", ReferenceKind::Attribute)[0]->resolution.reason ==
              Unresolved::NeedsType,
          "unknown prop is marked");
    check(refs_named(index, 1, "key", ReferenceKind::Attribute)[0]->resolution.reason ==
              Unresolved::Builtin,
          "key is builtin");
    check(refs_named(index, 1, "Nope", ReferenceKind::Tag)[0]->resolution.symbol == nope->id,
          "tag resolves to the import symbol");
    const auto card = refs_named(index, 1, "card", ReferenceKind::CssClass);
    check(card.size() == 1 && card[0]->resolution.reason == Unresolved::External,
          "css class is external");
    const auto uses = index.references_to(target->id, true);
    check(uses.size() == 3, "declaration, import, tag");
    const bool across =
        std::ranges::any_of(uses, [](const Occurrence& o) { return o.span.file == 1; });
    check(across, "uses span files");
    // Uses of the prop include the attribute in the other file.
    check(index.references_to(label_prop->id, false).size() == 2,
          "prop used by body and attribute");
}

void test_type_re_export() {
    const ProjectIndex index = ProjectIndex::build(
        {make(0, "a.tsx", "export type Props = { n: int };\n"),
         make(1, "index.ts", "export type { Props } from \"./a\";\n"),
         make(2, "local.ts", "type L = { n: int };\nexport type { L as Shown };\n"),
         make(3, "use.tsx",
              "import type { Props } from \"./index\";\nimport type { Shown } from \"./local\";\n"
              "export function U(p: Props, q: Shown) { return <b />; }\n")});
    const Symbol* props = find_symbol(index, 3, "Props", SymbolKind::Import);
    check(props && props->target.symbol == "f0:Props",
          "type import follows a re-export from another file");
    const Symbol* shown = find_symbol(index, 3, "Shown", SymbolKind::Import);
    check(shown && shown->target.symbol == "f2:L", "type import follows a renamed local re-export");
    check(index.references_to("f0:Props", false).size() == 2, "re-export entry and annotation");
}

void test_utf16_offsets() {
    // "é" is 2 UTF-8 bytes and 1 UTF-16 unit; the emoji is 4 bytes and 2 units.
    const std::string text =
        "export function A() {\n  const s = \"é😀\";\n  const t = s;\n  return <b>{t}</b>;\n}\n";
    const ProjectIndex index = ProjectIndex::build({make(0, "a.tsx", text)});
    const Symbol* t = find_symbol(index, 0, "t", SymbolKind::Local);
    check(t != nullptr, "t declared");
    if (t == nullptr) {
        return;
    }
    // Count UTF-16 units up to the declaration by hand.
    const std::size_t byte = text.find("t = s");
    std::uint32_t units = 0;
    for (std::size_t i = 0; i < byte; ++i) {
        const auto c = static_cast<unsigned char>(text[i]);
        if ((c & 0xC0U) == 0x80U) {
            continue;
        }
        units += c >= 0xF0U ? 2 : 1;
    }
    check(t->span.from == units && t->span.to == units + 1,
          "declaration offset counts UTF-16 units");
    check(units < byte, "the fixture really has multi-byte text before the name");
    const auto uses = index.references_to(t->id, false);
    check(uses.size() == 1 && uses[0].span.from > t->span.from, "reference after the declaration");

    // A BOM is not counted, as a TextDecoder drops it.
    const ProjectIndex bom = ProjectIndex::build({make(0, "a.tsx", "\xEF\xBB\xBF" + text)});
    check(find_symbol(bom, 0, "t", SymbolKind::Local)->span.from == units, "BOM is not counted");
}

void test_incremental_equals_fresh() {
    const std::string a = "export function A() { return <div />; }\n";
    const std::string b = "import { A } from \"./a\";\nexport function B() { return <A />; }\n";
    ProjectIndex live = ProjectIndex::build({make(0, "a.tsx", a), make(1, "b.tsx", b)});
    // Rename A in a.tsx by editing the declaration (UTF-16 offset of "A" in "function A").
    const std::vector<TextEdit> edits = {
        {0, static_cast<std::uint32_t>(a.find("A")), 1, "Ä😀"},
        {0, 0, 0, "// é\n"},
    };
    check(live.apply_edits(edits), "edits apply");
    const std::string a2 = "// é\nexport function Ä😀() { return <div />; }\n";
    const ProjectIndex fresh = ProjectIndex::build({make(0, "a.tsx", a2), make(1, "b.tsx", b)});
    check_eq(symbols_to_json(live, std::nullopt), symbols_to_json(fresh, std::nullopt),
             "incremental update equals a fresh build");
    check(*live.text(0) == a2, "text follows the edits");
    // The import no longer resolves; the dependent file's index changed without being reparsed.
    check(find_symbol(live, 1, "A", SymbolKind::Import)->target.reason ==
              Unresolved::ImportUnresolved,
          "dependents re-resolve");
    check(!live.apply_edits(std::vector<TextEdit>{{0, 9999, 1, ""}}), "range past the end refused");
    check(!live.apply_edits(std::vector<TextEdit>{{7, 0, 0, ""}}), "unknown file refused");
    check_eq(symbols_to_json(live, std::nullopt), symbols_to_json(fresh, std::nullopt),
             "refusal changes nothing");
    live.remove_file(1);
    check(live.file(1) == nullptr, "file removed");
}

void test_determinism_and_recovery() {
    const std::string good = "export function A() { return <div />; }\n";
    const std::string broken = "export function Bad( {\n";
    const std::string after = "export function C() { return <p />; }\n";
    const auto one =
        ProjectIndex::build({make(0, "a.tsx", good), make(1, "b.tsx", broken + after)});
    const auto two =
        ProjectIndex::build({make(1, "b.tsx", broken + after), make(0, "a.tsx", good)});
    check_eq(symbols_to_json(one, std::nullopt), symbols_to_json(two, std::nullopt),
             "order of input files");
    check(find_symbol(one, 0, "A", SymbolKind::Component) != nullptr, "good file indexed");
    check_eq(symbols_to_json(one, 0).substr(0, 20), "{\"files\":[{\"file\":0,",
             "single file selection");
    check_eq(analysis_capabilities_json(),
             "{\"name\":\"analysis\",\"protocol\":1,\"features\":[\"symbols\",\"diagnostics\","
             "\"definition\","
             "\"references\",\"hover\"]}",
             "capabilities");
}

std::string read(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

bool ascii(const std::string& text) {
    return std::ranges::all_of(text, [](char c) { return static_cast<unsigned char>(c) < 0x80U; });
}

// Every symbol and reference of every sample and corpus file is well formed: a span inside the
// file that spells the name, and either a declaration or an explicit marker.
void test_samples_and_corpus() {
    std::vector<std::filesystem::path> paths;
    for (const char* root : {TESSERA_SAMPLES_DIR, TESSERA_CORPUS_DIR}) {
        for (const auto& entry : std::filesystem::recursive_directory_iterator(root)) {
            if (entry.is_regular_file() && entry.path().extension() == ".tsx") {
                paths.push_back(entry.path());
            }
        }
    }
    std::ranges::sort(paths);
    check(paths.size() > 40, "corpus found");
    std::size_t references = 0;
    std::size_t resolved = 0;
    for (const auto& path : paths) {
        const std::string text = read(path);
        const ProjectIndex index = ProjectIndex::build({make(0, path.filename().string(), text)});
        const FileIndex* file = index.file(0);
        const std::string label = path.string();
        if (!file->indexed || !ascii(text)) {
            continue;
        }
        for (const Symbol& symbol : file->symbols) {
            const bool inside = symbol.span.to <= text.size();
            check(inside && text.substr(symbol.span.from, symbol.span.to - symbol.span.from) ==
                                symbol.name,
                  "symbol span spells its name: " + symbol.name + " in " + label);
        }
        for (const Reference& reference : file->references) {
            ++references;
            const bool inside = reference.span.to <= text.size();
            check(inside && text.substr(reference.span.from,
                                        reference.span.to - reference.span.from) == reference.name,
                  "reference span spells its name: " + reference.name + " in " + label);
            check(reference.resolution.resolved() !=
                      (reference.resolution.reason != Unresolved::None),
                  "resolved or marked: " + reference.name + " in " + label);
            if (reference.resolution.resolved()) {
                ++resolved;
                check(index.symbol(reference.resolution.symbol) != nullptr,
                      "target exists: " + reference.name);
            }
        }
    }
    check(references > 500 && resolved > 100, "the corpus exercises the index");
}

} // namespace

int main() {
    test_declarations_and_references();
    test_locate();
    test_cross_file();
    test_type_re_export();
    test_utf16_offsets();
    test_incremental_equals_fresh();
    test_determinism_and_recovery();
    test_samples_and_corpus();
    return jsx_test::failures == 0 ? 0 : 1;
}
