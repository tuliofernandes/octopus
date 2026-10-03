#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace octopus::llm {

class StopDetector {
 public:
  explicit StopDetector(std::vector<std::string> stop_strings);

  bool append(std::string_view chunk);
  void truncate(std::size_t size);

  const std::string& text() const noexcept;
  const std::vector<std::string>& stopStrings() const noexcept;

 private:
  std::vector<std::string> stop_strings_;
  std::string text_;
};

class StopSafeTextBuffer {
 public:
  explicit StopSafeTextBuffer(std::vector<std::string> stop_strings);

  std::string append(std::string_view chunk);
  std::string flush(std::string_view final_text);

 private:
  std::size_t max_stop_size_ = 0;
  std::size_t emitted_size_ = 0;
  std::string buffer_;
};

class LoopDetector {
 public:
  bool append(std::string_view chunk);

  bool detected() const noexcept;
  std::size_t generatedSize() const noexcept;
  std::size_t trimSize() const noexcept;

 private:
  bool detectRepeatedLines();
  bool detectRepeatedWindows();
  void recordDetection(std::size_t unit_size);

  std::string recent_text_;
  std::size_t generated_size_ = 0;
  std::size_t trim_size_ = 0;
  bool detected_ = false;
};

}  // namespace octopus::llm
