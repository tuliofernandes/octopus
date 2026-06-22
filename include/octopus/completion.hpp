#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace octopus {

class StopDetector {
public:
  explicit StopDetector(std::vector<std::string> stop_strings);

  bool append(std::string_view chunk);
  void truncate(std::size_t size);

  const std::string &text() const noexcept;
  const std::vector<std::string> &stop_strings() const noexcept;

private:
  std::vector<std::string> stop_strings_;
  std::string text_;
};

class LoopDetector {
public:
  bool append(std::string_view chunk);

  bool detected() const noexcept;
  std::size_t generated_size() const noexcept;
  std::size_t trim_size() const noexcept;

private:
  bool detect_repeated_lines();
  bool detect_repeated_windows();
  void record_detection(std::size_t unit_size);

  std::string recent_text_;
  std::size_t generated_size_ = 0;
  std::size_t trim_size_ = 0;
  bool detected_ = false;
};

} // namespace octopus
