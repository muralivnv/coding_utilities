#ifndef KOI_TEXTOBJECT_H_
#define KOI_TEXTOBJECT_H_

#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "piece_doc.h"
#include "syntax.h"

namespace koi {

struct ObjectRange {
  Index from{0};
  Index to{0};
};

// The ranges of `object` in this buffer, false when there are none to be had.
//
// `error` is cleared on entry and says two different things. False and a reason
// is the usual one: no grammar, no query, a parse that gave up. True and a
// reason is the other: the lookup ran, `out` holds what it found, and the query
// hit its budget or its match limit on the way -- so the answer is short, and a
// caller that reports "no function here" instead of this line is telling the
// user the file has no functions in it.
bool TextObjectRanges(const PieceTable& table, const std::filesystem::path& path,
                      std::string_view object, std::span<const std::string_view> suffixes,
                      std::vector<ObjectRange>& out, std::string& error,
                      Syntax* syntax = nullptr, Interval limit = Interval(0, 0));

}

#endif
