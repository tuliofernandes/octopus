#pragma once

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iosfwd>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace octopus {

struct TerminalRgb {
  std::uint8_t red = 0;
  std::uint8_t green = 0;
  std::uint8_t blue = 0;
};

inline bool operator==(TerminalRgb left, TerminalRgb right) noexcept {
  return left.red == right.red && left.green == right.green &&
         left.blue == right.blue;
}

struct TerminalCapabilities {
  TerminalCapabilities() = default;
  TerminalCapabilities(bool interactive, bool cursor, bool erase, bool sgr,
                       bool use_color, bool use_utf8,
                       std::optional<std::size_t> terminal_columns) noexcept
      : interactive_session(interactive),
        cursor_control(cursor),
        erase_line(erase),
        sgr_attributes(sgr),
        color(use_color),
        utf8(use_utf8),
        columns(terminal_columns) {}

  bool interactive_session = false;
  bool cursor_control = false;
  bool erase_line = false;
  bool sgr_attributes = false;
  bool color = false;
  bool utf8 = false;
  std::optional<std::size_t> columns;
  std::optional<TerminalRgb> response_background;
  bool truecolor = false;
};

TerminalCapabilities makeTerminalCapabilities(
    bool input_is_tty, bool output_is_tty, std::string_view term,
    std::optional<std::string_view> no_color, bool utf8,
    std::optional<std::size_t> columns, bool truecolor = false,
    std::optional<TerminalRgb> terminal_background = std::nullopt) noexcept;
TerminalCapabilities detectTerminalCapabilities(std::istream& in,
                                                std::ostream& out);
std::optional<std::size_t> queryTerminalWidth(int fd) noexcept;
std::optional<TerminalRgb> queryTerminalBackground(int input_fd,
                                                   int output_fd) noexcept;
std::optional<TerminalRgb> parseTerminalBackgroundResponse(
    std::string_view response) noexcept;
TerminalRgb lightenTerminalBackground(TerminalRgb background) noexcept;

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
  TerminalEncodingPolicy(
      bool use_sgr_attributes = false, bool use_color = false,
      std::optional<TerminalRgb> response_background = std::nullopt,
      bool use_truecolor = false, bool use_erase_line = false) noexcept
      : sgr_attributes(use_sgr_attributes),
        color(use_color),
        background(response_background),
        truecolor(use_truecolor),
        erase_line(use_erase_line) {}

  bool sgr_attributes = false;
  bool color = false;
  std::optional<TerminalRgb> background;
  bool truecolor = false;
  bool erase_line = false;
};

std::optional<TerminalDocument> parseMarkdown(std::string_view markdown);
std::string encodeTerminalDocument(const TerminalDocument& document,
                                   TerminalEncodingPolicy policy);

class TranscriptRenderer final {
 public:
  using WidthQuery = std::function<std::optional<std::size_t>()>;

  TranscriptRenderer(std::ostream& out, TerminalCapabilities capabilities,
                     WidthQuery width_query = {},
                     std::chrono::milliseconds spinner_interval =
                         std::chrono::milliseconds{80});
  TranscriptRenderer(const TranscriptRenderer&) = delete;
  TranscriptRenderer& operator=(const TranscriptRenderer&) = delete;
  TranscriptRenderer(TranscriptRenderer&&) = delete;
  TranscriptRenderer& operator=(TranscriptRenderer&&) = delete;
  ~TranscriptRenderer() noexcept;

  void showThinking();
  void finishThinking(std::chrono::steady_clock::duration elapsed);
  void startSpinner();
  void stopSpinner() noexcept;
  void clearActivity() noexcept;
  void renderAssistant(std::string_view markdown);
  void finishTurn() noexcept;

 private:
  std::optional<std::size_t> currentWidth() const;
  void writeActivity(std::string_view text, bool permanent);
  void writeActivityLocked(std::string_view text, bool permanent);
  void clearActivityLocked() noexcept;
  void runSpinner() noexcept;

  std::ostream& out_;
  TerminalCapabilities capabilities_;
  WidthQuery width_query_;
  std::chrono::milliseconds spinner_interval_;
  std::mutex output_mutex_;
  std::condition_variable spinner_wakeup_;
  std::thread spinner_thread_;
  bool spinner_stop_requested_ = false;
  bool activity_visible_ = false;
};

}  // namespace octopus
