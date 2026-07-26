#ifndef COMMON_TEST_HARNESS_H_
#define COMMON_TEST_HARNESS_H_

// A test binary that returns EXIT_SUCCESS after printing failures is worse than no
// test at all, because CI and pre-commit hooks both believe it. Everything here
// funnels into a counter, and TestSummary() turns that into the exit code.
//
// Usage:
//   int main() {
//     TEST_CASE("widget parsing");
//     EXPECT_TRUE(Parse("x").has_value());
//     EXPECT_EQ(Parse("x")->name, "x");
//     return common::TestSummary();
//   }

#include <cstdlib>
#include <iostream>
#include <string_view>

namespace common {

inline int g_test_failures = 0;
inline int g_test_checks = 0;
inline std::string_view g_test_case = "<none>";

inline int TestSummary() {
  std::cout << g_test_checks << " checks, " << g_test_failures << " failed\n";
  if (g_test_failures != 0) {
    std::cout << "FAILED\n";
    return EXIT_FAILURE;
  }
  std::cout << "ok\n";
  return EXIT_SUCCESS;
}

}  // namespace common

// Names the group that subsequent failures are reported against, so a failure in a
// long run says what it was testing rather than only where.
#define TEST_CASE(name) (common::g_test_case = (name))

#define EXPECT_TRUE(expr)                                                                            \
  do {                                                                                               \
    ++common::g_test_checks;                                                                         \
    if (!(expr)) {                                                                                   \
      ++common::g_test_failures;                                                                      \
      std::cerr << "FAIL [" << common::g_test_case << "] " #expr << "\n      at " << __FILE__ << ':' \
                << __LINE__ << std::endl;                                                            \
    }                                                                                                \
  } while (0)

#define EXPECT_FALSE(expr) EXPECT_TRUE(!(expr))

// Prints both sides on failure, which is the difference between "this assertion
// broke" and knowing why without reaching for a debugger.
//
// The operands are copied rather than bound to `const auto&`: a reference would
// otherwise latch onto a temporary subobject, which -Wdangling-reference rejects.
// Copying costs nothing that matters in a test and removes the lifetime question.
#define EXPECT_EQ(lhs, rhs)                                                                            \
  do {                                                                                                 \
    ++common::g_test_checks;                                                                            \
    auto lhs_value_ = (lhs);                                                                           \
    auto rhs_value_ = (rhs);                                                                           \
    if (!(lhs_value_ == rhs_value_)) {                                                                 \
      ++common::g_test_failures;                                                                        \
      std::cerr << "FAIL [" << common::g_test_case << "] " #lhs " == " #rhs << "\n      got: "         \
                << lhs_value_ << "\n      want: " << rhs_value_ << "\n      at " << __FILE__ << ':'    \
                << __LINE__ << std::endl;                                                              \
    }                                                                                                  \
  } while (0)

#define EXPECT_THROWS(expr)                                                                          \
  do {                                                                                               \
    ++common::g_test_checks;                                                                         \
    bool caught_ = false;                                                                            \
    try {                                                                                            \
      expr;                                                                                          \
    } catch (...) {                                                                                  \
      caught_ = true;                                                                                \
    }                                                                                                \
    if (!caught_) {                                                                                  \
      ++common::g_test_failures;                                                                      \
      std::cerr << "FAIL [" << common::g_test_case << "] " #expr " did not throw"                    \
                << "\n      at " << __FILE__ << ':' << __LINE__ << std::endl;                        \
    }                                                                                                \
  } while (0)

#define EXPECT_NO_THROW(expr)                                                                        \
  do {                                                                                               \
    ++common::g_test_checks;                                                                         \
    try {                                                                                            \
      expr;                                                                                          \
    } catch (const std::exception& ex_) {                                                            \
      ++common::g_test_failures;                                                                      \
      std::cerr << "FAIL [" << common::g_test_case << "] " #expr " threw: " << ex_.what()            \
                << "\n      at " << __FILE__ << ':' << __LINE__ << std::endl;                        \
    }                                                                                                \
  } while (0)

#endif  // COMMON_TEST_HARNESS_H_
