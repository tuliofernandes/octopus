#include "octopus/completion.hpp"

#include <algorithm>
#include <cstddef>
#include <limits>
#include <utility>

namespace octopus {
namespace {

/**
 * Keep loop detection bounded and conservative. The detector should catch
 * obvious runaway text without becoming a second model evaluator.
 */
constexpr std::size_t kLoopRecentWindow = 1024;
constexpr std::size_t kLoopRepeatCount = 4;
constexpr std::size_t kMaxRepeatedLineBytes = 80;
constexpr std::size_t kMinRepeatedWindowBytes = 8;
constexpr std::size_t kMaxRepeatedWindowBytes = 96;

std::vector<std::string> normalizeStopStrings(
    std::vector<std::string> stop_strings) {
  std::vector<std::string> normalized;
  normalized.reserve(stop_strings.size());

  // Empty stops would match everything, and duplicates only add extra checks.
  for (auto& stop : stop_strings) {
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

std::size_t maxSize(const std::vector<std::string>& values) {
  std::size_t result = 0;
  for (const auto& value : values) {
    result = std::max(result, value.size());
  }
  return result;
}

std::size_t matchingStopIndex(const std::string& text,
                              const std::vector<std::string>& stop_strings) {
  std::size_t match = std::numeric_limits<std::size_t>::max();

  /**
   * Prefer the longest matching suffix so a short stop cannot steal a more
   * specific model delimiter that ends at the same position.
   */
  for (std::size_t index = 0; index < stop_strings.size(); ++index) {
    const auto& stop = stop_strings[index];
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

bool hasNonSpace(std::string_view value) {
  return value.find_first_not_of(" \t\r\n") != std::string_view::npos;
}

bool hasSpace(std::string_view value) {
  return value.find_first_of(" \t\r\n") != std::string_view::npos;
}

bool allEqualWindows(std::string_view text, std::size_t unit_size) {
  const auto start = text.size() - (unit_size * kLoopRepeatCount);
  const auto first = text.substr(start, unit_size);

  for (std::size_t repeat = 1; repeat < kLoopRepeatCount; ++repeat) {
    if (text.substr(start + (repeat * unit_size), unit_size) != first) {
      return false;
    }
  }

  return true;
}

}  // namespace

StopDetector::StopDetector(std::vector<std::string> stop_strings)
    : stop_strings_(normalizeStopStrings(std::move(stop_strings))) {}

bool StopDetector::append(std::string_view chunk) {
  /**
   * Token pieces can split a textual stop marker, so matching must happen on
   * accumulated output rather than only on the newest chunk.
   */
  text_.append(chunk.data(), chunk.size());

  const auto match = matchingStopIndex(text_, stop_strings_);
  if (match == std::numeric_limits<std::size_t>::max()) {
    return false;
  }

  const auto& stop = stop_strings_[match];
  // The stop marker belongs to prompt protocol, not the assistant answer.
  text_.erase(text_.size() - stop.size());
  return true;
}

void StopDetector::truncate(std::size_t size) {
  /**
   * LoopDetector reports the safe visible prefix; StopDetector owns the final
   * accumulated text, so truncation happens here.
   */
  if (size < text_.size()) {
    text_.erase(size);
  }
}

const std::string& StopDetector::text() const noexcept { return text_; }

const std::vector<std::string>& StopDetector::stopStrings() const noexcept {
  return stop_strings_;
}

StopSafeTextBuffer::StopSafeTextBuffer(std::vector<std::string> stop_strings)
    : max_stop_size_(maxSize(normalizeStopStrings(std::move(stop_strings)))) {}

std::string StopSafeTextBuffer::append(std::string_view chunk) {
  if (chunk.empty()) {
    return {};
  }

  buffer_.append(chunk.data(), chunk.size());
  const auto keep_size = max_stop_size_ == 0 ? 0 : max_stop_size_ - 1;
  if (buffer_.size() <= keep_size) {
    return {};
  }

  const auto emit_size = buffer_.size() - keep_size;
  std::string emitted = buffer_.substr(0, emit_size);
  buffer_.erase(0, emit_size);
  emitted_size_ += emitted.size();
  return emitted;
}

std::string StopSafeTextBuffer::flush(std::string_view final_text) {
  buffer_.clear();
  if (final_text.size() <= emitted_size_) {
    return {};
  }

  std::string emitted(final_text.substr(emitted_size_));
  emitted_size_ += emitted.size();
  return emitted;
}

bool LoopDetector::append(std::string_view chunk) {
  /**
   * generated_size_ tracks the full output length even though recent_text_ is a
   * bounded window. That lets us trim the final answer at the right offset.
   */
  generated_size_ += chunk.size();
  if (detected_) {
    return true;
  }

  recent_text_.append(chunk.data(), chunk.size());
  // Keep memory bounded while still seeing enough context to catch short loops.
  if (recent_text_.size() > kLoopRecentWindow) {
    recent_text_.erase(0, recent_text_.size() - kLoopRecentWindow);
  }

  return detectRepeatedLines() || detectRepeatedWindows();
}

bool LoopDetector::detectRepeatedLines() {
  // Line loops are common visible failures: "foo\nfoo\nfoo\nfoo\n".
  if (recent_text_.empty() || recent_text_.back() != '\n') {
    return false;
  }

  std::vector<std::string_view> lines;
  lines.reserve(kLoopRepeatCount);

  std::size_t line_end = recent_text_.size();
  while (line_end > 0 && lines.size() < kLoopRepeatCount) {
    const auto previous_newline = line_end > 1
                                      ? recent_text_.rfind('\n', line_end - 2)
                                      : std::string::npos;
    const auto line_start =
        previous_newline == std::string::npos ? 0 : previous_newline + 1;
    lines.push_back(std::string_view(recent_text_)
                        .substr(line_start, line_end - line_start));

    if (previous_newline == std::string::npos) {
      break;
    }
    line_end = line_start;
  }

  if (lines.size() != kLoopRepeatCount ||
      lines[0].size() > kMaxRepeatedLineBytes || !hasNonSpace(lines[0])) {
    return false;
  }

  for (const auto line : lines) {
    if (line != lines[0]) {
      return false;
    }
  }

  recordDetection(lines[0].size());
  return true;
}

bool LoopDetector::detectRepeatedWindows() {
  // Window loops catch repeated phrases that do not align to line breaks.
  const auto max_unit_size =
      std::min(kMaxRepeatedWindowBytes, recent_text_.size() / kLoopRepeatCount);

  for (std::size_t unit_size = kMinRepeatedWindowBytes;
       unit_size <= max_unit_size; ++unit_size) {
    if (!allEqualWindows(recent_text_, unit_size)) {
      continue;
    }

    const auto repeated_start =
        recent_text_.size() - (unit_size * kLoopRepeatCount);
    const auto unit =
        std::string_view(recent_text_).substr(repeated_start, unit_size);
    /**
     * Require some language-like shape so runs of punctuation or whitespace do
     * not accidentally count as model loops.
     */
    if (!hasNonSpace(unit) || !hasSpace(unit)) {
      continue;
    }

    recordDetection(unit_size);
    return true;
  }

  return false;
}

void LoopDetector::recordDetection(std::size_t unit_size) {
  detected_ = true;
  /**
   * Keep the first copy and trim the repeated copies. This salvages the useful
   * answer prefix instead of returning an obvious runaway loop.
   */
  trim_size_ = generated_size_ - (unit_size * (kLoopRepeatCount - 1));
}

bool LoopDetector::detected() const noexcept { return detected_; }

std::size_t LoopDetector::generatedSize() const noexcept {
  return generated_size_;
}

std::size_t LoopDetector::trimSize() const noexcept {
  return detected_ ? trim_size_ : generated_size_;
}

}  // namespace octopus
