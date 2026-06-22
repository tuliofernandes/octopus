#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace octopus {

class StopDetector {
public:
  explicit StopDetector(std::vector<std::string> stop_strings);

  bool append(std::string_view chunk);

  const std::string &text() const noexcept;
  const std::vector<std::string> &stop_strings() const noexcept;

private:
  std::vector<std::string> stop_strings_;
  std::string text_;
};

} // namespace octopus
