#include "octopus/completion.hpp"

#include <algorithm>
#include <cstddef>
#include <limits>
#include <utility>

namespace octopus {
namespace {

constexpr std::size_t kLoopRecentWindow = 1024;
constexpr std::size_t kLoopRepeatCount = 4;
constexpr std::size_t kMaxRepeatedLineBytes = 80;
constexpr std::size_t kMinRepeatedWindowBytes = 8;
constexpr std::size_t kMaxRepeatedWindowBytes = 96;

std::vector<std::string>
normalize_stop_strings(std::vector<std::string> stop_strings) {
  std::vector<std::string> normalized;
  normalized.reserve(stop_strings.size());

  for (auto &stop : stop_strings) {
    if (stop.empty()) {
      continue;
    }
    if (std::find(normalized.begin(), normalized.end(), stop) ==
        normalized.end()) {
      normalized.push_back(std::move(stop));
    }
  }

  return normalized;
}

std::size_t matching_stop_index(const std::string &text,
                                const std::vector<std::string> &stop_strings) {
  std::size_t match = std::numeric_limits<std::size_t>::max();

  for (std::size_t index = 0; index < stop_strings.size(); ++index) {
    const auto &stop = stop_strings[index];
    if (text.size() < stop.size()) {
      continue;
    }
    if (text.compare(text.size() - stop.size(), stop.size(), stop) != 0) {
      continue;
    }
    if (match == std::numeric_limits<std::size_t>::max() ||
        stop.size() > stop_strings[match].size()) {
      match = index;
    }
  }

  return match;
}

bool has_non_space(std::string_view value) {
  return value.find_first_not_of(" \t\r\n") != std::string_view::npos;
}

bool has_space(std::string_view value) {
  return value.find_first_of(" \t\r\n") != std::string_view::npos;
}

bool all_equal_windows(std::string_view text, std::size_t unit_size) {
  const auto start = text.size() - (unit_size * kLoopRepeatCount);
  const auto first = text.substr(start, unit_size);

  for (std::size_t repeat = 1; repeat < kLoopRepeatCount; ++repeat) {
    if (text.substr(start + (repeat * unit_size), unit_size) != first) {
      return false;
    }
  }

  return true;
}

} // namespace

StopDetector::StopDetector(std::vector<std::string> stop_strings)
    : stop_strings_(normalize_stop_strings(std::move(stop_strings))) {}

bool StopDetector::append(std::string_view chunk) {
  text_.append(chunk.data(), chunk.size());

  const auto match = matching_stop_index(text_, stop_strings_);
  if (match == std::numeric_limits<std::size_t>::max()) {
    return false;
  }

  const auto &stop = stop_strings_[match];
  text_.erase(text_.size() - stop.size());
  return true;
}

void StopDetector::truncate(std::size_t size) {
  if (size < text_.size()) {
    text_.erase(size);
  }
}

const std::string &StopDetector::text() const noexcept { return text_; }

const std::vector<std::string> &StopDetector::stop_strings() const noexcept {
  return stop_strings_;
}

bool LoopDetector::append(std::string_view chunk) {
  generated_size_ += chunk.size();
  if (detected_) {
    return true;
  }

  recent_text_.append(chunk.data(), chunk.size());
  if (recent_text_.size() > kLoopRecentWindow) {
    recent_text_.erase(0, recent_text_.size() - kLoopRecentWindow);
  }

  return detect_repeated_lines() || detect_repeated_windows();
}

bool LoopDetector::detect_repeated_lines() {
  if (recent_text_.empty() || recent_text_.back() != '\n') {
    return false;
  }

  std::vector<std::string_view> lines;
  lines.reserve(kLoopRepeatCount);

  std::size_t line_end = recent_text_.size();
  while (line_end > 0 && lines.size() < kLoopRepeatCount) {
    const auto previous_newline =
        line_end > 1 ? recent_text_.rfind('\n', line_end - 2)
                     : std::string::npos;
    const auto line_start =
        previous_newline == std::string::npos ? 0 : previous_newline + 1;
    lines.push_back(std::string_view(recent_text_).substr(
        line_start, line_end - line_start));

    if (previous_newline == std::string::npos) {
      break;
    }
    line_end = line_start;
  }

  if (lines.size() != kLoopRepeatCount || lines[0].size() > kMaxRepeatedLineBytes ||
      !has_non_space(lines[0])) {
    return false;
  }

  for (const auto line : lines) {
    if (line != lines[0]) {
      return false;
    }
  }

  record_detection(lines[0].size());
  return true;
}

bool LoopDetector::detect_repeated_windows() {
  const auto max_unit_size =
      std::min(kMaxRepeatedWindowBytes, recent_text_.size() / kLoopRepeatCount);

  for (std::size_t unit_size = kMinRepeatedWindowBytes;
       unit_size <= max_unit_size; ++unit_size) {
    if (!all_equal_windows(recent_text_, unit_size)) {
      continue;
    }

    const auto repeated_start =
        recent_text_.size() - (unit_size * kLoopRepeatCount);
    const auto unit =
        std::string_view(recent_text_).substr(repeated_start, unit_size);
    if (!has_non_space(unit) || !has_space(unit)) {
      continue;
    }

    record_detection(unit_size);
    return true;
  }

  return false;
}

void LoopDetector::record_detection(std::size_t unit_size) {
  detected_ = true;
  trim_size_ = generated_size_ - (unit_size * (kLoopRepeatCount - 1));
}

bool LoopDetector::detected() const noexcept { return detected_; }

std::size_t LoopDetector::generated_size() const noexcept {
  return generated_size_;
}

std::size_t LoopDetector::trim_size() const noexcept {
  return detected_ ? trim_size_ : generated_size_;
}

} // namespace octopus
