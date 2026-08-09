#include "piece_tree.h"

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

namespace koi::pt {
namespace {

// -- ownership ---------------------------------------------------------------
// Two conventions, not the same one: Split *borrows* its input (never mutates,
// only builds new spines over retained children); Concat *consumes* its inputs
// -- one reference each, transferred in -- which lets it reuse a node whose
// count is already one, the common case for spines Split just produced.
// AsMutable follows Concat: takes a reference, gives one back.

Index g_live_nodes = 0;

Node* g_free_leaves = nullptr;
Node* g_free_branches = nullptr;

// The free list threads through the nodes themselves. memcpy rather than a
// cast because a Node is not a Node* and the aliasing rules are not negotiable.
Node* NextFree(Node* n) {
  Node* next = nullptr;
  std::memcpy(&next, static_cast<const void*>(n), sizeof(next));
  return next;
}
void SetNextFree(Node* n, Node* next) {
  std::memcpy(static_cast<void*>(n), &next, sizeof(next));
}

Leaf* AllocLeaf() {
  Leaf* l = nullptr;
  if (g_free_leaves != nullptr) {
    l = static_cast<Leaf*>(g_free_leaves);
    g_free_leaves = NextFree(g_free_leaves);
  } else {
    l = static_cast<Leaf*>(::operator new(sizeof(Leaf)));
  }
  l->bytes = 0;
  l->lfs = 0;
  l->refs = 1;
  l->n = 0;
  l->height = 0;
  ++g_live_nodes;
  return l;
}

Branch* AllocBranch(std::uint8_t height) {
  Branch* b = nullptr;
  if (g_free_branches != nullptr) {
    b = static_cast<Branch*>(g_free_branches);
    g_free_branches = NextFree(g_free_branches);
  } else {
    b = static_cast<Branch*>(::operator new(sizeof(Branch)));
  }
  b->bytes = 0;
  b->lfs = 0;
  b->refs = 1;
  b->n = 0;
  b->height = height;
  ++g_live_nodes;
  return b;
}

void Recycle(Node* n) {
  --g_live_nodes;
  if (n->height == 0) {
    SetNextFree(n, g_free_leaves);
    g_free_leaves = n;
  } else {
    SetNextFree(n, g_free_branches);
    g_free_branches = n;
  }
}

// -- aggregates --------------------------------------------------------------

void Resum(Leaf* l) {
  Index bytes = 0;
  Index lfs = 0;
  for (int i = 0; i < l->n; ++i) {
    bytes += l->piece[i].length;
    lfs += l->piece[i].lf_count;
  }
  l->bytes = bytes;
  l->lfs = lfs;
}

void Resum(Branch* b) {
  Index bytes = 0;
  Index lfs = 0;
  for (int i = 0; i < b->n; ++i) {
    bytes += b->child[i]->bytes;
    lfs += b->child[i]->lfs;
    b->ends_bytes[i] = bytes;
    b->ends_lfs[i] = lfs;
  }
  b->bytes = bytes;
  b->lfs = lfs;
}

Node* CloneNode(Node* n) {
  if (n->height == 0) {
    Leaf* src = static_cast<Leaf*>(n);
    Leaf* out = AllocLeaf();
    out->n = src->n;
    out->bytes = src->bytes;
    out->lfs = src->lfs;
    std::memcpy(out->piece, src->piece, sizeof(Piece) * static_cast<std::size_t>(src->n));
    return out;
  }
  Branch* src = static_cast<Branch*>(n);
  Branch* out = AllocBranch(src->height);
  out->n = src->n;
  out->bytes = src->bytes;
  out->lfs = src->lfs;
  for (int i = 0; i < src->n; ++i) {
    out->child[i] = src->child[i];
    out->ends_bytes[i] = src->ends_bytes[i];
    out->ends_lfs[i] = src->ends_lfs[i];
    Retain(out->child[i]);
  }
  return out;
}

// Consumes a reference, returns one to a node nobody else can see.
Node* AsMutable(Node* n) {
  if (n->refs == 1) return n;
  Node* copy = CloneNode(n);
  Release(n);
  return copy;
}

// Descends to the child holding `pos`, advancing `base` past the children
// skipped. Returns the last child when `pos` is at or past the end, which is
// what the callers that ask about a boundary position want.
int ChildForByte(const Branch* b, Index pos, Index& base, Index& lf_base) {
  const Index want = pos - base;
  int i = 0;
  while ((i + 1 < b->n) && (b->ends_bytes[i] <= want)) ++i;
  if (i > 0) {
    base += b->ends_bytes[i - 1];
    lf_base += b->ends_lfs[i - 1];
  }
  return i;
}

std::pair<Piece, Piece> SplitPiece(const Piece& p, Index at, const TextSource& src) {
  const std::string_view text = src.Of(p);
  std::int32_t lf_left = 0;
  // Count whichever half is shorter and subtract for the other; runs on
  // every edit.
  if ((at * 2) <= p.length) {
    lf_left = CountNewlines(text.substr(0, static_cast<std::size_t>(at)));
  } else {
    const std::int32_t lf_right = CountNewlines(text.substr(static_cast<std::size_t>(at)));
    lf_left = static_cast<std::int32_t>(p.lf_count) - lf_right;
  }
  const Piece left{p.src_start, static_cast<std::int32_t>(at), lf_left, p.from_original != 0};
  const Piece right{p.src_start + at, static_cast<std::int32_t>(p.length - at),
                    static_cast<std::int32_t>(p.lf_count) - lf_left, p.from_original != 0};
  return {left, right};
}

// -- bulk build --------------------------------------------------------------

Node* BuildLevel(std::vector<Node*>& level) {
  while (level.size() > 1) {
    std::vector<Node*> up;
    const std::size_t groups = (level.size() + kBranch - 1) / kBranch;
    // Spread evenly rather than filling greedily, so the last node of a level
    // is never left holding a single child. Rounding the group size up does
    // NOT do that: ceil(size/groups) is kBranch itself whenever the groups are
    // nearly full, which is greedy filling under another name and leaves the
    // remainder -- one child, when size % kBranch == 1 -- alone in the last
    // node. Hand the remainder out one child at a time instead.
    const std::size_t base = level.size() / groups;
    const std::size_t extra = level.size() % groups;
    const std::uint8_t height = static_cast<std::uint8_t>(level[0]->height + 1);
    std::size_t i = 0;
    for (std::size_t g = 0; g < groups; ++g) {
      const std::size_t stop = i + base + ((g < extra) ? 1 : 0);
      Branch* b = AllocBranch(height);
      b->n = static_cast<std::uint16_t>(stop - i);
      for (std::size_t j = i; j < stop; ++j) b->child[j - i] = level[j];
      Resum(b);
      up.push_back(b);
      i = stop;
    }
    level.swap(up);
  }
  return level.empty() ? nullptr : level[0];
}

// Drops single-child roots left behind by a split.
Node* Shrink(Node* root) {
  while ((root != nullptr) && (root->height > 0) && (root->n == 1)) {
    Branch* b = static_cast<Branch*>(root);
    Node* only = b->child[0];
    Retain(only);
    Release(root);
    root = only;
  }
  if ((root != nullptr) && (root->bytes == 0) && (root->height == 0) && (root->n == 0)) {
    Release(root);
    return nullptr;
  }
  return root;
}

Node* LeafFrom(std::span<const Piece> pieces) {
  Leaf* l = AllocLeaf();
  l->n = static_cast<std::uint16_t>(pieces.size());
  std::memcpy(l->piece, pieces.data(), sizeof(Piece) * pieces.size());
  Resum(l);
  return l;
}

// -- concat ------------------------------------------------------------------

struct Joined {
  Node* a{nullptr};
  Node* b{nullptr};  // null when the join fitted in one node
};

Joined ConcatNodes(Node* l, Node* r);

// Puts `a` (and `b`, if there is one) in place of the last child of `parent`.
// Splits `parent` when that overflows it, which is how a concatenation grows
// the tree by a level.
Joined ReplaceLast(Branch* parent, Node* a, Node* b) {
  Release(parent->child[parent->n - 1]);
  parent->child[parent->n - 1] = a;
  if (b == nullptr) {
    Resum(parent);
    return {parent, nullptr};
  }
  if (parent->n < kBranch) {
    parent->child[parent->n] = b;
    ++parent->n;
    Resum(parent);
    return {parent, nullptr};
  }
  // Full: move the back half into a new sibling, then append.
  Branch* right = AllocBranch(parent->height);
  const int keep = kBranch / 2;
  right->n = static_cast<std::uint16_t>(parent->n - keep);
  for (int i = keep; i < parent->n; ++i) right->child[i - keep] = parent->child[i];
  parent->n = static_cast<std::uint16_t>(keep);
  right->child[right->n] = b;
  ++right->n;
  Resum(parent);
  Resum(right);
  return {parent, right};
}

Joined ReplaceFirst(Branch* parent, Node* a, Node* b) {
  // `a` and `b` replace child 0, in that order.
  Release(parent->child[0]);
  if (b == nullptr) {
    parent->child[0] = a;
    Resum(parent);
    return {parent, nullptr};
  }
  if (parent->n < kBranch) {
    for (int i = parent->n; i > 1; --i) parent->child[i] = parent->child[i - 1];
    parent->child[0] = a;
    parent->child[1] = b;
    ++parent->n;
    Resum(parent);
    return {parent, nullptr};
  }
  Branch* right = AllocBranch(parent->height);
  const int keep = kBranch / 2;
  right->n = static_cast<std::uint16_t>(parent->n - keep);
  for (int i = keep; i < parent->n; ++i) right->child[i - keep] = parent->child[i];
  parent->n = static_cast<std::uint16_t>(keep);
  for (int i = parent->n; i > 1; --i) parent->child[i] = parent->child[i - 1];
  parent->child[0] = a;
  parent->child[1] = b;
  ++parent->n;
  Resum(parent);
  Resum(right);
  return {parent, right};
}

Joined MergeSame(Node* l, Node* r) {
  if (l->height == 0) {
    Leaf* ll = static_cast<Leaf*>(l);
    Leaf* rl = static_cast<Leaf*>(r);
    if ((ll->n + rl->n) <= kLeafMax) {
      Leaf* out = static_cast<Leaf*>(AsMutable(l));
      std::memcpy(out->piece + out->n, rl->piece, sizeof(Piece) * static_cast<std::size_t>(rl->n));
      out->n = static_cast<std::uint16_t>(out->n + rl->n);
      Resum(out);
      Release(r);
      return {out, nullptr};
    }
    return {l, r};
  }
  Branch* lb = static_cast<Branch*>(l);
  Branch* rb = static_cast<Branch*>(r);
  if ((lb->n + rb->n) <= kBranch) {
    Branch* out = static_cast<Branch*>(AsMutable(l));
    for (int i = 0; i < rb->n; ++i) {
      out->child[out->n + i] = rb->child[i];
      Retain(rb->child[i]);
    }
    out->n = static_cast<std::uint16_t>(out->n + rb->n);
    Resum(out);
    Release(r);
    return {out, nullptr};
  }
  return {l, r};
}

Joined ConcatNodes(Node* l, Node* r) {
  if (l->height == r->height) return MergeSame(l, r);
  if (l->height > r->height) {
    Branch* lb = static_cast<Branch*>(AsMutable(l));
    Node* last = lb->child[lb->n - 1];
    Retain(last);  // ConcatNodes consumes it; lb still holds its own reference
    const Joined j = ConcatNodes(last, r);
    return ReplaceLast(lb, j.a, j.b);
  }
  Branch* rb = static_cast<Branch*>(AsMutable(r));
  Node* first = rb->child[0];
  Retain(first);
  const Joined j = ConcatNodes(l, first);
  return ReplaceFirst(rb, j.a, j.b);
}

// -- split -------------------------------------------------------------------

// Borrows `n`. Produces two owned trees; either may be null.
void SplitNode(Node* n, Index at, const TextSource& src, Node*& out_left, Node*& out_right) {
  if (n->height == 0) {
    const Leaf* l = static_cast<const Leaf*>(n);
    Piece left_side[kLeafMax + 1];
    Piece right_side[kLeafMax + 1];
    int nl = 0;
    int nr = 0;
    Index base = 0;
    for (int i = 0; i < l->n; ++i) {
      const Piece& p = l->piece[i];
      const Index start = base;
      const Index stop = base + p.length;
      base = stop;
      if (stop <= at) {
        left_side[nl++] = p;
      } else if (start >= at) {
        right_side[nr++] = p;
      } else {
        const auto [a, b] = SplitPiece(p, at - start, src);
        left_side[nl++] = a;
        right_side[nr++] = b;
      }
    }
    out_left = (nl > 0) ? LeafFrom(std::span{left_side, static_cast<std::size_t>(nl)}) : nullptr;
    out_right = (nr > 0) ? LeafFrom(std::span{right_side, static_cast<std::size_t>(nr)}) : nullptr;
    return;
  }

  const Branch* b = static_cast<const Branch*>(n);
  Index base = 0;
  Index lf_base = 0;
  const int i = ChildForByte(b, at, base, lf_base);

  Node* child_left = nullptr;
  Node* child_right = nullptr;
  SplitNode(b->child[i], at - base, src, child_left, child_right);

  // Everything strictly left of child i. A single child is taken as-is, so a
  // split never manufactures a one-child branch for Shrink to undo.
  Node* left_part = nullptr;
  if (i == 1) {
    left_part = b->child[0];
    Retain(left_part);
  } else if (i > 1) {
    Branch* lb = AllocBranch(b->height);
    lb->n = static_cast<std::uint16_t>(i);
    for (int j = 0; j < i; ++j) {
      lb->child[j] = b->child[j];
      Retain(lb->child[j]);
    }
    Resum(lb);
    left_part = lb;
  }

  const int tail = b->n - (i + 1);
  Node* right_part = nullptr;
  if (tail == 1) {
    right_part = b->child[b->n - 1];
    Retain(right_part);
  } else if (tail > 1) {
    Branch* rb = AllocBranch(b->height);
    rb->n = static_cast<std::uint16_t>(tail);
    for (int j = 0; j < tail; ++j) {
      rb->child[j] = b->child[i + 1 + j];
      Retain(rb->child[j]);
    }
    Resum(rb);
    right_part = rb;
  }

  // Concat consumes, which is what makes these fresh spines free to reuse.
  if (left_part == nullptr) {
    out_left = child_left;
  } else if (child_left == nullptr) {
    out_left = left_part;
  } else {
    const Joined j = ConcatNodes(left_part, child_left);
    out_left = (j.b == nullptr) ? j.a : nullptr;
    if (j.b != nullptr) {
      Branch* root = AllocBranch(static_cast<std::uint8_t>(j.a->height + 1));
      root->n = 2;
      root->child[0] = j.a;
      root->child[1] = j.b;
      Resum(root);
      out_left = root;
    }
  }

  if (right_part == nullptr) {
    out_right = child_right;
  } else if (child_right == nullptr) {
    out_right = right_part;
  } else {
    const Joined j = ConcatNodes(child_right, right_part);
    out_right = j.a;
    if (j.b != nullptr) {
      Branch* root = AllocBranch(static_cast<std::uint8_t>(j.a->height + 1));
      root->n = 2;
      root->child[0] = j.a;
      root->child[1] = j.b;
      Resum(root);
      out_right = root;
    }
  }
}

}  // namespace

// -- reference counting ------------------------------------------------------

void Retain(Node* n) {
  if (n != nullptr) ++n->refs;
}

void Release(Node* n) {
  if (n == nullptr) return;
  if (--n->refs > 0) return;
  if (n->height > 0) {
    Branch* b = static_cast<Branch*>(n);
    for (int i = 0; i < b->n; ++i) Release(b->child[i]);
  }
  Recycle(n);
}

Tree& Tree::operator=(const Tree& other) {
  if (this != &other) {
    Retain(other.root_);
    Release(root_);
    root_ = other.root_;
  }
  return *this;
}

Tree& Tree::operator=(Tree&& other) noexcept {
  if (this != &other) {
    Release(root_);
    root_ = std::exchange(other.root_, nullptr);
  }
  return *this;
}

Index LiveNodeCount() { return g_live_nodes; }

Index ApproxNodeBytes() {
  return static_cast<Index>((sizeof(Leaf) + sizeof(Branch)) / 2);
}

// -- construction ------------------------------------------------------------

Tree Build(std::span<const Piece> pieces) {
  if (pieces.empty()) return Tree{};
  std::vector<Node*> level;
  const std::size_t leaves = (pieces.size() + kLeafMax - 1) / kLeafMax;
  // The same even spread BuildLevel does, and for the same reason: rounding
  // up gives kLeafMax and strands the remainder in a one-piece last leaf.
  const std::size_t base = pieces.size() / leaves;
  const std::size_t extra = pieces.size() % leaves;
  level.reserve(leaves);
  std::size_t i = 0;
  for (std::size_t g = 0; g < leaves; ++g) {
    const std::size_t stop = i + base + ((g < extra) ? 1 : 0);
    level.push_back(LeafFrom(pieces.subspan(i, stop - i)));
    i = stop;
  }
  return Tree{BuildLevel(level)};
}

void PiecesFor(Index src_start, std::string_view text, bool from_original,
               std::vector<Piece>& out) {
  out.clear();
  out.reserve(static_cast<std::size_t>((std::ssize(text) / kSourceChunkBytes) + 1));
  for (Index at = 0; at < std::ssize(text); at += kSourceChunkBytes) {
    const Index len = std::min<Index>(kSourceChunkBytes, std::ssize(text) - at);
    const std::string_view chunk =
        text.substr(static_cast<std::size_t>(at), static_cast<std::size_t>(len));
    out.emplace_back(src_start + at, static_cast<std::int32_t>(len), CountNewlines(chunk),
                     from_original);
  }
}

Tree BuildFromText(std::string_view text, bool from_original) {
  if (text.empty()) return Tree{};
  std::vector<Piece> pieces;
  PiecesFor(0, text, from_original, pieces);
  return Build(pieces);
}

// -- split and concat --------------------------------------------------------

void Split(const Tree& in, Index at, const TextSource& src, Tree& left, Tree& right) {
  // An output may alias the input -- Split(t, at, src, t, tail) is the natural
  // way to write it. Take a reference of our own first: without it, writing
  // `left` releases the root the very next line still has to read, and the
  // whole document goes silently missing.
  const Tree src_tree = in;
  const Index total = src_tree.Bytes();
  if ((src_tree.Root() == nullptr) || (at <= 0)) {
    left = Tree{};
    right = src_tree;
    return;
  }
  if (at >= total) {
    left = src_tree;
    right = Tree{};
    return;
  }
  Node* l = nullptr;
  Node* r = nullptr;
  SplitNode(src_tree.Root(), at, src, l, r);
  left = Tree{Shrink(l)};
  right = Tree{Shrink(r)};
}

Tree Concat(const Tree& left, const Tree& right) {
  if (left.Root() == nullptr) return right;
  if (right.Root() == nullptr) return left;
  Node* l = left.Root();
  Node* r = right.Root();
  Retain(l);
  Retain(r);
  const Joined j = ConcatNodes(l, r);
  if (j.b == nullptr) return Tree{Shrink(j.a)};
  Branch* root = AllocBranch(static_cast<std::uint8_t>(j.a->height + 1));
  root->n = 2;
  root->child[0] = j.a;
  root->child[1] = j.b;
  Resum(root);
  return Tree{root};
}

// -- cursor ------------------------------------------------------------------

bool Cursor::DescendLeftmost(Node* n) {
  while (n->height > 0) {
    Branch* b = static_cast<Branch*>(n);
    stack_[depth_] = n;
    slot_[depth_] = 0;
    ++depth_;
    n = b->child[0];
  }
  Leaf* l = static_cast<Leaf*>(n);
  // Unwound, not just reported: the branches pushed on the way down are still
  // on the stack, and Valid() is `depth_ > 0`, so leaving them there would
  // make an invalid cursor read as valid and CurrentPiece() index a Branch as
  // a Leaf. ValidateNode forbids the empty leaf that gets here; this makes the
  // failure harmless rather than relying on the callers to stop.
  if (l->n == 0) {
    depth_ = 0;
    return false;
  }
  stack_[depth_] = n;
  slot_[depth_] = 0;
  ++depth_;
  return true;
}

Cursor::Cursor(const Tree& tree, Index at) {
  Node* n = tree.Root();
  if ((n == nullptr) || (at < 0) || (at >= tree.Bytes())) return;
  // Checked once, here, not on every push: every other descent walks up
  // before down, so nothing can go deeper than one entry per level. Leaving
  // depth_ zero makes the cursor invalid, which every caller tests for.
  if ((static_cast<int>(n->height) + 1) > kMaxDepth) return;

  Index base = 0;
  Index lf_base = 0;
  while (n->height > 0) {
    Branch* b = static_cast<Branch*>(n);
    const int i = ChildForByte(b, at, base, lf_base);
    stack_[depth_] = n;
    slot_[depth_] = i;
    ++depth_;
    n = b->child[i];
  }
  Leaf* l = static_cast<Leaf*>(n);
  int i = 0;
  while ((i + 1 < l->n) && ((base + l->piece[i].length) <= at)) {
    base += l->piece[i].length;
    lf_base += l->piece[i].lf_count;
    ++i;
  }
  stack_[depth_] = n;
  slot_[depth_] = i;
  ++depth_;
  piece_start_ = base;
  piece_lfs_ = lf_base;
}

const Piece& Cursor::CurrentPiece() const {
  const Leaf* l = static_cast<const Leaf*>(stack_[depth_ - 1]);
  return l->piece[slot_[depth_ - 1]];
}

bool Cursor::Next() {
  if (depth_ == 0) return false;
  const Piece& here = CurrentPiece();
  piece_start_ += here.length;
  piece_lfs_ += here.lf_count;

  Leaf* l = static_cast<Leaf*>(stack_[depth_ - 1]);
  if ((slot_[depth_ - 1] + 1) < l->n) {
    ++slot_[depth_ - 1];
    return true;
  }
  // Walk up to the first ancestor with a sibling to the right, then take its
  // leftmost leaf. This is the whole cost of not having leaf links.
  --depth_;
  while (depth_ > 0) {
    Branch* b = static_cast<Branch*>(stack_[depth_ - 1]);
    if ((slot_[depth_ - 1] + 1) < b->n) {
      ++slot_[depth_ - 1];
      Node* next = b->child[slot_[depth_ - 1]];
      return DescendLeftmost(next);
    }
    --depth_;
  }
  return false;
}

bool Cursor::DescendRightmost(Node* n) {
  while (n->height > 0) {
    Branch* b = static_cast<Branch*>(n);
    stack_[depth_] = n;
    slot_[depth_] = b->n - 1;
    ++depth_;
    n = b->child[b->n - 1];
  }
  Leaf* l = static_cast<Leaf*>(n);
  if (l->n == 0) {  // as in DescendLeftmost: invalid must mean depth_ == 0
    depth_ = 0;
    return false;
  }
  stack_[depth_] = n;
  slot_[depth_] = l->n - 1;
  ++depth_;
  return true;
}

bool Cursor::Prev() {
  if (depth_ == 0) return false;
  if (slot_[depth_ - 1] > 0) {
    --slot_[depth_ - 1];
    const Piece& p = CurrentPiece();
    piece_start_ -= p.length;
    piece_lfs_ -= p.lf_count;
    return true;
  }
  --depth_;
  while (depth_ > 0) {
    Branch* b = static_cast<Branch*>(stack_[depth_ - 1]);
    if (slot_[depth_ - 1] > 0) {
      --slot_[depth_ - 1];
      if (!DescendRightmost(b->child[slot_[depth_ - 1]])) return false;
      const Piece& p = CurrentPiece();
      piece_start_ -= p.length;
      piece_lfs_ -= p.lf_count;
      return true;
    }
    --depth_;
  }
  return false;
}

// -- reading -----------------------------------------------------------------

// Descends to the piece holding `at`, keeping no ancestors; the path every
// grapheme query takes.
const Piece* FindPiece(const Tree& tree, Index at, Index& piece_start) {
  Node* n = tree.Root();
  if ((n == nullptr) || (at < 0) || (at >= tree.Bytes())) return nullptr;
  Index base = 0;
  Index lf_base = 0;
  while (n->height > 0) {
    Branch* b = static_cast<Branch*>(n);
    n = b->child[ChildForByte(b, at, base, lf_base)];
  }
  const Leaf* l = static_cast<const Leaf*>(n);
  int i = 0;
  while ((i + 1 < l->n) && ((base + l->piece[i].length) <= at)) {
    base += l->piece[i].length;
    ++i;
  }
  piece_start = base;
  return &l->piece[i];
}

bool ByteAt(const Tree& tree, Index at, const TextSource& src, char& out) {
  Index start = 0;
  const Piece* p = FindPiece(tree, at, start);
  if (p == nullptr) return false;
  const std::string_view text = src.Of(*p);
  const Index off = at - start;
  if ((off < 0) || (off >= std::ssize(text))) return false;
  out = text[static_cast<std::size_t>(off)];
  return true;
}

void ReadInto(const Tree& tree, Index from, Index to, const TextSource& src, std::string& out) {
  const Index lo = std::max<Index>(0, from);
  const Index hi = std::min<Index>(tree.Bytes(), to);
  if (lo >= hi) return;
  out.reserve(out.size() + static_cast<std::size_t>(hi - lo));
  for (Cursor cur{tree, lo}; cur.Valid(); ) {
    const Piece& p = cur.CurrentPiece();
    const Index start = cur.PieceStart();
    if (start >= hi) break;
    const Index take_lo = std::max(lo, start);
    const Index take_hi = std::min(hi, start + p.length);
    const std::string_view text = src.Of(p);
    out.append(text.substr(static_cast<std::size_t>(take_lo - start),
                           static_cast<std::size_t>(take_hi - take_lo)));
    if (take_hi >= hi) break;
    if (!cur.Next()) break;
  }
}

// -- line index --------------------------------------------------------------

Index LineAt(const Tree& tree, Index at, const TextSource& src) {
  const Index clamped = std::clamp<Index>(at, 0, tree.Bytes());
  if (clamped == 0) return 0;
  const Cursor cur{tree, clamped};
  if (!cur.Valid()) {
    // Past the last byte: every newline in the document precedes it.
    return tree.Newlines();
  }
  const Piece& p = cur.CurrentPiece();
  const std::string_view text = src.Of(p);
  const Index off = clamped - cur.PieceStart();
  return cur.PieceLineStart() + CountNewlines(text.substr(0, static_cast<std::size_t>(off)));
}

void PointAt(const Tree& tree, Index at, const TextSource& src, Index& row, Index& column) {
  const Index clamped = std::clamp<Index>(at, 0, tree.Bytes());
  const Cursor cur{tree, clamped};
  if (!cur.Valid()) {
    // At or past the last byte, so every newline precedes it.
    row = tree.Newlines();
    column = clamped - LineStart(tree, row, src);
    return;
  }
  const Piece& p = cur.CurrentPiece();
  const std::string_view text = src.Of(p);
  const std::string_view prefix = text.substr(0, static_cast<std::size_t>(clamped - cur.PieceStart()));
  row = cur.PieceLineStart() + CountNewlines(prefix);

  const std::size_t back = prefix.rfind('\n');
  if (back != std::string_view::npos) {
    column = static_cast<Index>(prefix.size() - back - 1);
    return;
  }

  // The line began in an earlier piece; walk back rather than descend again.
  // Bounded two ways: by bytes (a file with no newlines would cross it whole)
  // and by pieces (multi-cursor typing shreds a line into one-byte pieces).
  // The piece cap is measured, not stylistic: at 12 cursors a keystroke runs
  // 105us with a cap of 24 vs 38us at 256; at 512 cursors, 1.6ms vs 1.9ms.
  // 256 favours the common case and is within noise of no limit at all.
  constexpr Index kMaxWalkPieces = 256;
  constexpr Index kMaxWalkBytes = 4 * kSourceChunkBytes;

  Index col = static_cast<Index>(prefix.size());
  Cursor walk = cur;
  Index scanned = 0;
  for (Index steps = 0; walk.Prev(); ++steps) {
    const std::string_view t = src.Of(walk.CurrentPiece());
    const std::size_t nl = t.rfind('\n');
    if (nl != std::string_view::npos) {
      column = col + static_cast<Index>(t.size() - nl - 1);
      return;
    }
    col += static_cast<Index>(t.size());
    scanned += static_cast<Index>(t.size());
    if ((steps >= kMaxWalkPieces) || (scanned > kMaxWalkBytes)) {
      column = clamped - LineStart(tree, row, src);
      return;
    }
  }
  column = col;  // walked back to the start of the document
}

Index LineStart(const Tree& tree, Index line, const TextSource& src) {
  if (line <= 0) return 0;
  if (line > tree.Newlines()) return tree.Bytes();

  Node* n = tree.Root();
  if (n == nullptr) return 0;
  Index base = 0;
  Index lf_base = 0;
  while (n->height > 0) {
    Branch* b = static_cast<Branch*>(n);
    const Index want = line - lf_base;
    int i = 0;
    while ((i + 1 < b->n) && (b->ends_lfs[i] < want)) ++i;
    if (i > 0) {
      base += b->ends_bytes[i - 1];
      lf_base += b->ends_lfs[i - 1];
    }
    n = b->child[i];
  }
  const Leaf* l = static_cast<const Leaf*>(n);
  int i = 0;
  while ((i + 1 < l->n) && ((lf_base + l->piece[i].lf_count) < line)) {
    base += l->piece[i].length;
    lf_base += l->piece[i].lf_count;
    ++i;
  }
  // The line begins just past the (line - lf_base)'th newline in this piece.
  // memchr, not a byte loop: this is the scan the chunk size exists to bound,
  // on every vertical motion and rendered line, and memchr moves a vector at
  // a time.
  const std::string_view text = src.Of(l->piece[i]);
  Index want = line - lf_base;
  const char* p = text.data();
  const char* end = text.data() + text.size();
  while (p < end) {
    const char* hit = static_cast<const char*>(std::memchr(p, '\n', static_cast<std::size_t>(end - p)));
    if (hit == nullptr) break;
    if (--want == 0) return base + static_cast<Index>(hit - text.data()) + 1;
    p = hit + 1;
  }
  return tree.Bytes();
}

Index AdvanceLines(const Tree& tree, Index from, Index count, const TextSource& src,
                   Index max_pieces) {
  // Zero lines after the one containing `from` is the start of *that* line,
  // which is only `from` itself when `from` already sits on a line start.
  // Answering `from` instead was a silent wrong answer for any caller that did
  // not special-case zero away; a descent is the honest answer, and no hot
  // path asks for zero.
  if (count <= 0) return LineStart(tree, LineAt(tree, from, src), src);
  Index remaining = count;
  Index crossed = 0;
  bool first = true;
  for (Cursor cur{tree, from}; cur.Valid(); first = false) {
    if (++crossed > max_pieces) return -1;
    const Piece& p = cur.CurrentPiece();
    const Index off = first ? (from - cur.PieceStart()) : 0;
    // A piece without enough newlines is skipped on its count alone; only
    // the first piece is read, since only there does the walk start mid-way.
    if (!first && (static_cast<Index>(p.lf_count) < remaining)) {
      remaining -= static_cast<Index>(p.lf_count);
      if (!cur.Next()) break;
      continue;
    }
    const std::string_view text = src.Of(p);
    const char* base = text.data();
    const char* q = base + std::clamp<Index>(off, 0, std::ssize(text));
    const char* end = base + text.size();
    while (q < end) {
      const char* hit =
          static_cast<const char*>(std::memchr(q, '\n', static_cast<std::size_t>(end - q)));
      if (hit == nullptr) break;
      if (--remaining == 0) return cur.PieceStart() + static_cast<Index>(hit - base) + 1;
      q = hit + 1;
    }
    if (!cur.Next()) break;
  }
  return tree.Bytes();
}

// -- validation --------------------------------------------------------------

namespace {

std::string_view ValidateNode(const Node* n, int expect_height) {
  if (n->height != expect_height) return "a leaf is at the wrong depth";
  if (n->refs == 0) return "a live node has no references";
  if (n->height == 0) {
    const Leaf* l = static_cast<const Leaf*>(n);
    if (l->n > kLeafMax) return "an over-full leaf";
    Index bytes = 0;
    Index lfs = 0;
    for (int i = 0; i < l->n; ++i) {
      if (l->piece[i].length <= 0) return "an empty piece";
      bytes += l->piece[i].length;
      lfs += l->piece[i].lf_count;
    }
    if (bytes != l->bytes) return "a leaf's byte count is wrong";
    if (lfs != l->lfs) return "a leaf's newline count is wrong";
    return {};
  }
  const Branch* b = static_cast<const Branch*>(n);
  if (b->n > kBranch) return "an over-full branch";
  if (b->n < 1) return "an empty branch";
  Index bytes = 0;
  Index lfs = 0;
  for (int i = 0; i < b->n; ++i) {
    if (const std::string_view err = ValidateNode(b->child[i], expect_height - 1); !err.empty()) {
      return err;
    }
    if ((b->child[i]->height == 0) && (static_cast<const Leaf*>(b->child[i])->n < 1)) {
      return "an empty leaf below a branch";
    }
    bytes += b->child[i]->bytes;
    lfs += b->child[i]->lfs;
    if (b->ends_bytes[i] != bytes) return "a cached running byte total is stale";
    if (b->ends_lfs[i] != lfs) return "a cached running newline total is stale";
  }
  if (bytes != b->bytes) return "a branch's byte count is wrong";
  if (lfs != b->lfs) return "a branch's newline count is wrong";
  return {};
}

}  // namespace

std::string_view Validate(const Tree& tree) {
  if (tree.Root() == nullptr) return {};
  return ValidateNode(tree.Root(), tree.Root()->height);
}

}  // namespace koi::pt
