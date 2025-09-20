#include "args.h"

namespace common {

Args::Args(int argc, char** argv) {
  argv_.reserve(size_t(argc-1));
  for (int i = 1; i < argc; i++) {
    args_.emplace(argv[i], size_t(i-1));
    argv_.emplace_back(argv[i]);
  }
}

std::optional<std::vector<std::string_view>> Args::MultiValue(const std::vector<std::string_view>& keys,
                                                              bool parse_till_next_flag) const noexcept {
  std::vector<std::string_view> result;
  for (const std::string_view key : keys) {
    auto values = Impl(key, parse_till_next_flag);
    result.insert(result.end(), values.begin(), values.end());
  }
  if (result.empty()) return std::nullopt;
  return result;
}

std::optional<std::string_view> Args::Value(const std::vector<std::string_view>& keys) const noexcept {
  for (const std::string_view key : keys) {
    auto values = Impl(key, false);
    if (!values.empty()) return values.front();
  }
  return std::nullopt;
}

std::vector<std::string_view> Args::Impl(std::string_view key, bool parse_till_next_flag) const noexcept {
  std::vector<std::string_view> result;
  auto [start, end] = args_.equal_range(key);
  for (auto it = start; it != end; it++) {
    size_t i = it->second + 1;
    const size_t k = parse_till_next_flag  ? argv_.size() : std::min(i + 1, argv_.size());
    for (; i < k; i++) {
      if (argv_[i].starts_with('-')) break;
      result.push_back(argv_[i]);
    }
  }
  return result;
}

} // namespace common
