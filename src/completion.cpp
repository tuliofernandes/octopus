#include "octopus/completion.hpp"

#include <algorithm>
#include <cstddef>
#include <limits>
#include <utility>

namespace octopus {
namespace {

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

const std::string &StopDetector::text() const noexcept { return text_; }

const std::vector<std::string> &StopDetector::stop_strings() const noexcept {
  return stop_strings_;
}

} // namespace octopus
