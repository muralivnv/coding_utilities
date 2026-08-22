#ifndef KOI_ANCHOR_H_
#define KOI_ANCHOR_H_

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "piece_doc.h"
#include "project.h"

namespace koi {

struct Document;
struct Editor;

// Healing: taking a stored location back to the line it names after the file
// under it has moved. docs/smart-jump.md, "Self-healing anchors" -- the line is
// a cache, `(file, symbol, content)` is what the row *is*, and nothing here may
// delete a row for failing to find it.
//
// Everything above the job at the bottom of this header is a free function over
// plain data, so the ladder can be tested without an Editor, a store or a file.

// -- what a line is -----------------------------------------------------------

// Whitespace off both ends, and no more than this many bytes of what is left.
// The trim is what makes a line comparable across a reindent; the cap is
// kMaxSymbolNameBytes' reasoning applied to a line of source, cut on a
// code-point boundary so a clipped line is still UTF-8.
inline constexpr std::size_t kMaxContentBytes = 200;

// One definition, shared by the recorder and the healer. A row is matched by
// comparing the text a record stored against the text a heal reads back, so two
// spellings of "the same line" is the one divergence that cannot be tolerated:
// it would not fail loudly, it would make every row unhealable.
std::string_view TrimAnchorLine(std::string_view raw);
std::string NormalizeAnchorLine(std::string_view raw);

// FNV-1a over a normalised line. A hint, not an identity: a collision makes a
// line look less unique than it is, and the ladder's answer to "not unique" is
// to be more careful.
std::uint64_t AnchorLineHash(std::string_view normalized);

// A file, hashed once. Every rung below rung 2 is a lookup against this.
struct AnchorFile {
  // One entry per document line, normalised. Sized like LineCount(): a file
  // ending in a newline has a final empty line, so an index here is a document
  // line number minus one whichever way the file ends.
  std::vector<std::string> lines;
  std::vector<std::uint64_t> hashes;
  // (hash, line) sorted by hash then line, so "where else does this text
  // appear" is an equal_range rather than a scan per row.
  std::vector<std::pair<std::uint64_t, Index>> index;
};

void SplitAnchorLines(std::string_view text, AnchorFile& out);

// The lines on either side of `line0`, in the shape ContextAround stores them:
// two above and two below, the anchor line itself left out, blank lines kept in
// place, newline separated. Always four entries -- a slot off either end of the
// file is an empty one -- because the compare is positional and a context that
// is short at the top would line up against the wrong neighbours.
std::string AnchorContextAt(const AnchorFile& file, Index line0);

// -- patience diff over line hashes -------------------------------------------

// One region the two texts disagree about, in half-open 0-based line indices.
// A pure insertion has `old_from == old_to`, a pure deletion `new_from ==
// new_to`. Everything outside a hunk corresponds line for line.
struct DiffHunk {
  Index old_from{0};
  Index old_to{0};
  Index new_from{0};
  Index new_to{0};
};

// Patience diff: unique-common lines, longest increasing subsequence over them,
// recurse between the anchors that survive. Bram Cohen's, and the reason it is
// this and not LCS is that it does not match a stray `}` in one function to a
// `}` in another -- which for anchor healing is the whole failure mode.
std::vector<DiffHunk> DiffLineMap(std::span<const std::uint64_t> old_lines,
                                  std::span<const std::uint64_t> new_lines);

struct MappedLine {
  Index line{0};
  // False when the line fell inside a changed hunk: `line` is then the hunk's
  // start in the new text, which is where the content rungs should start
  // looking, not an answer.
  bool exact{false};
};

MappedLine MapLineThroughDiff(std::span<const DiffHunk> hunks, Index line);

// -- banded edit distance ------------------------------------------------------

// Levenshtein distance, never reported above `cap`.
//
// Myers 1999 bit-parallel in one 64-bit word for strings of at most 64 bytes,
// which is O(len) with a handful of instructions per character. Longer strings
// take diff-match-patch's Match_MaxBits route: the first 64 bytes are measured
// exactly and the tails are charged the cheap upper bound -- mismatched bytes
// over the shared length, plus the length difference. That over-reports a tail
// edit that shifts the rest along, which is the safe direction: rung 7 accepts
// on a *low* score.
int EditErrors(std::string_view a, std::string_view b, int cap);

// -- the ladder -----------------------------------------------------------------

// The enclosing symbol's line span in the new file, 0-based and inclusive.
// Empty (to < from) means "not known", and the rungs that want one stand down.
struct SymbolSpan {
  Index from{0};
  Index to{-1};
  bool Empty() const { return to < from; }
};

// Everything the ladder is allowed to look at, viewed rather than copied: this
// is built once per row over a file's worth of them, and the strings it points
// at are the store rows the caller is already holding. They have to outlive the
// call.
struct HealInput {
  const AnchorFile* file{nullptr};
  // The row's cached line, 1-based, as the store holds it.
  Index line{1};
  // The row's stored text. An empty `content` is a row recorded without the
  // buffer: nothing below rung 2 can say anything about it.
  std::string_view content;
  std::string_view context;
  // Whether `line` is an open buffer's live position rather than the one the
  // store holds. It matters to rung 2, which maps *stored* coordinates through
  // the hunks: a mapped position it cannot verify is a better seed than a
  // stored line the file no longer has, but for a live line the shift has
  // already been applied once and taking it again would double-shift.
  bool live_line{false};
  // Rung 2's input, or null when there is no old text to diff against -- no
  // blob, no repository, no git, the object not in the odb. All four are the
  // same shape here, which is the no-git contract.
  const std::vector<DiffHunk>* hunks{nullptr};
  // Rungs 5 and 7. Empty is supported: rung 5 stands down and rung 7 searches
  // the +/-50 window instead.
  SymbolSpan symbol;
};

// Which rung answered. The numbers are the design's, and they are reported
// rather than derived so a test can say *how* a row healed, not merely that it
// did. Rung 1 -- the blob gate -- is decided by the caller before the ladder is
// reached, since it is a fact about the file rather than about a row.
enum class HealRung : std::uint8_t {
  kBlob = 1,
  kDiff = 2,
  kAtLine = 3,
  kNearby = 4,
  kInSymbol = 5,
  kAnywhere = 6,
  kFuzzy = 7,
  kMiss = 8,
};

struct HealResult {
  // 1-based, and 0 on a miss.
  Index line{0};
  int rung{static_cast<int>(HealRung::kMiss)};
  // 1.0 for every rung that matched the text exactly, 1 - errors/len for rung 7.
  double similarity{0};
  // Whether the text this landed on appears exactly once in the file. Counted
  // live, off the file the ladder is holding -- the row's recorded occurrence
  // count is not consulted, because a number measured against a file that has
  // since changed is worth less than one measured against the truth. The
  // write-back rule needs it: stored content is refreshed only on a match that
  // was both close and unambiguous.
  bool unique{false};
  bool miss{true};
};

// The whole ladder, over inputs it is handed. No file is read here, no
// subprocess is run and nothing is parsed: the caller does that once for the
// file and calls this once per row.
HealResult ResolveAnchor(const HealInput& in);

// How far rungs 4 and 7 look either side of the seed line.
inline constexpr Index kAnchorSearchWindow = 50;

// Rung 7 accepts at or below this. diff-match-patch's match_main threshold, and
// its score shape: errors/len + |dline| / kAnchorLineScale.
inline constexpr double kFuzzyAccept = 0.5;
inline constexpr double kAnchorLineScale = 32.0;

// Stored content is rewritten only at or above this similarity, and only on a
// unique match. Below it a run of small plausible repairs walks the anchor onto
// a line nobody ever visited -- `if (foo)` -> `if (foo && bar)` -> `if (bar)`.
inline constexpr double kRefreshSimilarity = 0.9;

// -- the heal job ----------------------------------------------------------------

// One file's worth of healing: read once, hashed once, at most one subprocess,
// at most one parse. Nothing in here repeats per row.
struct AnchorJob {
  // -- input, filled on the main thread -------------------------------------
  // The key the rows are stored under, and a path valid from here.
  std::string key;
  std::filesystem::path path;
  // The repository to ask for old blobs, or empty when there is none -- which
  // is the whole of rung 2's precondition, and is why a project without git
  // forks nothing at all rather than forking and being told no.
  std::filesystem::path git_root;
  // The bytes to heal against. The save trigger has them in hand already; the
  // open and focus-in triggers leave this empty and the worker reads the file.
  std::string text;
  bool have_text{false};
  // The open document these rows shadow, and the revision `text` was taken at,
  // so the apply can put the healed lines into the shadow at the right point in
  // the journal and catch them up from there. -1 for a file nothing has open.
  Index doc_id{-1};
  Index doc_revision{-1};
  std::vector<AnchorRow> rows;

  // -- output, written by the worker ----------------------------------------
  std::atomic<bool> done{false};
  std::vector<AnchorHeal> heals;
  // Rung 1 answered for every row: the file is byte for byte what they were
  // recorded against, so nothing is written at all.
  bool blob_gate{false};
  // Whether `git cat-file` was actually run. Observable so a test can assert
  // that a project with no repository heals by content and shells out to
  // nothing -- which is a property of this code, not of $PATH.
  bool ran_git{false};
  // Whether the file was parsed, which is what refills a null `symbol`.
  bool parsed{false};
  // The file is past kMaxUniqBytes (editor.h): read and hashed for the blob
  // gate, and then left alone rather than split line by line on a pool thread.
  bool too_big{false};
  // Rungs the job used, one bit per rung, for tests and for a report.
  std::uint32_t rungs{0};
};

// The worker-thread body. Pure with respect to the editor: it reads a file, may
// run one `git cat-file`, may parse once, and writes its answers into the job.
void RunAnchorJob(AnchorJob& job);

// -- live shifting ------------------------------------------------------------

// AnchorShadow itself is on Document (editor.h), because it belongs to the
// buffer and dies with it. These are what move it.
//
// Replays the journal onto the shadow. An edit entirely above an anchor shifts
// it by the edit's row delta; one entirely below leaves it; one whose span
// touches the anchor's own line marks it dirty. Falling behind `journal_base`
// marks every row dirty -- the shifts cannot be reconstructed, and guessing is
// how anchors land on neighbours.
void SyncAnchorShadow(Document& doc);

// Brings the shadow level with the store: rows it has never seen are adopted at
// the store's line, and a row the recorder has touched since (its `seq` moved)
// is taken from the store again. Rows the shadow already tracks keep the line
// it has shifted them to.
void AdoptAnchorRows(Editor& ed, Document& doc);

// The live line of `id` in this document, or false when the document does not
// track it or an edit has landed on it. Callers fall back to the stored line.
bool AnchorShadowLine(Editor& ed, Document& doc, std::int64_t id, Index& line);

// -- triggers -------------------------------------------------------------------

// Queues a heal for `path`, or does nothing when there is no store, no rows for
// the file, or a job for it already in flight. `text` is the buffer's bytes for
// the save trigger and empty for the others, which read the file in the worker.
void StartAnchorHeal(Editor& ed, const std::filesystem::path& path, std::string text,
                     bool have_text);

// Applies every finished job, on the main thread, one IMMEDIATE transaction
// each. True while any job is still running, which is what keeps the main
// loop's poll alive.
bool PumpAnchorHeals(Editor& ed);

}

#endif
