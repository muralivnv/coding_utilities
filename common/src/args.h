#ifndef COMMON_ARGS_H_
#define COMMON_ARGS_H_

#include <unordered_map>
#include <string_view>
#include <optional>
#include <vector>

namespace common {

class Args {
 public:
  Args(int argc, char** argv);
  ~Args() = default;
  inline bool Has(std::string_view key) const noexcept {
    return args_.find(key) != args_.end();
  }
  std::optional<std::vector<std::string_view>> MultiValue(const std::vector<std::string_view>& keys,
                                                          bool parse_till_next_flag = false) const noexcept;
  std::optional<std::string_view> Value(const std::vector<std::string_view>& keys) const noexcept;

 private:
  std::unordered_multimap<std::string_view, size_t> args_;
  std::vector<std::string_view> argv_;
  std::vector<std::string_view> Impl(std::string_view key, bool parse_till_next_flag = false) const noexcept;
};

} // namespace common

#endif // COMMON_ARGS_H_
