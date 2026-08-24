#ifndef KOI_SEARCH_H_
#define KOI_SEARCH_H_

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "piece_doc.h"

namespace koi {

// The one-line reason inside gai's multi-line compile complaint; see the note
// on the format at the definition.
std::string OneLineReason(std::string_view what);

bool FindInText(std::string_view pattern, std::string_view text, std::vector<Interval>& out,
                std::string& error);

bool FindInDocument(const PieceTable& table, std::string_view pattern, Interval range,
                    std::vector<Interval>& out, std::string& error);

bool FindFirstInDocument(const PieceTable& table, std::string_view pattern, Index from,
                         std::optional<Interval>& out, std::string& error);

}

#endif
