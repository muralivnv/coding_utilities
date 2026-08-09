#ifndef ERROR_H_
#define ERROR_H_

#include <system_error>
#include <source_location>
#include <string>

namespace koi {

struct ErrorCtx final {
  std::error_code ec{};
  std::source_location src_loc{};

  operator bool() const { return static_cast<bool>(ec); }
  bool operator==(const std::error_code e) const { return ec == e; }
};

enum class PieceTableErrorCode : int {
  kNoError=0,

  kOutOfBoundsDocPos = 100,
  kOutOfBoundsInsertPos,
  kElementShiftBufferOverflow,
  kEmptyInputString,
  kEmptyInputPieceList,
  kEmptyPieceSplit,
  kMismatchInputStringAndDocRange,
  kMalformedUtf8Input,
  kDocPosNotOnGraphemeBoundary,

  kPieceCountMaxLimitReached = 150,
  kPieceRangeQueryReturnedEmpty,
  kPieceTableEmpty,

  kUnknownCmdInfo = 200,
  kApplyCmdNotPossible,

  kTestingPieceStringNotEqBruteForceString = 2000,

  kUnknownError = 3000
};

class PieceTableErrorCategory : public std::error_category {
 public:
  const char* name() const noexcept override;
  std::string message(int ev) const override;
};

std::string FormatErrorCtx(const ErrorCtx& error_ctx);
std::string FormatErrorCtxDebug(const ErrorCtx& error_ctx);
const PieceTableErrorCategory& PieceTableErrorCategoryInstance();

template<typename ErrorCodeEnum>
inline __attribute__((always_inline))
ErrorCtx MakeErrorCtx(const ErrorCodeEnum code, const std::error_category& category,
                      std::source_location loc = std::source_location::current()) {
  ErrorCtx ctx;
  ctx.ec = std::error_code{static_cast<int>(code), category};
  ctx.src_loc = std::move(loc);
  return ctx;
}

template<typename ErrorCodeEnum>
inline __attribute__((always_inline))
ErrorCtx MakeErrorCtx(const ErrorCodeEnum code,
                      std::source_location loc = std::source_location::current()) {
  return MakeErrorCtx(code, std::generic_category(), loc);
}

template<>
inline __attribute__((always_inline))
ErrorCtx MakeErrorCtx(const PieceTableErrorCode code, std::source_location loc) {
  return MakeErrorCtx(code, PieceTableErrorCategoryInstance(), loc);
}

inline __attribute__((always_inline))
ErrorCtx Success() { return {}; }

}

#endif
