# koi — audit findings, consolidated

2026-08-18. This file replaces the eight per-round audit reports
(audit_findings v1–v6 plus the command/selection and navigate/unicode side
reports) and round8_candidates.md. Everything those files verified has been
FIXED and committed; the per-finding record lives in git history (`git log
--grep "Fix M\|Fix L\|Fix leap\|Fix partition"` walks it). What remains below
is the complete set of KNOWN OPEN items, plus the durable design notes and
flake classes a future audit or fix round should start from.

Closed rounds, for orientation:
- Rounds 1–7 (v1–v4 + side reports): all findings fixed across fix rounds
  ending `2d20a94`.
- Round 8 (candidates surfaced in passing by round 7's fix agents, triaged
  as 9 Medium / 20 Low): M-1..M-6, M-8, M-9 FIXED/CLOSED
  (`32a6d13`, `ff4edf3`, `308e1e0`, `6be6fca`, `306e762`, `a17cde9`); the
  Lows below remain.
- v5 (partitioned_navigation): the feature was DELETED (`be8b309`) in favor
  of leap — every v5 finding is moot.
- v6 (leap): all 12 findings FIXED (`7e1a422`, `75d0eca`).
- Note: severities below are orchestrator triage from the round-8 sweep,
  not adversarially audit-verified; a future round may promote or refute.

## Open — Medium (1)

| # | Area | Finding |
|---|---|---|
| M-7 | gai | `Substitute` runs with no match context (PCRE2's 10M-step default): a catastrophic `-r` pattern burns seconds per line, then throws mid-scan — exit 1 with partial output already flushed. Same class as the fixed Find-side limits (`7b7ae14`), unfixed on the substitution side. |

## Open — Low (21)

| # | Area | Finding |
|---|---|---|
| L-1 | undo/notes | `EditThroughCursors` notes cursors before `Apply`, so a *refused* transaction leaves the note for the next edit — narrow window, refusals are rare. |
| L-2 | injection | syntax.cpp's 25 ms query budget does not bound injected-region *parses* (deliberate non-fix in round 7 #17: tightening it reintroduces half-styled screens); cold whole-file paints can run ~60 ms. |
| L-3 | injection | Unknown-language fences (```mermaid) consume injection count/byte budget without ever painting; a cheap `LayerFor` consult at collection would stop it. |
| L-4 | render | `SelectedIn`/`CursorAt` binary-search on `Selection::To()` monotonicity, which `Normalize` provides but does not document — latent contract gap. |
| L-5 | sqlite | `keylog.cpp Flush()` discards `BEGIN IMMEDIATE`'s result — on failure every event insert auto-commits and the trailing COMMIT fails silently. Append-only, nothing lost. |
| L-6 | sqlite | `project.cpp RecordEdit` = `BumpFile` + `MovePinIn`, two transactions — a gap counts the visit without moving the pin's line. Cosmetic. |
| L-7 | sqlite | `sqlite.h OpenAndCheckDatabase`'s kTooNew/kUnreadable exits do not close the handle; safe only because current callers own it in an RAII object — a future bare-`sqlite3*` caller leaks. |
| L-8 | gai | koi's `navigate.cpp LineMatches`/`PaintQueryMatches` and `query.cpp` predicate `kMatch` still pass `Find` a null error — an erroring pattern silently filters as non-matching. Pathological patterns only. |
| L-9 | gai | gai's fatal exception text goes to stdout, oddly beside the newer stderr diagnostics; changing it changes observable behavior, hence deferred. |
| L-10 | gai | `regex.cpp`'s `msg(256, '.')` buffer idiom vs `DescribeError`'s — two idioms in one file, one artefact dot koi must strip; also relies on `runtime_error(const char*)` truncating at an embedded NUL. |
| L-11 | store paths | keylog writes root-relative paths that nothing reads back yet — the below-root resolution trap (`98a0700`'s class) armed for whenever a reader is added. |
| L-12 | symbols | Scanned symbol names keep a trailing space (a heading node includes its newline; the sanitiser maps it to `' '`) — affects dedup keys and display. |
| L-13 | symbols | `LookUpSymbol` with every staged row refused (all paths carry tabs) opens the picker on empty input with no message. Tab-bearing paths only. |
| L-14 | symbols | `cli.cpp` `hot_paths` dedupe compares raw strings — `./f.cpp` vs `f.cpp` spellings duplicate hot symbols in `--hot-first` output. |
| L-15 | project | `Query`'s `out.reserve(want)` is unbounded for a huge `want` (~56 MB at 10⁶); reachable today only from a test shape. |
| L-16 | gai | `Colorize` re-runs the filters `Process` already ran when `--color` is on — per-line double match cost. |
| L-17 | commands | `VerticalPage` calls `Move` directly and bypasses `DoMove`'s `collapse_insert_caret` rule — the one vertical mover outside the shared rule. |
| L-18 | commands | `ScrollBy` computes its target from the primary cursor only; non-primary cursors can be left far outside the viewport (pre-existing, unchanged by the round-7 #32 fix). |
| L-19 | sidebar | Tab alignment measures from column 0 while rows draw after a 4-column prefix — a tab-bearing name can misalign (never overflows; the round-7 #34 fix bounds width). |
| L-20 | theme | `ParseColor` accepts 8-hex-digit colours and silently drops the alpha byte — the shipped ronin theme sets alpha on its selection colours, so intent is silently altered. |
| L-21 | syntax | syntax.cpp's query exec is deadlined (25 ms) but has NO match limit (silent capture recycling needs a second reporting flag through the Syntax interface), its byte-range casts at :246-248 are unguarded `static_cast<uint32_t>` (koi never checks file_size on open), and `Syntax::Captures` returns true after a deadline-cut query with `TimedOut()` latching across paints — the budgeted-but-silent path is the one a real `mi` keystroke takes. Surfaced by the M-8 sweep (`a17cde9`). |

## Known flake classes (catalogued, not fixed)

- **Clipboard**: the suite is NOT parallel-safe on a machine with
  `wl-copy`/`xclip` on PATH — the system selection is one global object, so
  concurrent runs overwrite each other (~40% of parallel runs fail clipboard
  cases). Single runs also drift ±34 checks with clipboard availability.
  Candidate fix: a `KOI_TEST_NO_CLIPBOARD`-style flag, or serialising those
  cases.
- **Syntax budgets under load**: `syntax: injected regions past the old 512
  cap…` and `syntax: a paint stops handing text to other grammars…` fail
  under sustained CPU contention — fixed parse deadlines meeting a
  descheduled process. Related to (but distinct from) L-2.
- One-shot unreproduced sightings, for pattern-matching if they recur:
  `:from-watched -- a view in another pane keeps up…` (watcher timing, seen
  ~3× under load/mutants), `excerpt revert: an auto-paired keystroke after
  undo…` (12/12 once under IO churn, 0 since; tripwire named
  `ParseExcerptView`'s header check), `background jobs: a save supersedes…`
  (once), `view names never answer for files…` (once).

## Durable design notes (deliberate trades — keep chosen, not forgotten)

- Jump/pin/keylog sqlite runs `PRAGMA synchronous=OFF`: transactions are
  atomic against crashes and other instances (post `6be6fca`), but power
  loss can lose recently committed WAL writes. Durability was traded away
  on purpose.
- `WriteInPlace` (the hard-link save path) is non-atomic by design; a
  concurrent multi-instance save on a hard-linked file can interleave.
  Documented at the function.
- `StampFile` takes mtime and size in two stats; a foreign write between
  them tears the stamp *conservatively* (compares unequal → warns).
- `ExternallyModified` treats "cannot stat at all" as unchanged so `:w`
  works after `rm` — but that also answers "replaced by something
  unreadable" with "fine". Debatable, documented.
- `Document` carries a never-reused identity id (leap liveness); the struct
  is still copyable and a copy would duplicate the id — no current path
  copies one, and the constraint is written at the field, not enforced.
- `LoadDocument` does `static_cast<std::errc>(read_ec.value())` on an
  errno-valued error_code — works on glibc, not guaranteed by the standard.
- Nothing cross-checks config.reference.toml's command table against
  `AllCommands()`; it was hand-verified 1:1 when leap landed and will drift
  silently.
- The scan pool (scan-workers) is grow-only at runtime; lowering the
  setting takes a restart. Stated in EnsureScanWorker and the reference
  config.
- `ed.status` is one slot shared by hints, warnings, and messages; leap now
  carries its own hint (`LeapState::hint`) as a priority workaround. Third
  bug class traced to this sharing — a structural refactor candidate if it
  bites again.
