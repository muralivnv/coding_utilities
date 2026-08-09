#include "error.h"

namespace koi {

std::string FormatErrorCtx(const ErrorCtx& error_ctx) {
  if (!error_ctx) return {};
  return error_ctx.ec.message();
}

std::string FormatErrorCtxDebug(const ErrorCtx& error_ctx) {
  std::string error_msg;
  if (error_ctx) {
    error_msg += error_ctx.ec.category().name();
    error_msg += ": ";
    error_msg += error_ctx.ec.message();
    error_msg += " [";
    error_msg += std::to_string(error_ctx.ec.value());
    error_msg += "]\n\tFile: ";
    error_msg += error_ctx.src_loc.file_name();
    error_msg += ':';
    error_msg += std::to_string(error_ctx.src_loc.line());
    error_msg += "\n\tFun: ";
    error_msg += error_ctx.src_loc.function_name();
    error_msg += '\n';
  }
  return error_msg;
}

const char* PieceTableErrorCategory::name() const noexcept {
  return "PieceTableErrorCategory";
}

std::string PieceTableErrorCategory::message(int ev) const {
  using namespace std::string_literals;
  switch (static_cast<PieceTableErrorCode>(ev)) {
    case PieceTableErrorCode::kNoError:
      return "No Error"s;
    case PieceTableErrorCode::kOutOfBoundsDocPos:
      return "Out-of-bounds document position"s;
    case PieceTableErrorCode::kOutOfBoundsInsertPos:
      return "Out-of-bounds insertion position"s;
    case PieceTableErrorCode::kElementShiftBufferOverflow:
      return "Element shift would result in buffer overflow"s;
    case PieceTableErrorCode::kEmptyInputString:
      return "Input string is empty"s;
    case PieceTableErrorCode::kEmptyInputPieceList:
      return "Input piece list is empty"s;
    case PieceTableErrorCode::kEmptyPieceSplit:
      return "Both lhs and rhs split is empty"s;
    case PieceTableErrorCode::kMismatchInputStringAndDocRange:
      return "Intput string and doc range range size mismatch"s;
    case PieceTableErrorCode::kMalformedUtf8Input:
      return "Input string is not well-formed UTF-8"s;
    case PieceTableErrorCode::kDocPosNotOnGraphemeBoundary:
      return "Document position is inside a grapheme cluster"s;
    case PieceTableErrorCode::kPieceCountMaxLimitReached:
      return "Piece count max-limit reached and cannot reduce piece count"s;
    case PieceTableErrorCode::kPieceRangeQueryReturnedEmpty:
      return "Piece range query returned empty"s;
    case PieceTableErrorCode::kPieceTableEmpty:
      return "Piece table is empty"s;
    case PieceTableErrorCode::kUnknownCmdInfo:
      return "Unknown Command Info Action"s;
    case PieceTableErrorCode::kApplyCmdNotPossible:
      return "Command Info cannot be applied, piece table could be full"s;
    case PieceTableErrorCode::kTestingPieceStringNotEqBruteForceString:
      return "Piece string != Brute force string"s;
    case PieceTableErrorCode::kUnknownError:
      return "Unknown error"s;
  }
  return "Unrecognized error"s;
}

const PieceTableErrorCategory& PieceTableErrorCategoryInstance() {
  static PieceTableErrorCategory instance;
  return instance;
}

}
