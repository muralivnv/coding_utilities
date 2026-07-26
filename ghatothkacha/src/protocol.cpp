#include "protocol.h"

#include <cctype>
#include <vector>

#include "format.h"

namespace ghatothkacha {

std::string_view Strip(std::string_view v) {
  // remove leading whitespace
  size_t start = 0;
  while (start < v.size() && std::isspace(static_cast<unsigned char>(v[start]))) ++start;
  v.remove_prefix(start);

  // remove trailing whitespace
  while (!v.empty() && std::isspace(static_cast<unsigned char>(v.back()))) {
    v.remove_suffix(1);
  }

  return v;
}

std::optional<HistoryItem> ToInsertItem(std::string_view id, std::string_view cmd, std::string_view dir,
                                        std::string_view start_time_ns) {
  if (Strip(id).empty()) {
    rostd::printf<"Cannot perform history insert, input 'id' is empty\n">();
    return std::nullopt;
  }
  if (Strip(cmd).empty()) {
    rostd::printf<"Cannot perform history insert, input 'cmd' is empty\n">();
    return std::nullopt;
  }
  if (Strip(dir).empty()) {
    rostd::printf<"Cannot perform history insert, input 'dir' is empty\n">();
    return std::nullopt;
  }
  if (Strip(start_time_ns).empty()) {
    rostd::printf<"Cannot perform history insert, input 'start_time_ns' is empty\n">();
    return std::nullopt;
  }
  HistoryItem item;
  item.id = id;
  item.cmd = cmd;
  item.dir = dir;

  {  // decode start_time_ns
    auto result = StringToInt<uint64_t>(start_time_ns);
    if (!result) {
      rostd::printf<"Error while decoding 'start_time_ns'\nInput 'start_time_ns': %s">(start_time_ns);
      return std::nullopt;
    }
    item.start_timestamp_ns = result.value();
  }
  return item;
}

std::optional<HistoryItem> ToUpdateItem(std::string_view id, std::string_view end_time_ns,
                                        std::string_view retcode) {
  if (Strip(id).empty()) {
    rostd::printf<"Cannot perform history update, input 'id' is empty\n">();
    return std::nullopt;
  }
  if (Strip(end_time_ns).empty()) {
    rostd::printf<"Cannot perform history update, input 'end_time_ns' is empty\n">();
    return std::nullopt;
  }
  if (Strip(retcode).empty()) {
    rostd::printf<"Cannot perform history update, input 'retcode' is empty\n">();
    return std::nullopt;
  }

  HistoryItem item;
  item.id = id;

  {  // decode end_time_ns
    auto result = StringToInt<uint64_t>(end_time_ns);
    if (!result) {
      rostd::printf<"Error while decoding 'end_time_ns'\nInput 'end_time_ns': %s">(end_time_ns);
      return std::nullopt;
    }
    item.end_timestamp_ns = result.value();
  }

  {  // decode retcode
    auto result = StringToInt<int>(retcode);
    if (!result) {
      rostd::printf<"Error while decoding 'retcode'\nInput 'retcode': %s">(retcode);
      return std::nullopt;
    }
    item.retcode = result.value();
  }
  return item;
}

std::string_view EncodeMessage(const HistoryItem& item, bool is_update) {
  if (is_update) {
    return common::FormatIntoStringView<"U%?%?%?%?%?%?">(kDelimiter, item.id, kDelimiter, item.end_timestamp_ns,
                                                         kDelimiter, item.retcode);
  }
  return common::FormatIntoStringView<"I%?%?%?%?%?%?%?%?">(kDelimiter, item.id, kDelimiter, item.start_timestamp_ns,
                                                           kDelimiter, item.dir, kDelimiter, item.cmd);
}

Message DecodeMessage(std::string_view datagram) {
  Message msg;
  // A single byte cannot carry an action plus a delimiter, let alone fields.
  if (datagram.size() <= 1)
    return msg;

  const char action = datagram.front();
  std::string_view payload = datagram.substr(1);
  if (payload.empty() || payload.front() != kDelimiter)
    return msg;
  payload.remove_prefix(1);  // drop the leading delimiter

  std::vector<std::string_view> fields;
  fields.reserve(4);
  size_t start = 0;
  size_t end = payload.find(kDelimiter);
  while (end != std::string_view::npos) {
    fields.push_back(payload.substr(start, end - start));
    start = end + 1;
    end = payload.find(kDelimiter, start);
  }
  fields.push_back(payload.substr(start));

  // The field counts are exact: a delimiter inside a command would produce an extra
  // field and the entry is dropped rather than stored mangled.
  if (action == 'I' && fields.size() == 4) {
    if (const std::optional<HistoryItem> item = ToInsertItem(fields[0], fields[3], fields[2], fields[1])) {
      msg.action = Action::kInsert;
      msg.item = *item;
    }
  } else if (action == 'U' && fields.size() == 3) {
    if (const std::optional<HistoryItem> item = ToUpdateItem(fields[0], fields[1], fields[2])) {
      msg.action = Action::kUpdate;
      msg.item = *item;
    }
  }
  return msg;
}

}  // namespace ghatothkacha
