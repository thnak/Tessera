# Security review v1 (issue #198)

Status: review, 2026-11-16, against `origin/main` `d16a7e1e`. This is a **review**, not a
specification: it records what was examined, what was found, and what was deliberately not examined.
Findings are filed as issues and are **not** fixed here; the fixes need their own PRs so the review
stays auditable. The standing security design is [`security.md`](../design/security.md); this
document does not replace it.

## 1. Scope

Reviewed, on the four surfaces issue #198 names:

1. **The threat model** (§2-§3): the trust boundaries between untrusted input and code that runs.
2. **Compiler resource limits** (§4): the front end, the lowerer, IR validation and codegen.
3. **IO surfaces** (§5): `appfw::Storage`, `hal::FileSystem`, the network module's URL parsing and
   redirect rules.
4. **Update and crash surfaces** (§6): the release manifest and update policy, crash capture, the
   uploader.

### What was not reviewed

- **The renderer, RHI, GPU/WebGPU back ends, the scene graph, layout, text, animation, input
  dispatch, accessibility and the automation harness.** Not one line. A GPU driver bug or an
  unvalidated Vulkan structure is out of scope for this pass.
- **The designer/Wasm bridge** (`designer/`, `wasm_*` entries, `designer-protocol-v1.md`). The
  sandbox claims in `security.md` section 5 are unverified.
- **Services beyond the files touched by §5**: `ServiceContext` was read, the lifecycle runner was
  not.
- **The dependencies themselves** (Yoga, FreeType, ICU, Vulkan loader, Mesa). No CVE pass, no
  version audit; `docs/specs/dependencies.md` was not re-derived.
- **The CI workflows and the supply chain gates.** `scripts/check_release_manifest.py` was read;
  the GitHub Actions definitions were not.
- **Network back ends**: none exists in-tree (§5.3), so nothing was reviewed beyond the module's own
  rules.
- **Fuzzing.** `security.md` section 10 asks for parser/IR/codegen fuzzing; there is none in the
  tree. The probes in this review are hand-written, not coverage-guided.

Where a claim below rests on reading rather than on a probe, it says **not probed**.

## 2. Trust boundaries

The model is a two-tier compiler that turns a TSX project into C++ which is then compiled and run.
The sharp edge is therefore not "can input reach a parser" but "what stops a project from writing
the code it is compiled into". Enumerated by input, worst outcome first:

| Input | Attacker controls | Worst outcome | Bound in this tree |
|---|---|---|---|
| `*.tsx` / `*.jsx` project source | arbitrary text up to 1 MiB | **generated C++ executes.** Any escape bug in the emitters is remote code execution at build time and then in the shipped binary | the fixed JSX subset; `TESSERA_JSX_*` diagnostics; every user string goes through `escape_string` (see §3) |
| stylesheet (`.css`, inline `style`) | arbitrary text | a crash in the compiler/runtime (§4.1, §4.2); a selector that matches more than it should | none in the selector/at-rule paths today — two crashes, both confirmed |
| `tessera.json` | paths, package roots, namespace, theme refs | read/write outside the project; a namespace that is not a C++ identifier | `read_packages`/`read_theme` reject absolute paths and `..`; `valid_namespace` rejects a non-identifier |
| asset (image, shader, font) | bytes | decoder memory corruption | **not reviewed** |
| `ui/elements.json` (custom elements) | class/source paths, shader refs | loading native code named by project data | **not reviewed** (and no custom-element loader was found in this pass) |
| deep link / activation | any URL the OS hands over | route injection, traversal into the app's route table | scheme and host allowlists; `.`/`..` segments rejected (`navigation_deep_link.cpp`) |
| network response | headers, body, redirects | credential leak on a hop; unbounded body | origin comparison drops `Authorization`; `max_response_bytes`; `kMaxRedirects` (§5.3) |
| update manifest | the manifest's fields, if the host is compromised | installing an attacker's binary | unsigned is refused; URL host allowlisted; the committed manifest is byte-compared to its derivation (§6.1) |
| crash report being uploaded | the record on disk (an attacker with write access to the crash directory) | exfiltrating a path or token that redaction missed | `redact_report` + `contains_unredacted` — **bypassable** (§6.2) |
| storage entry name (app data) | the name a caller passes | writing over another entry's data | traversal rejected; identity/aliasing **not** handled (§5.1) |

The compiler is a code-execution surface **by design**, and the honest bound is not a sandbox: it is
(a) the closed JSX subset, (b) the emitters' escaping, (c) the `bind::` contract, which makes a
handler a *declared C++ member the developer writes* rather than text copied out of the TSX. The
review found no path by which TSX text reaches the generated file unescaped (see §3, negative
results). It did find that the resource bounds which keep the compiler from dying on hostile input are
missing from two stages (§4), and that a stylesheet — an input nobody thinks of as code — can kill the
build tool outright.

## 3. Generated-code escaping (negative result)

Every place user text becomes C++ was read:

- `escape_string` (`src/compiler/codegen/src/emit_expr_names.cpp:45`) escapes `"`, `\`, `\n`, `\r`,
  `\t` and every byte below 0x20 as `\u00XX`. String literals are the only way user text enters a
  generated string.
- `escape_message` (`src/compiler/codegen/src/emit_base.cpp:126`) does the same for the
  `static_assert` message that carries TSX paths and binding names.
- `escaped_file` (`src/compiler/codegen/src/emit_line_map.cpp:57`) escapes `"` and `\` in the
  `#line` directive's file name. A newline cannot reach it: `parse_url`-style control rejection does
  not apply here, but the lexer's identifier/string spans cannot contain a raw newline, and the
  emitter writes one `#line` per statement.
- Identifiers are not escaped, they are **restricted**: the lexer's `is_ident_start`/`is_ident_part`
  (`src/compiler/analyzer/src/lexer.cpp:17`) accept `[A-Za-z0-9_$]`. A `$` is a legal C++ identifier
  character in practice (GCC/Clang/MSVC accept it as an extension), but `$A` as a component name is
  refused earlier by `TESSERA_JSX_SUBSET_COMPONENT_NAME`, and a `$x` local lowers through the
  action emitter, not through a raw identifier copy (probe: a `$x` local in a component raises no
  diagnostic and does not appear in a generated identifier). **Not probed** for every emitter.
- `ir::Service::type` and `ir::Service::header` are project-manifest data emitted into C++ verbatim
  (`emit_services.cpp:140`), validated only as "non-empty" (`validate_component.cpp:105-119`). Today
  nothing populates them from a project file (`ui/services.json` has no reader in the tree), so this
  is a **latent** injection point, not an exploitable one. Recorded so the reader that lands does
  not treat "non-empty" as validation.

## 4. Compiler resource limits

### 4.1 Front end: the documented limits hold (probe)

The front end is the one surface whose limits were worth trusting, and they survived probing. Using
the workspace's prebuilt `tessera_codegen.exe` (Release, `vs2026`) against synthetic projects:

| Input | Result | Time |
|---|---|---|
| `(`×400000 + `1` (deep expression) | `TESSERA_JSX_LIMIT_DEPTH` at 1:75 | 27 ms |
| 1 MiB file, exactly at the limit | parses; one byte more is `TESSERA_JSX_LIMIT_FILE_SIZE` | — |
| 40000 sibling `<Text/>` (200k tokens) | `TESSERA_JSX_LIMIT_TOKENS` at 1:350021 | 18 ms |
| 12000 bytes of `\xFF` | `TESSERA_JSX_LEX_INVALID_UTF8`, report capped, no crash | — |

`TESSERA_JSX_LIMIT_*` is real: `Lexer` checks the file size before any scan
(`src/compiler/analyzer/src/lexer.cpp:61`), counts tokens (`lexer.cpp:164,180`), and `Parser::enter`
plus the stored node height cap recursion *and* AST depth (`src/compiler/analyzer/src/parser.cpp:137`,
`:56`, `:148`, `:169`). Diagnostic text is capped at 256 bytes in `DiagnosticSink::report`
(`src/compiler/analyzer/src/diagnostic.cpp:63-79`). This matches `security.md` section 3.1.

The one defect here is cosmetic and is filed as S10/#409: a file over the size limit is refused
correctly by the library (with the right code) but the CLI reports a generic "the file cannot be
parsed" because it tests `root == nullptr` before looking at `parsed.diagnostics`
(`tools/codegen/regenerate.cpp:360-367`).

### 4.2 Stylesheet selectors: unbounded recursion, confirmed process crash

**Finding S1 (high).** `SelectorParser::parse_pseudo` recurses through `parse_selector_list` for
`not`/`is`/`where`/`has` with no depth budget
(`src/ui/style/selector.cpp:357-363`, re-entering at `:405-409`). `parse_selector_list` is what
`prepare_stylesheet` calls for every rule (`src/ui/style/prepared_stylesheet.cpp:109`), and what the
codegen CLI reaches for every side-effect CSS import (`tools/codegen/regenerate_assets.cpp:133`).

Probe (`probe_style.exe`, throwaway, linked against the workspace's `tessera_style.lib`):

| Input | Result |
|---|---|
| `:not(`×200 + `a` + `)`×200 | `ok=0` (a parse error), 23 ms |
| `:not(`×600 | `ok=0`, 1.2 s |
| `:not(`×1200 | **exit `-1073741571` (0xC00000FD, stack overflow)**, 5.8 s |
| `:not(`×20000 | same, 5.8 s |

Reproduced through the shipped CLI, not just the harness. A `page.tsx` containing
`import "./evil.css";` against an `evil.css` of nested `:not(` gave the same `0xC00000FD` after 2.9 s.
The fixture must use a **supported primitive**: an HTML tag such as `<div>` is refused at
`TESSERA_JSX_SUBSET_HTML_TAG` *before* the stylesheet is prepared, so the crash is reachable only from
a project that is otherwise **valid** — which makes it worse, not better, because "write valid TSX"
does not avoid it. `security.md` section 3.1's "never a crash, a hang or a stack overflow" is scoped
to the JSX front end, so this contradicts its intent rather than its letter; the stylesheet engine has
no equivalent invariant and no hostile-input corpus like `tests/unit/compiler_limits_tests.cpp`.

The blob **reader** does have a budget — `kMaxSelectorDepth = 16`
(`src/ui/style/blob_io.hpp:19`, enforced at `stylesheet_blob_read.cpp:84`) — so a compiled stylesheet
is safe to load. The *parser* that produces one is not.

### 4.3 Stylesheet at-rules: unbounded recursion in prepare, confirmed process crash

**Finding S2 (high).** `Preparer::at_rule` recurses into `rules(parse_stylesheet(source.block), …)`
for `@media`, `@container`, `@supports` and `@layer`
(`src/ui/style/prepared_stylesheet.cpp:166,178,192,207`; `layer_at_rule` at `:166`). Nothing caps the
nesting, and `parse_stylesheet` itself returns a *flat* AST (the nested block is kept as an
`AtRule::block` string), so the parser alone survives while prepare recurses.

Probe (`probe_prepare.exe`, throwaway, links `tessera_style.lib` + `tessera_scene.lib`):

| Input | Result |
|---|---|
| 100 nested `@layer x{` | items=101, 7 ms |
| 200 nested `@layer x{` | **0xC00000FD**, 3.2 s |
| 150 nested `@media (min-width:1px){` | items=1, ok |
| 200 nested `@media (min-width:1px){` | **0xC00000FD**, 4.3 s |
| 100000 rules (1.9 MB) | items=100000, 172 ms — wide input is fine |

S1 and S2 are the same class — unbounded recursion in the stylesheet engine, reachable from one
untrusted input (a `.css` the project imports) — but they are separate issues because a depth guard in
`parse_selector_list` does not fix `Preparer::rules`.

### 4.4 The lowerer is quadratic in line length (probe)

**Finding S3 (high).** `jsx::SourceMap::locate` recomputes the column by scanning from the start of
the line for every offset (`src/compiler/analyzer/src/source_map.cpp:26-33`), and
`to_source_span` calls it twice per IR node
(`src/compiler/lowering/src/source_span.cpp:14-19`). Every node on one long line therefore pays
O(line length).

Probe (`probe_stage.exe`, throwaway, links the real `tessera_lowering_core`/`tessera_ir`/
`tessera_ir_ids`/`tessera_react_rules`/`tessera_jsx_frontend`): `<View>` with N self-closing
`<Text/>` children, all on **one** line, then the same content one child per line.

| Nodes | Layout | Bytes | `lower_module` |
|---|---|---|---|
| 45 001 | one line | 1 005 KiB | **28 504 ms** |
| 45 001 | one line | 615 KiB | **15 334 ms** |
| 25 001 | one line | 775 KiB | **12 767 ms** |
| 45 001 | one child per line | 540 KiB | **47 ms** |

Parse is 3-5 ms and `ir::validate` / `assign_identity` are 14/23 ms in every row: the whole cost is
in lowering, and it is the source map. All front-end caps pass (under 1 MiB, under 200k tokens,
depth 2), so no limit fires. This is a build-time denial of service and, on the designer/Wasm path, a
freeze of the preview. A file *at* the size cap on one line is the worst case and needs no cleverness
— one-line output, or a minified bundle, reaches it.

### 4.5 IR validation and codegen have no depth or size cap (latent)

**Finding S4 (low-medium, latent).** `security.md` section 3.2 is titled "Checks the downstream
stages **must add**" and lists `TESSERA_IR_LIMIT_DEPTH`/`_NODES` in `ir::validate`, a decode depth in
`ir_json`, and a byte and identifier cap in codegen. That framing is **honest** — a to-do list, not a
description of the tree — and section 3's summary line ("Resource limits: enforced by the front end
per section 3.1; loop depth is validated in the IR") is accurate. An earlier draft of this review
mis-read the table as a claim; that is retracted.

What is true is that the to-do item has a crash attached. Grep finds none of the five codes, and
`kMaxLoopDepth = 2` (`validate_node.cpp:16`) is the only depth `ir::validate` checks. Probe
(`probe_ir.exe`, throwaway): a hand-built `IrModule` whose `View` chain is N deep.

| Depth | Result |
|---|---|
| 100 | 0 diagnostics |
| 1 000 | 0 diagnostics |
| 5 000 | **0xC00000FD, stack overflow** |

`validate_expr`/`ComponentChecker::walk`/`for_each_component_ref` are recursive with no budget, as are
`Node`'s destructor and the codegen emitters. **Exploitability today is low**: no IR *reader* exists
(`ir_json` is write-only), so IR reaches `validate` only from the lowerer, itself front-end bounded.
The finding is that §3.2's own item has a confirmed crash behind it, so the first `ir_json` reader
inherits an unbounded recursion with no `TESSERA_IR_LIMIT_*` code to report it. Filed as #400.

## 5. IO surfaces

### 5.1 `appfw::Storage`: names are safe from traversal, not from aliasing

`is_valid_entry_name` (`src/appfw/storage_atomic.cpp:56-77`) rejects empty, over-128-byte, `.`, `..`,
any `/`, `\` or `:`, and any `..` substring. Every entry point calls it before building a path
(`src/appfw/storage.cpp:156,195,233,302,338`), and the atomic sequence writes a temp file in the
target's own directory and replaces (`storage_atomic.cpp:79-161`). The traversal question is
**answered negatively by probe**: `../escape`, `../../etc/passwd`, `a/../../b`, `C:\Windows\System32`,
`\\server\share`, `/abs`, `a..b`, `..hidden` all return `valid=0`.

**Finding S5 (medium).** The name is validated for *shape*, not for *identity*. Two distinct legal
names can be the same file:

| Names | `is_valid_entry_name` | What the probe wrote then read |
|---|---|---|
| `trail` then `trail.` | 1 / 1 | writing `trail.` **overwrote** `trail`; read(`trail`) == `SECOND` |
| `Settings` then `settings` | 1 / 1 | same file on a case-insensitive volume |
| `nav.state` then `nav.state ` (trailing space) | 1 / 1 | same file |

and a Windows reserved device name is accepted but does not land where the caller asked: `NUL`
returns `write=1` from `hal::NativeFileSystem`, `exists()` is then `0`, and no file appears. So an
entry can silently alias, or silently vanish, without any `StorageError`. That is a data-integrity
defect rather than a traversal (nothing escapes the root), and `docs/design/services/storage.md`
never claims more than "no absolute/traversal paths", so the fix is a validation rule plus a
documented one — not a vulnerability in the classic sense. Filed because "two names, one file" is
exactly the bug a caller cannot see.

### 5.2 `hal::FileSystem`

`NativeFileSystem` maps `std::error_code`s to the seam's value errors and creates parent directories
on write (`src/hal/filesystem/filesystem.cpp:63-83`). `read` stats first and refuses over `max_bytes`,
then allocates exactly the file size, so a file that grows between the stat and the read is truncated
rather than overflowing. No path validation lives here on purpose: the seam takes a path and its
callers own the policy. Nothing found. **Not probed** for TOCTOU on a symlinked path, and symlink
policy is unstated in the tree.

### 5.3 Network: URL parsing and redirects

`parse_url` (`src/appfw/net_url.cpp:63`) rejects control bytes and spaces, non-http(s) schemes,
missing hosts, non-numeric and out-of-range ports, port 0, and over-long URLs (`kMaxUrlLength`,
8192). `follow_redirect` (`net_redirect.cpp:22-77`) refuses a relative target, refuses `https`→`http`,
computes the origin as (scheme, host, effective port) and drops `Authorization` when it changes.
`net_client_impl.cpp:146-161` re-parses at submit and refuses cleartext unless `allow_cleartext`.

Probe (`probe_net.exe`, throwaway, links `tessera_appfw_net.lib`) — the negatives first, because they
are the results that matter:

| Attempt | Result |
|---|---|
| redirect to `file:///etc/passwd` | refused (`Unsupported`) |
| redirect to `//evil.example/b` (protocol-relative) | refused (`Unsupported`, relative) |
| redirect `https://` → `http://` | refused (`Unsupported`) |
| `https://api.example/x?token=…` in a diagnostic | `redacted_url` drops the query |
| 302 from `api.example` to `other.example` | **followed**, and `Authorization` **dropped** |
| 302 from `api.example` to `API.example:443` | followed, `Authorization` **kept** (same origin) |
| 11th redirect | `too_many_redirects` (`kMaxRedirects = 10`) |

**Finding S6 (medium).** Only `Authorization` is dropped. `Cookie` and `Proxy-Authorization` survive
a cross-origin hop — the probe's second request carried both to `other.example`. The specification
(`docs/specs/networking-v1.md` section 3 item 2) names only `Authorization`, so the code is
spec-compliant and the *spec* is the gap: `Cookie` is at least as much a credential. Filed against
the spec, with the code change as the fix.

**Finding S7 (low).** The authority parser treats `\` as an ordinary character:
`https://good.example\@evil.example/x` parses as userinfo `good.example\` and host `evil.example`,
and `url_string` round-trips it to the same text. The redirect rule then compares `evil.example`
correctly, so a credential is not leaked — but the *first* request's URL displays as
`good.example\@evil.example`, and RFC 3986 forbids `\` in a URI; a back end that normalises it (some
Windows stacks treat `\` as `/`) would send the request to a host the caller did not intend.
`has_forbidden_character` (`net_url.cpp:28`) is the natural place to reject it. Ranked low because no
back end ships, and because the redirect path compares origins structurally.

No back end exists in the tree (`src/appfw/net_*.cpp` contains only `FakeNet` and the client), so
every claim above is about the module's own rules. The WinHTTP/libcurl/`fetch` behaviour that would
carry them is unreviewed.

## 6. Update and crash surfaces

### 6.1 Update manifest: the allowlist and the derivation hold (probe)

`release/channels.json` has `updateHosts: []` and `signing.keys: []`;
`check_release_manifest.py` fails a manifest whose `url`/`notesUrl` is not `https` on an allowlisted
host, whose `channel`/`platform` disagree with its file name, whose digest differs from the ledger, or
which is not byte-identical to its projection (`scripts/release_manifest_rules.py:48-159`).

Probe (Python, against the real modules, with a synthetic two-release ledger and one allowlisted
host):

| Attempt | Result |
|---|---|
| clean projection | no findings |
| manifest names another release's version | `manifests.drift` — not byte-identical to its projection |
| windows manifest carries the linux artifact's digest and size | `manifests.drift` |
| an extra committed manifest nothing projects | `manifests.projection: stale` |
| `url` with `user@host` or a port | refused (`manifests.url`) |
| `url` host not allowlisted | refused (`manifests.url`) |
| `https://updates.example.com//evil` | **accepted** — the host is right and the path is the host's own |

So "can a manifest name an artifact it does not own?" is **disproved**: the committed manifest is
compared to a derivation from the ledger, and the digest comes from the ledger's artifact for that
platform. A forged manifest needs a forged ledger in the same commit, which is a review of a pull
request, not a runtime attack — and the gate cannot see that, which is worth stating.

`plan_update` was probed too:

| Input | Result |
|---|---|
| a signed manifest, `signature_verified=True` | `offer`, `apply_mode=prompt` |
| the same, `signature_verified=False` | `blocked`, `manifests.shape` |
| an **unsigned** manifest with `signature_verified=True` | **`blocked`** — the unsigned case is checked separately from the caller's claim |
| a downgrade to 0.9.0 | `blocked`, `channels.downgrade` |
| `apply_allowed(offer, unsaved_work_active=True)` | `(False, "save or dismiss unsaved work…")` |

That is the honest-but-partial claim in `updates-and-distribution-v1.md` section 6.3, and it survives
probing: `signature_verified` is a caller input, and the module's guarantee is only that forgetting it
refuses. **Not probed**: whether a caller of `plan_update` can pass `True` without a signature —
there is no update client yet.

### 6.2 Crash redaction: the gate can be bypassed, and the planted-marker test does not cover it

**Finding S8 (medium).** `contains_unredacted` `continue`s on any line containing `<home>`
(`src/diagnostics/crash_redact.cpp:122-126`), skipping the home-prefix, environment-name and
path-value checks for that entire line. `redact_report` mirrors it: it applies `redact_paths` to the
whole line, so once the home rule has replaced the prefix the value-level rule can no longer fire.

Probe (`probe_redact.exe`, throwaway, links `tessera_diagnostics.lib`):

| Report line | After `redact_report` | `contains_unredacted` |
|---|---|---|
| `reason=<home> C:\ProgramData\acme\secret\client.cpp` | unchanged | **`0` — would be uploaded** |
| `commit=token=AKIAIOSFODNN7EXAMPLE` | unchanged | `0` — a token rides out |
| `gpu=sk-live-0123456789abcdef` | unchanged | `0` |
| `app=../users/planted-marker/x` | unchanged (relative is not "absolute") | `0`, marker left |
| `reason=<home> and C:\Users\planted-marker\x.cpp` | `… <home>\x.cpp` | `0` (the marker *was* removed) |

`tests/unit/crash_upload_tests.cpp` is the test this claim rests on (`:111-145`): it plants
`C:\Users\planted-marker-9f3\…` on its own line, asserts the marker is gone, asserts
`contains_unredacted(redacted) == false`, and asserts idempotence. Every assertion passes with the
bypass in place, because the fixture never puts `<home>` or a non-home absolute path on the same line
as a second secret, and never uses a token-shaped value. So the test proves "a home path is
redacted"; it does not prove "redaction removes what section 5 lists" (`security.md` section 6 lists
"headers, query params, **file paths**, user data"). The gate's `0` on the first row means
`upload_crash_report` would transmit it: "a report that still carries a user path is refused"
(`crash_upload.hpp` contract 3) is false for a path outside a home directory — on a corporate Windows
image, most paths.

The rest of the crash surface held up: capture writes one fixed name under `<crash_dir>`
(`crash_capture.cpp:31,64`), the POSIX arm opens `O_WRONLY|O_CREAT|O_TRUNC|O_CLOEXEC` mode 0600, the
Windows arm writes a temp file and `MoveFileExW(MOVEFILE_REPLACE_EXISTING)`, the record is bounded at
32 KiB with `truncated=1` rather than a short list (`crash_capture_internal.hpp:39`, `:280-285`), and
the handler allocates nothing. `upload_enabled` is false for a default config and
`upload_crash_report` returns before touching the transport on all three closed gates — the four
zero-call assertions in the test are real. Nothing writes outside the configured directory: the report
path is `<crash_dir>/crash-report.txt` with the directory taken from config and the name a constant,
so no report *content* can influence it.

## 7. Findings

Ranked by severity and exploitability. S1/S2 lead as one *class* — unbounded recursion in the
stylesheet engine — because they are the same defect shape; they are filed separately because a depth
guard in `parse_selector_list` does not fix `Preparer::rules`. Issue numbers are in §8.

| id | Severity | Finding | Evidence | Confirmed by probe |
|---|---|---|---|---|
| S1 | high | Nested `:not(`/`:is(`/`:where(`/`:has(` in a stylesheet overflows the stack in `parse_selector_list`; a 7 KB project CSS import kills `tessera_codegen` | `src/ui/style/selector.cpp:357-363`, `:405`; reaches the CLI through `tools/codegen/regenerate_assets.cpp:133` | yes — 0xC00000FD at depth 1200, and through the CLI |
| S2 | high | Nested `@media`/`@layer`/`@container`/`@supports` overflows the stack in `prepare_stylesheet` | `src/ui/style/prepared_stylesheet.cpp:166,178,192,207` | yes — 0xC00000FD at depth 200 |
| S3 | high | Lowering is O(line length) per node via `SourceMap::locate`; a legal 1 MiB single-line file takes 28 s | `src/compiler/analyzer/src/source_map.cpp:26-33`, `src/compiler/lowering/src/source_span.cpp:14` | yes — 28 504 ms vs 47 ms |
| S5 | medium | `Storage` entry names alias: `trail`/`trail.`, `Settings`/`settings`, `nav.state`/`nav.state ` are one file; `NUL` is accepted and silently writes nowhere | `src/appfw/storage_atomic.cpp:56-77` | yes — overwrite read back, `NUL` `exists=0` |
| S6 | medium | A cross-origin redirect drops `Authorization` but keeps `Cookie` and `Proxy-Authorization` | `src/appfw/net_redirect.cpp:71-75`; spec gap `docs/specs/networking-v1.md` §3 item 2 | yes — both headers reached `other.example` |
| S8 | medium | `contains_unredacted` skips every check on a line containing `<home>`, so a non-home path or a token is uploaded; the planted-marker test passes with the bypass in place | `src/diagnostics/crash_redact.cpp:122-126`; `tests/unit/crash_upload_tests.cpp:111` | yes — `contains_unredacted==0` with an absolute `C:\ProgramData\…` path |
| S4 | low-medium | `ir::validate`, the codegen emitters and `Node`'s destructor recurse with no depth budget; `security.md` §3.2's iterative pre-pass is an accurate to-do item that is not implemented | `src/compiler/ir/src/validate.cpp`, `validate_node.cpp:74`, `validate_expr.cpp:134`; grep finds none of the five codes | yes — 0xC00000FD at depth 5 000 (latent: no IR reader) |
| S7 | low | `\` is legal in a URL authority, so `https://good.example\@evil.example/x` parses as host `evil.example` and displays as `good.example\@evil.example` | `src/appfw/net_url.cpp:28,104-127` | yes — parsed and round-tripped |
| S9 | low | `ui.limits.*` is documented as the front end's configuration, and nothing reads it: the CLI parses with `Limits{}` defaults always | `docs/specs/project-config-v1.md:73-77`, `jsx-subset-v1-jsx.md:325`; `tools/codegen/regenerate.cpp:62`; grep finds no reader | yes — by grep and by reading the CLI |
| S10 | low | A file over the size limit reports a generic "the file cannot be parsed" and loses `TESSERA_JSX_LIMIT_FILE_SIZE`, because the null-root branch is checked before the diagnostics | `tools/codegen/regenerate.cpp:360-367` | yes — library emits the code, CLI drops it |

S4 was drafted as "the doc describes limits that do not exist" and **corrected**: §3.2 is honestly
titled "Checks the downstream stages must add". S4 and S9 are documentation-versus-code gaps; S4 has a
probe, S9 is a grep. S10 is a diagnostic-contract gap, not a security defect, and is included because
it is the one limit a user meets first.

## 8. Filed issues

| Finding | Issue |
|---|---|
| S1 — selector recursion | [#397](https://github.com/thnak/Tessera/issues/397) |
| S2 — at-rule recursion | [#398](https://github.com/thnak/Tessera/issues/398) |
| S3 — quadratic lowering | [#399](https://github.com/thnak/Tessera/issues/399) |
| S4 — IR depth / §3.2 pre-pass | [#400](https://github.com/thnak/Tessera/issues/400) |
| S5 — Storage name identity | [#401](https://github.com/thnak/Tessera/issues/401) |
| S6 — cross-origin credentials | [#402](https://github.com/thnak/Tessera/issues/402) |
| S8 — redaction gate | [#403](https://github.com/thnak/Tessera/issues/403) |
| S7 — backslash in authority | [#404](https://github.com/thnak/Tessera/issues/404) |
| S9 — `ui.limits.*` unread | [#405](https://github.com/thnak/Tessera/issues/405) |
| S10 — file-limit diagnostic | [#409](https://github.com/thnak/Tessera/issues/409) |

Each issue carries the reproduction, the observed versus expected result, the `file:line` it was
traced to, and a severity justification. None of them is fixed here.

## 9. Negative results (worries checked and correctly handled)

Recorded so the next reviewer does not repeat them:

- **Deep JSX/expression/type nesting** — `TESSERA_JSX_LIMIT_DEPTH`, 27 ms, no crash. The
  `enter()`/`finish_node` pair genuinely bounds recursion *and* destruction.
- **Oversized file / token flood / invalid UTF-8** — every one stops at a named limit; the diagnostic
  report stays at `max_diagnostics + 1` entries. (The *size* limit is also enforced correctly at the
  library level; the CLI loses its code — that is S10, a message-quality defect, not a missing bound.)
- **Path traversal through `Storage`** — `../`, `..`, `a/../../b`, drive prefixes, UNC and absolute
  names are all refused; `%2e%2e%2f` is accepted but is never percent-decoded, so it stays a literal
  file name (harmless here, worth remembering if a future slice decodes names).
- **Storage atomicity** — the temp file is created in the target's directory, flushed, then replaced;
  a cancellation or failure removes it. Read, not probed beyond the existing suite.
- **Redirect to another scheme or host** — `file://`, protocol-relative and `https`→`http` are
  refused; a cross-origin hop drops `Authorization` while a same-origin one (default port, different
  host case) keeps it.
- **Update signature** — an unsigned manifest is refused even when the caller claims
  `signature_verified=True`; a downgrade is refused; a less-stable channel move needs consent.
- **Manifest URL allowlist** — credentials, a port, a lookalike host and a non-allowlisted host are
  all refused; a forged manifest is caught by projection drift, so a manifest cannot name an artifact
  it does not own.
- **Crash capture containment** — one fixed file name under the configured directory; mode 0600;
  atomic publish; 32 KiB bound; no allocation in the handler.
- **Uploader opt-in** — a default config, `enabled` without `consent`, `consent` without an endpoint
  and an empty report all send nothing, with the transport's call count asserted.
- **Generated-code escaping** — every user string passes through an escaper; identifiers are
  restricted at the lexer, not escaped later; the `bind::` contract keeps handler bodies in
  developer-owned C++.

## 10. Open questions

- Should the stylesheet engine get a `TESSERA_CSS_LIMIT_*` invariant and a hostile corpus like
  `tests/unit/compiler_limits_tests.cpp`? S1 and S2 are two instances of the same missing rule, and
  the blob reader's `kMaxSelectorDepth = 16` shows the shape a fix would take.
- Is a 1 MiB single-line file worth supporting? If yes, `SourceMap` needs a memoised or
  binary-search column lookup; if no, the file limit should be a *line* limit too.
- `ir_json` is write-only today. The §3.2 depth budget should land with the reader, not after it.
- Does the update policy stay a Python module, or move into the client? Its refusals are good; its
  `signature_verified` input is only as good as its one caller, and there is no caller.
- Should `parse_stylesheet` have a node cap at all, or is the at-rule/selector depth budget enough?

## 11. Notes for the Lead

- `docs/README.md` is off-limits in this lane, so this document is **not** in the docs map yet. The
  gate needs one line under the specs section, in the same style as its neighbours:

  ```markdown
  - [`specs/security-review-v1.md`](specs/security-review-v1.md) — the #198 review: trust
    boundaries, compiler resource limits (two confirmed stack overflows), IO, update/crash findings,
    and what was not reviewed
  ```

  Without it `python -B scripts/gates.py --only docs` fails with
  "docs/specs/security-review-v1.md: not referenced from docs/README.md".
- The ten issues in §8 are open and unassigned; each says "Part of #198" and none is fixed here.
- No `docs/design/services.md` reference: it was split into `docs/design/services/` (the parts used
  here are `storage.md` section 4 and `service-injection.md`), and `ADR-050` is still a single file
  at `decisions/ADR-050-opt-in-app-framework-layer.md` in this checkout — not a directory.
