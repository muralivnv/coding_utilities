#ifndef KOI_SIDEBAR_H_
#define KOI_SIDEBAR_H_

#include <string>
#include <string_view>
#include <vector>

namespace koi {

struct ProjectStore;

// `text` cut to fit `width` terminal columns, with U+2026 standing for what was
// dropped. The result is never wider than `width` -- that is the whole point of
// it, and the sidebar's column arithmetic depends on it -- except that a width
// of zero yields the empty string and every width of one or more can still cost
// the one column the ellipsis itself takes.
//
// Width is positional: a tab is measured against the column it lands on, from
// column 0. A caller drawing the result at some other column will see a
// tab-bearing result land differently, but never wider.
std::string TruncateToWidth(std::string_view text, int width);

std::vector<std::string> SidebarLines(ProjectStore& store, int columns);

int RunSidebar();

}

#endif
