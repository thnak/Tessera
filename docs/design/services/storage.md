# Storage

Part of the [services design set](README.md). Section 4 is the storage target and 4.1 the AF7
module that shipped (`tessera_appfw_storage`); the longer rationale is
[`../storage-module.md`](../storage-module.md).

## 4. Storage

- Three classes: settings (small key/value), documents (app data), cache (evictable).
- Atomic writes (temp + rename); fsync policy documented per platform.
- Schema migrations are versioned and run once; failures do not corrupt data.
- Paths are sandboxed per app; no absolute/traversal paths from untrusted input.
- Large data is streamed; reads/writes happen off the UI thread.
- Encryption/secrets use the platform keystore; never plaintext logs. Keystore availability per
  platform is an open question (see [`status-and-open-questions.md`](status-and-open-questions.md)
  section 10).
- Tests: atomicity, migration, corruption recovery, concurrent access, quota limits.

### 4.1 Implemented shape (AF7, issue #179)

The `storage` module ships as `tessera_appfw_storage` (ADR-050 section 1 layer 1): documents and
settings, every byte through an I/O executor, every write an atomic replace, and a corrupted entry
reported rather than returned. This subsection is the design record for what landed and for the three
decisions the ADR left open.

**Where `IoExecutor` lives: `runtime`, as a seam plus two implementations.** Layer 0 already owned
the *mechanisms* - the bounded `WorkerExecutor` pool, the bounded in-flight `IoExecutor` front end,
and the `CompletionQueue` the UI thread drains - but nothing that a module could be *handed* without
naming a thread. AF7 adds that interface as `runtime::IoTaskExecutor`
(`include/tessera/runtime/io_task_executor.hpp`) plus two implementations: `WorkerIoExecutor` (one
bounded pool, completion posted to the shared queue) for production, and `ManualIoExecutor` (two
hand-drained FIFOs, no thread, no sleep) for tests. It is in layer 0 for ADR-050 section 1 rule 1's
reason: an abstract executor is a seam, and putting it in the module would force every other module
to depend on storage. `post()` never runs the work inline, so a UI-thread caller cannot accidentally
perform the blocking call, and the completion is always delivered through the queue.

**The atomic-write sequence, and why each step is where it is.** `appfw::atomic_replace`
(`src/appfw/storage_atomic.cpp`) is the only code path that writes:

| Step | Call | Why it is that step |
|---|---|---|
| 1 | create the parent directory | an absent root is a first-run condition, not an error |
| 2 | write the complete payload to `<target>.<generation>.tmp` | the target is never opened for writing, so there is nothing to tear |
| 3 | flush the temp file | the rename must not make bytes visible that are still in the page cache |
| 4 | replace the target with the temp file | the only step a reader can observe |
| 5 | `Durable` only: flush the directory | on POSIX the new directory entry is the part not on disk yet |

The temp file is created **in the target's own directory**, because a rename across devices is a copy
and a copy is not atomic. That is the one invariant a caller cannot verify from outside, so it is
asserted (`temp_path_for` is exposed and tested).

Step 4 is a platform primitive, not `std::filesystem::rename`. Windows is the lane where the portable
call is wrong twice over: `std::rename` fails with `EEXIST` when the target exists, and
`std::filesystem::rename` is not specified to replace at all. `hal::FileSystem::replace` is
`MoveFileEx(MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)` on Windows and `rename(2)` on POSIX
(`src/platform/{windows/filesystem_win32,linux/filesystem_posix}.cpp`), and the real-file test runs
the sequence against the actual OS rather than trusting the comment.

**Which `Durability` is the default, and the fsync policy per platform.** This is the deliberate
deviation from the bullet above. Both policies flush the temp file; only `Durable` flushes the
directory, and `BestEffort` is the default. The failure window `Durable` closes is narrow: a *crash*
may lose the last write, but a reader still sees the complete old content or the complete new
content, never a mixture - because the rename is atomic either way. Paying a directory flush per
write for a UI-facing settings store is not a trade this slice makes silently, so it is opt-in and
per-`StorageConfig`.

| Platform | `Durable` step 5 | Note |
|---|---|---|
| Windows | `MoveFileEx` already flushed the rename; `platform_flush_directory` is a successful no-op | Win32 has no supported directory fsync |
| Linux | `fsync` on an `O_DIRECTORY` descriptor (`ENOTSUP` -> `Unsupported`) | without it a power loss can leave the bytes on disk under the old name |

**Corruption fallback.** An entry is not raw payload: a 24-byte header carries a magic, the format
version, the entry *kind* (document vs settings), the payload length and an FNV-1a checksum
(`src/appfw/storage_envelope.cpp`). Each field has its own diagnostic step, so "header-short" (a
torn write), "header-magic" (a foreign file), "header-length" (a truncation) and "checksum" (a bit
flip) are distinguishable. Every one reports `Corrupted`, records a bounded diagnostic, and **leaves
the file in place**: a corrupt read never deletes data behind the user's back. The caller's fallback
is therefore its own default, which is what the acceptance criterion asks for.

**Cancellation.** Every operation takes a `CancellationToken`. An already-cancelled token is refused
at submit (the same rule `runtime::IoExecutor::submit` uses), so nothing is posted. A cancel that
arrives while the body runs is observed between whole steps; the body then removes its temp file and
the target keeps its old content. `StorageDiagnostics` counts `aborted_writes` and
`temp_files_removed` separately, because "the write was abandoned" and "the temp file is gone" are
two claims and the test asserts both.

**Migrations and the cache (issue #411).** `Storage::load_migrated` upgrades older documents with a
pure `MigrationChain` and names a newer version (`FutureVersion`); `CacheStore` is the bounded LRU
cache class. Both are specified in [`storage-module.md`](../storage-module.md) sections 9 and 10.

**What is still not done.** No keystore/encryption (issue #841) and no streaming for large documents
(a write is one bounded buffer, capped by `StorageConfig::max_entry_bytes`; needs an API-shape decision
first). The header names them so a caller does not assume they exist.
