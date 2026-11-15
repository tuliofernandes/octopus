#include "octopus/cli/terminal_renderer.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <mutex>
#include <ostream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

namespace octopus {
namespace {
void ensureTranscriptEnding(std::string& output) {
  while (!output.empty() && output.back() == '\n') {
    output.pop_back();
  }
  output += "\n\n";
}

}  // namespace

TranscriptRenderer::TranscriptRenderer(
    std::ostream& out, TerminalCapabilities capabilities,
    WidthQuery width_query, std::chrono::milliseconds spinner_interval)
    : out_(out),
      capabilities_(std::move(capabilities)),
      width_query_(std::move(width_query)),
      spinner_interval_(
          std::max(spinner_interval, std::chrono::milliseconds{1})) {}

TranscriptRenderer::~TranscriptRenderer() noexcept { clearActivity(); }

void TranscriptRenderer::showThinking() {
  stopSpinner();
  writeActivity("Thinking...", false);
}

void TranscriptRenderer::finishThinking(
    std::chrono::steady_clock::duration elapsed) {
  std::lock_guard<std::mutex> lock(output_mutex_);
  if (!activity_visible_) {
    return;
  }
  writeActivityLocked("Thought for " + formatElapsed(elapsed) + ".", true);
}

void TranscriptRenderer::startSpinner() {
  stopSpinner();
  const auto width = currentWidth();
  if (!capabilities_.interactive_session || !capabilities_.cursor_control ||
      !capabilities_.erase_line || !width.has_value() || *width < 4) {
    return;
  }

  {
    std::lock_guard<std::mutex> lock(output_mutex_);
    spinner_stop_requested_ = false;
    writeActivityLocked(capabilities_.utf8 ? "\xE2\xA0\x8B" : "|", false);
  }
  try {
    spinner_thread_ = std::thread(&TranscriptRenderer::runSpinner, this);
  } catch (...) {
    std::lock_guard<std::mutex> lock(output_mutex_);
    spinner_stop_requested_ = true;
    clearActivityLocked();
  }
}

void TranscriptRenderer::stopSpinner() noexcept {
  {
    std::lock_guard<std::mutex> lock(output_mutex_);
    spinner_stop_requested_ = true;
  }
  spinner_wakeup_.notify_all();
  if (spinner_thread_.joinable()) {
    spinner_thread_.join();
  }
  std::lock_guard<std::mutex> lock(output_mutex_);
  clearActivityLocked();
}

void TranscriptRenderer::clearActivity() noexcept { stopSpinner(); }

void TranscriptRenderer::renderAssistant(std::string_view markdown) {
  clearActivity();
  const std::string sanitized = sanitizeTerminalText(markdown);
  std::string encoded;
  if (capabilities_.interactive_session && capabilities_.sgr_attributes) {
    try {
      const auto document = parseMarkdown(sanitized, currentWidth());
      if (document.has_value()) {
        encoded = encodeTerminalDocument(
            *document, {capabilities_.sgr_attributes, capabilities_.color,
                        capabilities_.color ? capabilities_.response_background
                                            : std::nullopt,
                        capabilities_.truecolor, capabilities_.erase_line,
                        capabilities_.terminal_background});
      } else {
        encoded = sanitized;
      }
    } catch (...) {
      encoded = sanitized;
    }
  } else {
    encoded = sanitized;
  }
  ensureTranscriptEnding(encoded);
  if (capabilities_.interactive_session) {
    encoded.insert(encoded.begin(), '\n');
  }
  std::lock_guard<std::mutex> lock(output_mutex_);
  out_.write(encoded.data(), static_cast<std::streamsize>(encoded.size()));
  out_.flush();
}

void TranscriptRenderer::finishTurn() noexcept {}

std::optional<std::size_t> TranscriptRenderer::currentWidth() const {
  if (width_query_) {
    return width_query_();
  }
  return capabilities_.columns;
}

void TranscriptRenderer::writeActivity(std::string_view text, bool permanent) {
  std::lock_guard<std::mutex> lock(output_mutex_);
  writeActivityLocked(text, permanent);
}

void TranscriptRenderer::writeActivityLocked(std::string_view text,
                                             bool permanent) {
  if (!capabilities_.interactive_session || !capabilities_.cursor_control ||
      !capabilities_.erase_line) {
    return;
  }
  const std::optional<std::size_t> width = currentWidth();
  if (!width.has_value() || *width < 4) {
    return;
  }

  std::string displayed(text);
  if (displayed.size() >= *width) {
    const std::size_t retained = *width > 4 ? *width - 4 : 0;
    displayed.resize(retained);
    displayed += "...";
  }
  std::string output = "\r\x1b[2K" + displayed + "\x1b[0m";
  if (permanent) {
    output.push_back('\n');
  }
  out_.write(output.data(), static_cast<std::streamsize>(output.size()));
  out_.flush();
  activity_visible_ = !permanent;
}

void TranscriptRenderer::clearActivityLocked() noexcept {
  if (!activity_visible_) {
    return;
  }
  try {
    constexpr std::string_view clear = "\r\x1b[2K\x1b[0m";
    out_.write(clear.data(), static_cast<std::streamsize>(clear.size()));
    out_.flush();
  } catch (...) {
  }
  activity_visible_ = false;
}

void TranscriptRenderer::runSpinner() noexcept {
  constexpr std::array<std::string_view, 8> kUtf8Frames{
      "\xE2\xA0\x8B", "\xE2\xA0\x99", "\xE2\xA0\xB9", "\xE2\xA0\xB8",
      "\xE2\xA0\xBC", "\xE2\xA0\xB4", "\xE2\xA0\xA6", "\xE2\xA0\xA7"};
  constexpr std::array<std::string_view, 4> kAsciiFrames{"|", "/", "-", "\\"};
  std::size_t frame = 1;
  std::unique_lock<std::mutex> lock(output_mutex_);
  while (!spinner_stop_requested_) {
    if (spinner_wakeup_.wait_for(lock, spinner_interval_,
                                 [this] { return spinner_stop_requested_; })) {
      break;
    }
    try {
      if (capabilities_.utf8) {
        writeActivityLocked(kUtf8Frames[frame % kUtf8Frames.size()], false);
        frame = (frame + 1) % kUtf8Frames.size();
      } else {
        writeActivityLocked(kAsciiFrames[frame % kAsciiFrames.size()], false);
        frame = (frame + 1) % kAsciiFrames.size();
      }
    } catch (...) {
      spinner_stop_requested_ = true;
    }
  }
}

}  // namespace octopus
