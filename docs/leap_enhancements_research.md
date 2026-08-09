# Leap enhancements — what flash.nvim and the leap ecosystem suggest for koi

Question: creative ways to grow koi's `leap` (y + two graphemes + labels,
commit 32d5888, audit v6 fixes in flight). Sources: flash.nvim (folke),
leap-spooky.nvim / telepath.nvim (remote actions), leap.nvim's own docs.
Ranked by payoff-for-koi / cost, grounded in machinery koi already has
(labels + wide-glyph-safe rendering, multi-cursor SelectionSet, treesitter
textobjects/symbols, incremental search, jumplist, splits).

## The one-line summary of flash.nvim

Flash's insight is that *labels are a rendering trick, not a mode*: any list
of positions — search matches, f/t candidates, treesitter nodes, another
window's text — can wear labels, and any action — jump, select, yank-at-a-
distance — can consume the picked target. Leap is one cell of that matrix
(2-char pattern × jump). koi now owns the expensive part (label overlay,
match scanning, the state machine); the creative headroom is filling more
cells with it.

## Ranked recommendations

### E1 — Leap into multi-cursor — **IMPLEMENTED** `afe44b3`
After the two characters, one extra key (suggest `A-ret` or `*`) places a
cursor at EVERY current match instead of jumping to one — and a modifier on
a label key (e.g. uppercase label) ADDS a cursor at that match, keeping the
existing ones. koi is a selection-first multi-cursor editor; this turns
"type 2 chars" into the fastest multi-cursor spawner in the editor, scoped
to the viewport, no regex needed. `select_regex` already exists for the
whole-buffer case; this is its visual, viewport, zero-syntax sibling.
Cost: small — matches are already in `LeapState::matches`; build a
SelectionSet from them (MinWidth1 each) instead of collapsing.
This is the one idea here that would make koi's leap *better* than its
inspirations rather than caught up with them.

### E2 — Remote operations, the selection-first way (spooky/telepath's idea)
In vim, "yank a distant word" needs operator machinery (`yr{leap}iw`). In
koi's model it decomposes naturally: leap → act → come back. Cheapest
faithful version: a "boomerang" variant (`Y`?) that records the origin,
jumps, and after the next *editing command* (or on Esc) returns the caret
to the origin — the jumplist already records the origin today; the new bit
is the auto-pop. Combined with E1-style label-with-modifier you get "yank
that word over there without going there," which is spooky.nvim's entire
reason to exist, in ~40 lines. Decide the return trigger carefully (next
edit vs next command vs explicit) — telepath returns after the operator,
which maps to "after next edit" here.

### E3 — Labels on search (`/`) — flash's flagship
koi's `/` already highlights matches incrementally. Add labels to the
visible matches while the prompt is open; pressing a label jumps
immediately (no Enter, no n-cycling). Two design points flash solved:
- labels must be characters that CANNOT continue the pattern — compute the
  set of next-characters actually present after current matches and pick
  labels outside it (flash: "labels are guaranteed not to exist as a
  continuation of the search pattern"). Fall back to hiding labels when the
  alphabet is exhausted; typing always wins.
- this obsoletes nothing: Enter/n keep working for off-screen matches.
Cost: moderate — prompt input routing must split "pattern char" vs "label
char" by the exclusion rule; rendering reuses the leap overlay wholesale.
With this, leap's 2-char mode and `/` stop being rivals: leap is the
fixed-length no-prompt fast path, labeled search is the arbitrary-pattern
path, one label system under both.

### E4 — Treesitter node labels (flash `S`/`R`)
Label the treesitter ancestors of the caret (fn body, block, class, call
args…); one press sets the selection to that node. koi already has
expand_selection — this replaces "press expand N times, overshoot, shrink"
with "read the label on the scope you want." Rainbow/depth-tinted labels
(flash's trick) make nesting legible. The `R` combo (2-char pattern, then
label the *enclosing nodes* of the chosen match) composes E3+E4 for "select
that function over there." Cost: moderate; textobject queries exist, but
node→label→selection wiring and depth styling are new. Note v6/M-8: the
textobject query path needs its budget fix first — do M-8 before this.

### E5 — f/t with labels on repeat (flit's idea, scoped down)
Plain f/t stays exactly as is; but when a count or a repeat (`;`) would be
needed — i.e. the first candidate is not the one — light labels on all
candidates of that character on the line. One press instead of `3;`.
Cost: small; find-char machinery + leap labels.

### E6 — Cross-pane leap
`LeapVisibleRanges` reads the focused pane only. Flash jumps across
windows: labels in every visible pane, a pick focuses that pane and jumps.
koi has real splits and `jump_view_*`; the leap state would need a per-pane
range set and labels drawn in unfocused panes (renderer currently gates the
overlay on `pane.focused`). Medium cost, high wow, and uniquely useful with
excerpt views: leap from the code pane into a grep-results pane by its
text.

### E7 — Polish stolen from flash's config page (cheap, do with the Lows)
- **Continue mode**: `y` then Enter immediately = re-run the previous
  pattern (LeapState already holds `first`; keep the last pair).
- **Autojump-when-unique** already matches flash; keep.
- **Label position option** (`overlay` on first cell today; `after` as an
  option once the v6 Lows land — flash offers both because neither wins
  everywhere).
- **Jump offset**: land *after* the pair for append-style workflows
  (flash's `pos = "end"`), one setting, trivially cheap.
- **Backdrop dim**: flash dims non-match text during the jump. koi's user
  explicitly rejected dim for partition nav ("borders only") — offer as a
  default-off theme scope (`ui.virtual.leap-backdrop`) or skip.

## What NOT to take

- Flash's full extensibility API (custom matcher/labeler/action callbacks)
  — that's a plugin platform's need, not an editor's. koi should add cells
  of the matrix directly (E1–E6), not a Lua-shaped indirection layer.
- Fuzzy pattern mode — fights the "type what you see" contract that makes
  leap low-load; koi has pickers for fuzzy.
- Multi-label 2-char label schemes (label pairs like `aa`/`as`) — only
  needed past ~50 visible targets; koi's paging already covers the tail.

## Suggested order

E7's continue+offset with the v6 Low round (same code, same tests) → E1
(multi-cursor spawn — the differentiator, and it stress-tests LeapState for
E2/E3) → E2 (boomerang) → E3 (labeled search) → E4 after M-8's budget fix →
E5/E6 as appetite allows. R7-style keylog metrics (presses per jump, label
vs autojump ratio, continue usage) would tell you which of E3–E6 earn their
keep.

## Sources

- flash.nvim README (labeled search, char motions, treesitter S/R, remote
  ops, label positioning/offsets, autojump conditions):
  https://github.com/folke/flash.nvim
- leap-spooky.nvim (remote textobjects — "actions at a distance"):
  https://github.com/ggandor/leap-spooky.nvim
- telepath.nvim (leap-engine remote ops, `dr{search}iw`, return semantics):
  https://github.com/rasulomaroff/telepath.nvim
- leap.nvim README (safe labels, beacons, design rationale):
  https://github.com/ggandor/leap.nvim
