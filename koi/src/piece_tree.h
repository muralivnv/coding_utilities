#ifndef KOI_PIECE_TREE_H_
#define KOI_PIECE_TREE_H_

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace koi::pt {

// A persistent B+tree of pieces: knows nothing about documents, undo or
// cursors, and every operation produces a *new* tree sharing what it did not
// change. Persistent because undo is then a pointer -- an edit copies only
// its root-to-leaf path (~1.5 KB) and old roots stay valid. Pieces rather
// than bytes in the leaves because a freshly opened file is then a handful of
// entries pointing into a read-only mapping.
//
// THREADING: reference counts are NOT atomic and the node pool is not locked.
// One thread owns a tree and everything reachable from it; make the counts
// atomic before sharing snapshots that get copied or destroyed elsewhere.

using Index = std::ptrdiff_t;

// Small on purpose: node size is paid per keystroke (every edit copies its
// path) while depth grows as log_16, and a descent's scan of `ends_bytes[]`
// at 16 children is two cache lines.
inline constexpr int kBranch = 16;
inline constexpr int kLeafMax = 32;

// One run of bytes from one of the two source buffers. No document position
// or absolute line number: those are what made a flat array O(n) per edit;
// here they are recovered during the descent, which has to happen anyway.
struct Piece {
  std::int64_t src_start{0};
  // `length` and `lf_count` are narrow because a piece is *short*, not because
  // a document is: every piece is born in PiecesFor, which cuts at
  // kSourceChunkBytes, and splitting only ever shortens one. The offset into
  // the source buffer is what has to carry a whole document, and it is 64-bit.
  // Keep kSourceChunkBytes below INT32_MAX and this stays true; the static
  // assert below is where that link is written down.
  std::int32_t length{0};
  std::uint32_t lf_count : 31 {0};
  std::uint32_t from_original : 1 {1};

  Piece() = default;
  Piece(std::int64_t start, std::int32_t len, std::int32_t lfs, bool original)
      : src_start{start},
        length{len},
        lf_count{static_cast<std::uint32_t>(lfs)},
        from_original{original ? 1u : 0u} {}
};
static_assert(sizeof(Piece) == 16, "the piece is meant to be two per cache line quarter");

// The two buffers pieces point into. The tree never owns text.
struct TextSource {
  std::string_view original;
  std::string_view modified;

  std::string_view Of(const Piece& p) const {
    const std::string_view src = p.from_original ? original : modified;
    return src.substr(static_cast<std::size_t>(p.src_start), static_cast<std::size_t>(p.length));
  }
};

struct Node {
  Index bytes{0};        // total bytes in this subtree
  Index lfs{0};          // total newlines in this subtree
  std::uint32_t refs{1};
  std::uint16_t n{0};    // children, or pieces in a leaf
  std::uint8_t height{0};  // 0 is a leaf
};

struct Leaf : Node {
  Piece piece[kLeafMax];
};

// Running totals *through* each child, duplicated here rather than read from
// `child[i]`: the descent then touches two contiguous cache lines instead of
// chasing sixteen pointers, and cumulative sums make the search independent
// comparisons the processor can vectorise instead of a serial add chain.
struct Branch : Node {
  Index ends_bytes[kBranch];
  Index ends_lfs[kBranch];
  Node* child[kBranch];
};

void Retain(Node* n);
void Release(Node* n);

// Owns one reference to a root. Copying is a snapshot: O(1), and the two trees
// then share every node until one of them is edited.
class Tree {
 public:
  Tree() = default;
  explicit Tree(Node* root) : root_{root} {}
  Tree(const Tree& other) : root_{other.root_} { Retain(root_); }
  Tree(Tree&& other) noexcept : root_{std::exchange(other.root_, nullptr)} {}
  Tree& operator=(const Tree& other);
  Tree& operator=(Tree&& other) noexcept;
  ~Tree() { Release(root_); }

  Node* Root() const { return root_; }
  bool Empty() const { return (root_ == nullptr) || (root_->bytes == 0); }
  Index Bytes() const { return (root_ == nullptr) ? 0 : root_->bytes; }
  Index Newlines() const { return (root_ == nullptr) ? 0 : root_->lfs; }
  int Height() const { return (root_ == nullptr) ? 0 : root_->height; }

 private:
  Node* root_{nullptr};
};

// -- construction -----------------------------------------------------------

// Bulk-loads pieces into a balanced tree, bottom up. O(n) and no splitting.
Tree Build(std::span<const Piece> pieces);

// -O3 vectorises this; std::ranges::count compiles to a byte-at-a-time loop
// and a memchr loop pays a call per newline, which in source text is worse.
// Inline here so the tree and the document layer share one definition without
// losing that.
inline std::int32_t CountNewlines(std::string_view s) {
  std::int32_t n = 0;
  for (const char c : s) n += static_cast<std::int32_t>(c == '\n');
  return n;
}

// Cuts `text` into pieces of at most kSourceChunkBytes ('src_start' is where
// `text` begins in its source). Chunking bounds both cold line scans and the
// newline recount when an edit splits a piece.
inline constexpr Index kSourceChunkBytes = 1024;
// This is what keeps Piece's 32-bit `length` and 31-bit `lf_count` honest for a
// document of any size: no piece is longer than a chunk, so neither field ever
// sees a document-sized number. Raising the chunk past these bounds would
// silently truncate instead of failing to build.
static_assert(kSourceChunkBytes <= 0x7fffffff, "a chunk must fit Piece::length");
static_assert(kSourceChunkBytes <= 0x7fffffff, "a chunk's newlines must fit Piece::lf_count");
void PiecesFor(Index src_start, std::string_view text, bool from_original,
               std::vector<Piece>& out);
Tree BuildFromText(std::string_view text, bool from_original);

// -- the two primitives everything else is written in ------------------------
// Split + concatenate rather than in-place insert/delete: same O(log n), but
// deletion never borrows from or merges with a sibling -- concatenation
// restores the invariants in one place for every operation.

// Everything before `at` and everything from `at` on. Splitting inside a piece
// splits the piece, which needs the text to divide the newline count.
void Split(const Tree& in, Index at, const TextSource& src, Tree& left, Tree& right);

Tree Concat(const Tree& left, const Tree& right);

// -- reading ----------------------------------------------------------------

// Walks in document order via an ancestor stack: sibling pointers are
// impossible in a persistent tree (copying a leaf would cascade into copying
// its neighbours, defeating the sharing).
class Cursor {
 public:
  Cursor() = default;
  // Positions on the piece containing `at`; use Valid() to test for the end.
  Cursor(const Tree& tree, Index at);

  bool Valid() const { return depth_ > 0; }
  const Piece& CurrentPiece() const;
  // Document offset of the first byte of the current piece.
  Index PieceStart() const { return piece_start_; }
  // Newlines before the current piece.
  Index PieceLineStart() const { return piece_lfs_; }
  // Advances to the next piece; false at the end of the document.
  bool Next();
  // Steps back to the previous piece; false at the start of the document.
  bool Prev();

 private:
  bool DescendLeftmost(Node* n);
  bool DescendRightmost(Node* n);

  // Sized for the paper bound (nothing enforces minimum fill, so height h can
  // on paper need 2^h pieces), not the measured height of 4; the constructor
  // refuses a taller tree rather than write past the array. Deliberately not
  // value-initialised: zeroing showed up on the grapheme queries, which build
  // a cursor per byte on every motion.
  static constexpr int kMaxDepth = 24;
  Node* stack_[kMaxDepth];
  int slot_[kMaxDepth];
  int depth_{0};
  Index piece_start_{0};
  Index piece_lfs_{0};
};

// The piece containing `at` and the offset it starts at; null outside the
// document. Keeps no ancestor stack, so it is the cheapest way in.
const Piece* FindPiece(const Tree& tree, Index at, Index& piece_start);

// One byte, without the gather a range read performs.
bool ByteAt(const Tree& tree, Index at, const TextSource& src, char& out);

// Appends [from, to) to `out`.
void ReadInto(const Tree& tree, Index from, Index to, const TextSource& src, std::string& out);

// -- line index -------------------------------------------------------------
// Both are O(log n) plus a scan bounded by one piece, which chunking bounds by
// kSourceChunkBytes.

// 0-based line containing `at`.
Index LineAt(const Tree& tree, Index at, const TextSource& src);

// Row and byte-column of an offset in a single descent, with the column
// answered by a short backward scan. Every edit asks for this; LineAt plus
// LineStart instead is the difference between a keystroke costing one
// microsecond and twenty.
void PointAt(const Tree& tree, Index at, const TextSource& src, Index& row, Index& column);

// Byte offset line `line` begins at, clamped into the document.
Index LineStart(const Tree& tree, Index line, const TextSource& src);

// Where the `count`th line after the one containing `from` begins, walking
// forward rather than descending: consecutive-line callers get a memchr from
// the last answer. A `count` of zero (or less) is that same contract read
// literally -- the start of the line `from` is on -- and costs a descent, so
// the walk callers should not ask for it. Returns -1 when it would cross more
// than `max_pieces`; the caller must fall back to the descent. The limit is
// measured, not stylistic: without it, 500 indent passes turned a 3.3 ms
// indent into 14 ms, every line by then starting with hundreds of four-byte
// pieces.
Index AdvanceLines(const Tree& tree, Index from, Index count, const TextSource& src,
                   Index max_pieces);

// -- diagnostics ------------------------------------------------------------

// Checks every invariant: aggregates match their children, all leaves sit at
// the same depth, no node but the root is over-full, references are sane.
// Returns an empty string when the tree is well formed.
std::string_view Validate(const Tree& tree);

// Live nodes, for leak checks in tests.
Index LiveNodeCount();

// What one node costs, averaged over the two kinds. The pool never returns
// memory to the system, so node count times this is what a session holds.
Index ApproxNodeBytes();

}  // namespace koi::pt

#endif  // KOI_PIECE_TREE_H_
