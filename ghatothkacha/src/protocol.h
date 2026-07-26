#ifndef GHATOTHKACHA_PROTOCOL_H_
#define GHATOTHKACHA_PROTOCOL_H_

// The wire format the shell hook and the daemon speak over a unix datagram
// socket, plus the field validation either side applies.
//
// A datagram is an action byte, then a delimiter-prefixed, delimiter-separated
// field list:
//
//   I <US> id <US> start_timestamp_ns <US> dir <US> cmd
//   U <US> id <US> end_timestamp_ns   <US> retcode
//
// US is 0x1F, chosen because a shell command will not contain it.

#include <charconv>
#include <concepts>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "printx.hpp"

namespace ghatothkacha {

constexpr char kDelimiter = '\x1F';

struct HistoryItem {
  std::string_view id{""};
  std::string_view cmd{""};
  std::string_view dir{""};
  uint64_t start_timestamp_ns{0};
  uint64_t end_timestamp_ns{0};
  int retcode{0};
};

enum class Action { kNone, kInsert, kUpdate };

// kNone means the datagram was malformed or a field failed validation. There is no
// error detail on purpose: a shell hook has nobody to report to, and dropping one
// entry beats blocking the prompt.
struct Message {
  Action action{Action::kNone};
  HistoryItem item{};
};

std::string_view Strip(std::string_view v);

template <std::integral T>
std::optional<T> StringToInt(std::string_view input) {
  T result{};
  auto [ptr, ec] = std::from_chars(input.data(), input.data() + input.size(), result);
  if (ec == std::errc()) {
    return result;
  } else if (ec == std::errc::invalid_argument) {
    rostd::printf<"Input is not a number\n">();
  } else if (ec == std::errc::result_out_of_range) {
    rostd::printf<"Input is larger than the specified type\n">();
  }
  return std::nullopt;
}

// Validate the fields of an insert or an update. Every field is required and must
// be non-blank; the numeric ones must parse whole.
std::optional<HistoryItem> ToInsertItem(std::string_view id, std::string_view cmd, std::string_view dir,
                                        std::string_view start_time_ns);
std::optional<HistoryItem> ToUpdateItem(std::string_view id, std::string_view end_time_ns,
                                        std::string_view retcode);

// Serialises `item` for the wire. The returned view points into a reusable buffer
// and stays valid until the next call, which keeps the shell hook allocation-free.
std::string_view EncodeMessage(const HistoryItem& item, bool is_update);

// Parses a received datagram. The returned item's views point into `datagram`, so
// it must outlive the Message.
Message DecodeMessage(std::string_view datagram);

}  // namespace ghatothkacha

#endif  // GHATOTHKACHA_PROTOCOL_H_
