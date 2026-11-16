# Storage module: atomic writes on the I/O executor (AF7, issue #179)

Status: implemented. The normative contract is [ADR-050 section 8, row `storage`](../../decisions/ADR-050/modules-and-tiers.md#8-module-placement-each-module-keeps-or-gets-its-own-spec)
and the module's slice row AF7; the behavioural design record is
[`services/storage.md`](services/storage.md#41-implemented-shape-af7-issue-179) section 4.1. This note is the longer rationale, the failure table and
the test map. Code: `include/tessera/appfw/storage.hpp`, `src/appfw/storage*.cpp`,
`include/tessera/runtime/io_task_executor.hpp`, `src/hal/filesystem/`,
`src/platform/{windows,linux}/filesystem_*`.

## 1. What the module owns, and what it deliberately does not

It owns named project data: **documents** (opaque payloads) and **settings** (an ordered key/value
list in one entry). Both go through the same atomic-replace sequence and the same envelope.

It does not own cache eviction, migrations, streaming or the keystore. Those are later slices of the
same module (ADR-050 section 8 lists them); the header and section 4.1 of [`services/storage.md`](services/storage.md) name them so
a caller does not assume they exist.

## 2. The seam: `IoTaskExecutor`

Layer 0 already had three mechanisms: `WorkerExecutor` (a bounded thread pool that joins its
threads), `IoExecutor` (a bounded in-flight front end over that pool) and `CompletionQueue` (the
queue the UI thread drains). What it lacked was something a module could be *handed* without naming a
thread. AF7 adds that as one small interface with two implementations:

```cpp
struct IoTask { std::function<void()> work; std::function<void()> completion; };

class IoTaskExecutor {
public:
    virtual bool post(IoTask task) = 0;              // never runs `work` inline
    virtual std::size_t capacity() const noexcept = 0;
};
```

`post()` only enqueues. That is the property that makes "never block the UI thread" structural rather
than a review note: a caller on the UI thread *cannot* perform the blocking call, because the body it
hands over does not run on its stack. The completion is delivered through the completion queue, so
the module's callback mutates UI-owned state on the UI thread.

**Why layer 0 and not the module.** ADR-050 section 1 rule 1: layer 0 gains seams, not concepts. An
abstract executor is the same kind of thing as the executors already in `src/runtime`; putting it in
`tessera_appfw_storage` would make every later module depend on storage to get an executor.

Two implementations, no third:

| | `WorkerIoExecutor` (production) | `ManualIoExecutor` (tests) |
|---|---|---|
| thread | one bounded pool (`WorkerExecutor`), joined in the destructor | none |
| completion | posted to the shared `CompletionQueue` | a FIFO the test drains |
| determinism | real scheduling | two FIFOs: `run_one()` runs a body, `run_completions()` delivers |
| at the bound | `post()` returns false and counts a rejection | same |

The manual executor separates the two stages on purpose. `ManualExecutor` holds one FIFO of closures,
so a test cannot tell "the body ran" from "the completion ran"; here the completion only exists after
its body ran, which is the invariant the real executor has and the one a test needs to assert.

## 3. The atomic-write sequence

`appfw::atomic_replace` is the only code path that writes.

| # | Step | What it protects |
|---|---|---|
| 1 | create the parent directory | first run: an absent root is not an error |
| 2 | write the complete payload to `<target>.<generation>.tmp` | the target is never opened for writing, so there is nothing to tear |
| 3 | flush the temp file | the rename cannot make bytes visible that are still in the page cache |
| 4 | replace the target with the temp file | the only step a reader can observe |
| 5 | `Durable` only: flush the directory | on POSIX the new directory entry is the part not yet on disk |

Two invariants carry the guarantee:

- **Same directory.** The temp file is created beside the target, not in a system temp directory. A
  rename across devices is a copy in the kernel, and a copy is not atomic. `temp_path_for` is exposed
  so the rule is asserted rather than assumed.
- **One visible step.** Nothing before step 4 is observable under the target's name; step 4 itself is
  the platform's atomic replace. So every failure mode is "the old content, and a temp file to
  clean up", never "half the new content".

Step 4 is `hal::FileSystem::replace`, not `std::filesystem::rename`. Windows is where the portable
call is wrong twice: `std::rename` fails with `EEXIST` when the target exists, and
`std::filesystem::rename` is not specified to replace at all. The platform halves are
`MoveFileEx(MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)` and `rename(2)`.

### 3.1 Durability, and why `BestEffort` is the default

Both policies flush the temp file; only `Durable` flushes the directory. The window `Durable` closes
is narrow: without it a crash may lose the *last* write, but a reader still sees the complete old
content or the complete new content because the rename is atomic either way. A directory flush per
write is a real cost for a UI-facing settings store, so it is opt-in per `StorageConfig` rather than
the default, and this note is the record of that choice.

| Platform | `Durable` step 5 |
|---|---|
| Windows | covered by `MOVEFILE_WRITE_THROUGH`; `platform_flush_directory` is a successful no-op (Win32 has no supported directory fsync) |
| Linux | `fsync` on an `O_DIRECTORY` descriptor; `ENOTSUP` is reported as `Unsupported` |

## 4. Failure table

Every row is induced in the suite; none is hoped for.

| Failure | Where it is induced | What the caller sees | State on disk |
|---|---|---|---|
| create-directory fails | fake fails `create_directory` | `Io`, step `create-directory` | target unchanged |
| temp write fails | fake fails `write` and leaves half the bytes | `Io`, step `write-temp` | target unchanged, partial temp removed |
| temp flush fails | fake fails `flush` | `Io`, step `flush-temp` | target unchanged, temp removed |
| cancelled mid-sequence | fake cancels the token from inside `flush` | `Cancelled`, step `cancelled` | target unchanged, temp removed |
| replace fails | fake fails `replace` | `Io`, step `replace` | target unchanged, temp removed |
| directory flush fails | fake fails `flush_directory` | `Io`, step `flush-dir` | **new** content visible, durability unconfirmed |
| foreign/truncated/flipped entry | test writes the bytes | `Corrupted` + a diagnostic | file left in place |
| missing entry | read an absent name | `NotFound` | nothing written |

The `flush-dir` row is the one where the new content is visible after a reported failure. That is
correct: the rename already happened, and reporting success would claim a durability the platform
did not provide.

## 5. Corruption fallback

An entry is not raw payload. A 24-byte header precedes it: magic `TSR1`, format version, entry kind
(document or settings), payload length, FNV-1a checksum. Each field has its own diagnostic step:

| Diagnostic step | What it means |
|---|---|
| `header-short` | fewer than 24 bytes: the shape a torn write would leave |
| `header-magic` | not a Tessera entry at all (a foreign file, a rename from another program) |
| `header-kind` | a settings entry read as a document, or the reverse |
| `header-version` | a format version this build does not know |
| `header-length` | the declared length disagrees with the bytes: a truncation |
| `checksum` | the length matches but a byte changed |

Every one returns `Corrupted`, records a bounded diagnostic (`StorageDiagnostics`, ring capacity from
`StorageConfig::diagnostic_capacity`) and **leaves the file alone**. A corrupt read never deletes the
user's data; the caller falls back to its own default, which is what the acceptance criterion asks.

The checksum is a corruption detector, not a security measure: an attacker who can write the file can
forge it. Integrity against tampering belongs to the encryption the restoration snapshot specifies
([`state-restoration-v1.md`](../specs/state-restoration-v1.md) section 3), not to this header.

## 6. Cancellation

Two cases, both covered:

- **Already cancelled at submit.** The operation returns false and nothing is posted, matching
  `runtime::IoExecutor::submit`. No slot is burned to report what the caller already knew.
- **Cancelled while the body runs.** Observed between whole steps. The body removes its temp file and
  returns `Cancelled`; the target keeps its old bytes. `StorageDiagnostics` counts `aborted_writes`
  and `temp_files_removed` separately, because "the write was abandoned" and "no temp file survived"
  are two claims and the suite asserts both.

The store's lifetime is not a cancellation hazard: a work body captures a `shared_ptr` to the
implementation, so destroying the store mid-flight cannot dangle.

## 7. Test map

One executable, `appfw_storage`, four groups:
`tests/unit/appfw_storage_{atomic,cancel,corruption,executor}_tests.cpp` over
`appfw_storage_test_support.hpp`.

| Group | Proves |
|---|---|
| atomic | the step order (as a vector, so a reordering cannot pass), a failure at every step leaves the old content, a torn write, a failed first write, `Durable`'s extra step, distinct temp generations, name sandboxing |
| cancel | cancelled-before-start touches nothing, cancelled-mid-sequence removes the temp, the store's counters, a cancelled read, a cancelled first write |
| corruption | round trip, then magic/length/checksum/short-header/version/kind, a miss vs a corruption, the bounded ring |
| executor | the manual round trip (post does not run, body then completion), the capacity bound, a throwing body, settings round trip, the real file system's replace-over-existing, and one real-thread case proving the production seam |

Determinism: `ManualIoExecutor` and a fake `FileSystem` for everything; no wall clock, no sleep, no
thread except the single seam test. The one real-file test writes under the build tree
(`TESSERA_STORAGE_SCRATCH_DIR`, passed by `src/appfw/CMakeLists.txt`), never `%TEMP%`.

**Sensitivity is measured, not assumed.** Replacing step 4 with a truncate-in-place write makes six
assertions fail (the step-order vector, three old-content-intact checks, the no-target-after-failure
check, and the durable ordering), which is what the suite is for.

## 8. Open questions

- **Migrations.** The version is checked and rejected, not upgraded. A migration runner needs its own
  spec line (which versions are readable, what a failed migration leaves behind).
- **Cache class and quota.** ADR-050 section 8 lists an evictable cache; it is not in this slice and
  needs a policy (LRU by what clock? bounded by bytes or entries?).
- **Keystore.** The `Durability`/`FileSystem` seam has no encryption hook yet. Adding one is a
  `FileSystem` decorator, not a change to `Storage`, so the seam is already the right shape.
- **Streaming.** A write is one bounded buffer (`max_entry_bytes`). Large documents need chunked
  writes with the same atomicity, which means writing chunks to the temp file and renaming once.
