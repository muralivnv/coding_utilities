// Tests for overview.cpp: the overview, and what it says about the sections it
// could not finish.
//
// Declared in tests.h, run from main.cpp.

#include "tests.h"

namespace koi {

void OverviewMarksTheSectionsItCouldNotFinish() {
  TEST_CASE("overview: a file whose query ran out of budget comes back marked");

  // The same unbounded exec, on the other side of the program: `--overview` is
  // what a script shells out to, and a query with no deadline blocks it for as
  // long as the file wants. Bounded is only half of it -- a section that
  // silently stopped early is a summary of a file that does not exist -- so the
  // stop is marked in the output, on a key of its own.
  //
  // Not in `error`, and so not in the exit code: a file that came back short is
  // not a run that failed, which is the symbol scan's contract too.
  const FakeQueryDir queries{"cpp"};
  EXPECT_TRUE(queries.Ready());
  if (!queries.Ready()) return;

  const Scratch scratch{"koi-overview-budget"};

  // Measured with the budget lifted this costs about ten seconds against the
  // two it is allowed. One function holding every call, so that what is being
  // timed is the query and not the O(calls x functions) walk that attributes
  // them afterwards.
  queries.Write("functions.scm", PilingUpQuery(128, "function.name", "function.def"));
  const std::string runaway = WideCalls(24, 400, false);
  const std::filesystem::path runaway_file = scratch.Write("runaway.cpp", runaway);

  OnAThreadOfItsOwn([&] {
    const std::vector<std::string> paths{runaway_file.string()};
    std::string out;
    std::string error;
    bool ok = false;
    const long long ms = MillisecondsOf([&] { ok = OverviewOf(paths, {}, out, error); });
    EXPECT_TRUE(ok);
    EXPECT_EQ(error, std::string{});
    EXPECT_TRUE(ms < 8000);
    EXPECT_TRUE(out.starts_with("file: "));
    EXPECT_TRUE(out.find("\npartial: runaway.cpp exceeded the 2000ms budget -- some of its "
                         "contents are missing\n") != std::string::npos);
    // Partial, not empty: what the query produced before the deadline is kept
    // and printed under the marker.
    EXPECT_TRUE(out.find("functions:\n") != std::string::npos);
  });

  // The match limit, marked the same way and under the same key.
  queries.Write("functions.scm", PilingUpQuery(8, "function.name", "function.def"));
  const std::string wide = WideCalls(300, 1, false);
  const std::filesystem::path wide_file = scratch.Write("wide.cpp", wide);

  OnAThreadOfItsOwn([&] {
    const std::vector<std::string> paths{wide_file.string()};
    std::string out;
    std::string error;
    EXPECT_TRUE(OverviewOf(paths, {}, out, error));
    EXPECT_EQ(error, std::string{});
    EXPECT_TRUE(out.find("\npartial: wide.cpp: too many query matches -- some of its contents "
                         "are missing\n") != std::string::npos);
  });

  // And an ordinary file with the queries koi ships: no marker, and the section
  // it always produced.
  queries.Forget();
  const std::filesystem::path plain_file = scratch.Write("plain.cpp",
                                                         "struct Widget {\n"
                                                         "  int count;\n"
                                                         "};\n"
                                                         "\n"
                                                         "void Draw() {\n"
                                                         "  Paint();\n"
                                                         "}\n");

  OnAThreadOfItsOwn([&] {
    const std::vector<std::string> paths{plain_file.string()};
    std::string out;
    std::string error;
    EXPECT_TRUE(OverviewOf(paths, {}, out, error));
    EXPECT_EQ(error, std::string{});
    EXPECT_TRUE(out.find("partial:") == std::string::npos);
    EXPECT_TRUE(out.find("Widget@1:") != std::string::npos);
    EXPECT_TRUE(out.find("Draw@5: Paint@6") != std::string::npos);
  });
}

}  // namespace koi
