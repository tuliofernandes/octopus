#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace octopus {

/**
 * Accumulates decoded token pieces and stops when the visible text ends with a
 * configured boundary. It trims the boundary so users do not see model syntax.
 */
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

/**
 * Detects a small set of obvious repetition failures. This is not "AI quality"
 * scoring; it is a practical safety net for raw decoding loops.
 */
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
