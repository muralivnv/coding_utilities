#ifndef KOI_SYMBOLS_H_
#define KOI_SYMBOLS_H_

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <generator>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "piece_doc.h"

namespace koi {

struct Symbol {
  std::string path;
  Index line{1};
  Index column{1};
  std::string name;
};

enum class SymbolKind : std::uint8_t {
  kDefinitions,
  kReferences,
  kBoth,
};

// `error` accumulates rather than being replaced: a scan records at most one
// complaint, and only into an empty string, so a caller threading one string
// through several scans keeps the first thing any of them had to say. A scan
// with nothing to report leaves the string untouched -- it never erases an
// earlier scan's diagnostic. A caller that wants a per-scan verdict clears the
// string itself before each call.
std::generator<Symbol> ScanSymbols(std::span<const std::string> paths, SymbolKind kind,
                                   std::string& error, std::string_view containing_word = {},
                                   const std::atomic<bool>* cancel = nullptr);

std::generator<Symbol> ScanSymbols(std::filesystem::path path, SymbolKind kind,
                                   std::string& error);

std::vector<Symbol> CollectSymbols(std::span<const std::string> paths, SymbolKind kind,
                                   std::string& error, std::string_view containing_word = {});

bool ContainsWord(std::string_view haystack, std::string_view needle);

std::string FormatSymbolRow(const Symbol& symbol);

// True when `row` names something to open: either it carries a `:<line>` field,
// or it is a bare path to a file that exists from here. False for the rest of
// what a picker can print -- a banner, an echo of the query, a "no matches"
// line -- which is not a target however much it looks like one. `out` is left
// alone unless the answer is true, and `row` may view into it.
bool ParseSymbolRow(std::string_view row, Symbol& out);

inline constexpr size_t kHidePad = 300;

// A picker row: the display, padding wide enough to push the payload off the
// screen, a tab, then the payload. Empty when the row cannot be built without
// lying about what it says -- a tab or a newline in the payload, a newline in
// the display -- because RowPayload cannot get those back. Callers skip an
// empty row rather than writing a blank line into a picker.
std::string PickerRow(std::string_view display, std::string_view payload);

std::string_view RowPayload(std::string_view row);

std::string SymbolPickerRow(const Symbol& symbol);

std::vector<std::string_view> IndexableLanguages();

}

#endif
