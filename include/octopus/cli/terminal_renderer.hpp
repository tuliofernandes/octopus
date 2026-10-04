#pragma once

#include <chrono>
#include <cstddef>
#include <functional>
#include <iosfwd>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace octopus {

struct TerminalCapabilities {
  bool interactive_session = false;
  bool cursor_control = false;
  bool erase_line = false;
  bool sgr_attributes = false;
  bool color = false;
  bool utf8 = false;
  std::optional<std::size_t> columns;
};

TerminalCapabilities makeTerminalCapabilities(
    bool input_is_tty, bool output_is_tty, std::string_view term,
    std::optional<std::string_view> no_color, bool utf8,
    std::optional<std::size_t> columns) noexcept;
TerminalCapabilities detectTerminalCapabilities(std::istream& in,
                                                std::ostream& out);
std::optional<std::size_t> queryTerminalWidth(int fd) noexcept;

std::string sanitizeTerminalText(std::string_view text);
std::string formatElapsed(std::chrono::steady_clock::duration elapsed);

struct TerminalTextStyle {
  bool bold = false;
  bool italic = false;
  bool code = false;
  bool strikethrough = false;
  bool accent = false;
};

struct TerminalRun {
  std::string text;
  TerminalTextStyle style;
};

struct TerminalLine {
  std::vector<TerminalRun> runs;
};

struct TerminalDocument {
  std::vector<TerminalLine> lines;
};

struct TerminalEncodingPolicy {
  bool sgr_attributes = false;
  bool color = false;
};

std::optional<TerminalDocument> parseMarkdown(std::string_view markdown);
std::string encodeTerminalDocument(const TerminalDocument& document,
                                   TerminalEncodingPolicy policy);

class TranscriptRenderer final {
 public:
  using WidthQuery = std::function<std::optional<std::size_t>()>;

  TranscriptRenderer(std::ostream& out, TerminalCapabilities capabilities,
                     WidthQuery width_query = {});
  TranscriptRenderer(const TranscriptRenderer&) = delete;
  TranscriptRenderer& operator=(const TranscriptRenderer&) = delete;
  TranscriptRenderer(TranscriptRenderer&&) = delete;
  TranscriptRenderer& operator=(TranscriptRenderer&&) = delete;
  ~TranscriptRenderer() noexcept;

  void showThinking();
  void finishThinking(std::chrono::steady_clock::duration elapsed);
  void clearActivity() noexcept;
  void renderAssistant(std::string_view markdown);
  void finishTurn() noexcept;

 private:
  std::optional<std::size_t> currentWidth() const;
  void writeActivity(std::string_view text, bool permanent);

  std::ostream& out_;
  TerminalCapabilities capabilities_;
  WidthQuery width_query_;
  bool activity_visible_ = false;
};

}  // namespace octopus
