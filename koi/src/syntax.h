#ifndef KOI_SYNTAX_H_
#define KOI_SYNTAX_H_

#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "piece_doc.h"

namespace koi {

using CaptureId = std::uint16_t;
inline constexpr CaptureId kNoCapture = 0;

struct Capture {
  Index from{0};
  Index to{0};
  std::string_view name;
};

struct Syntax {
  virtual ~Syntax() = default;

  virtual std::string_view Language() const = 0;

  virtual std::span<const std::string> CaptureNames() const = 0;

  virtual void Sync(const PieceTable& table) = 0;

  virtual void Paint(const PieceTable& table, Interval range, std::vector<CaptureId>& out) = 0;

  virtual bool TimedOut() const = 0;

  // Whether the last paint stopped looking for injected regions before it ran
  // out of them, because the document put more of them in one call than a frame
  // is willing to parse. Everything past that point is painted with the base
  // grammar alone. Reported like TimedOut(): raised by a paint, and lowered by
  // the next parse of a changed buffer.
  virtual bool InjectionsTruncated() const = 0;

  // How many injected-region parses have run since this Syntax was opened.
  // Observable so that a test can say what no timing assertion can: that
  // scrolling over a <script> body reuses its tree instead of re-parsing it.
  virtual Index InjectionParses() const = 0;

  virtual bool Captures(const PieceTable& table, std::span<const std::string_view> query_files,
                        Interval range, std::vector<Capture>& out, std::string& error) = 0;

  virtual bool InLiteralOrComment(Index pos) = 0;
};

std::string_view LanguageForPath(const std::filesystem::path& path);

std::string_view CommentTokenFor(std::string_view language);

std::span<const std::string_view> KnownLanguages();

// The grammar to parse an injected region with, given what the document called
// it -- a fenced block's info string, an html script/style type -- or empty for
// anything that is not a plausible grammar name. Unknown languages are not an
// error: a fence can say anything, and `mermaid` simply has no grammar here.
std::string GrammarFor(std::string_view written);

std::shared_ptr<Syntax> OpenSyntax(const std::filesystem::path& path, std::string& error);

std::shared_ptr<Syntax> OpenSyntaxForLanguage(std::string_view language, std::string& error);

std::vector<std::filesystem::path> RuntimeRoots();

std::filesystem::path FindRuntimeFile(const std::filesystem::path& relative);

}

#endif
