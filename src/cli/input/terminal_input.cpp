#include "octopus/cli/terminal_input.hpp"

#include "octopus/cli/terminal_renderer.hpp"

#include "cli/terminal/text.hpp"

#include <algorithm>
#include <cerrno>
#include <cctype>
#include <istream>
#include <limits>
#include <ostream>
#include <string>

#include <termios.h>
#include <unistd.h>

namespace octopus {
namespace {

constexpr char kEscape = '\x1b';
constexpr char kCtrlC = '\x03';
constexpr char kCtrlD = '\x04';
constexpr char kCtrlW = '\x17';
constexpr char kBackspace = '\x08';
constexpr char kDelete = '\x7f';
bool endsWithContinuation(const std::string& line) {
  return !line.empty() && line.back() == '\\';
}

void appendLine(std::string& pending, const std::string& line) {
  if (!pending.empty()) {
    pending.push_back('\n');
  }
  pending += line;
}

class ScopedRawTerminal final {
 public:
  explicit ScopedRawTerminal(int fd) : fd_(fd) {
    if (tcgetattr(fd_, &original_) != 0) {
      return;
    }

    termios raw = original_;
    raw.c_lflag &= static_cast<tcflag_t>(~(ICANON | ECHO | ISIG));
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    active_ = tcsetattr(fd_, TCSAFLUSH, &raw) == 0;
  }

  ScopedRawTerminal(const ScopedRawTerminal&) = delete;
  ScopedRawTerminal& operator=(const ScopedRawTerminal&) = delete;

  ~ScopedRawTerminal() {
    if (active_) {
      tcsetattr(fd_, TCSAFLUSH, &original_);
    }
  }

  bool active() const noexcept { return active_; }

 private:
  int fd_;
  termios original_{};
  bool active_ = false;
};

class ScopedBracketedPaste final {
 public:
  explicit ScopedBracketedPaste(std::ostream& out) : out_(out) {
    out_ << "\x1b[?2004h";
    out_.flush();
  }

  ScopedBracketedPaste(const ScopedBracketedPaste&) = delete;
  ScopedBracketedPaste& operator=(const ScopedBracketedPaste&) = delete;

  ~ScopedBracketedPaste() noexcept {
    try {
      out_ << "\x1b[?2004l";
      out_.flush();
    } catch (...) {
    }
  }

 private:
  std::ostream& out_;
};

void writeInput(std::ostream& out, std::string_view prompt,
                std::string_view continuation_prompt,
                const TerminalLineEditor& editor) {
  out << prompt;
  const auto& line = editor.currentLine();
  for (const char byte : line) {
    if (byte == '\n') {
      out << '\n' << continuation_prompt;
      continue;
    }
    out << byte;
  }
}

TerminalInputMetrics redrawInput(std::ostream& out, std::string_view prompt,
                                 std::string_view continuation_prompt,
                                 const TerminalLineEditor& editor,
                                 TerminalInputMetrics previous_metrics) {
  if (previous_metrics.cursor.row > 0) {
    out << "\r\x1b[" << previous_metrics.cursor.row << "A";
  } else {
    out << '\r';
  }

  out << "\x1b[J";
  writeInput(out, prompt, continuation_prompt, editor);

  const std::size_t columns =
      queryTerminalWidth(STDOUT_FILENO)
          .value_or(std::numeric_limits<std::size_t>::max());
  const auto metrics =
      measureTerminalInput(prompt, continuation_prompt, editor.currentLine(),
                           editor.cursor(), columns);
  if (metrics.end.row > metrics.cursor.row) {
    out << "\x1b[" << (metrics.end.row - metrics.cursor.row) << "A";
  }
  out << '\r';
  if (metrics.cursor.column > 0) {
    out << "\x1b[" << metrics.cursor.column << "C";
  }
  out.flush();
  return metrics;
}

TerminalReadResult readCookedInput(std::istream& in, std::ostream& out,
                                   std::string_view prompt) {
  out << prompt;
  out.flush();

  TerminalReadResult result;
  std::string line;
  while (std::getline(in, line)) {
    if (endsWithContinuation(line)) {
      line.pop_back();
      appendLine(result.text, line);
      continue;
    }

    appendLine(result.text, line);
    result.status = TerminalReadStatus::Submitted;
    return result;
  }

  if (!result.text.empty()) {
    result.status = TerminalReadStatus::Submitted;
  }
  return result;
}

TerminalReadResult readRawInput(std::ostream& out, std::string_view prompt,
                                std::string_view continuation_prompt) {
  ScopedRawTerminal terminal(STDIN_FILENO);
  if (!terminal.active()) {
    return {TerminalReadStatus::EndOfFile, ""};
  }

  ScopedBracketedPaste bracketed_paste(out);
  TerminalLineEditor editor;
  const std::size_t initial_columns =
      queryTerminalWidth(STDOUT_FILENO)
          .value_or(std::numeric_limits<std::size_t>::max());
  TerminalInputMetrics rendered =
      measureTerminalInput(prompt, continuation_prompt, editor.currentLine(),
                           editor.cursor(), initial_columns);
  out << prompt;
  out.flush();

  while (true) {
    char byte = '\0';
    const ssize_t read_count = read(STDIN_FILENO, &byte, 1);
    if (read_count == 0) {
      out << '\n';
      return {TerminalReadStatus::EndOfFile, ""};
    }
    if (read_count < 0) {
      if (errno == EINTR) {
        continue;
      }
      out << '\n';
      return {TerminalReadStatus::EndOfFile, ""};
    }

    const bool was_pasting = editor.isPasting();
    const auto event = editor.feed(byte);
    switch (event.type) {
      case TerminalInputEventType::Submitted:
        out << '\n';
        return {TerminalReadStatus::Submitted, event.text};
      case TerminalInputEventType::EndOfFile:
        out << '\n';
        return {TerminalReadStatus::EndOfFile, ""};
      case TerminalInputEventType::Cancelled:
        out << "^C\n";
        return {TerminalReadStatus::Cancelled, ""};
      case TerminalInputEventType::None:
        break;
    }

    if (was_pasting || editor.isPasting()) {
      if (was_pasting && !editor.isPasting()) {
        rendered =
            redrawInput(out, prompt, continuation_prompt, editor, rendered);
      }
      continue;
    }

    if (editor.isParsingEscape()) {
      continue;
    }

    rendered = redrawInput(out, prompt, continuation_prompt, editor, rendered);
  }
}

}  // namespace

TerminalInputEvent TerminalLineEditor::feed(char byte) {
  const unsigned char unsigned_byte = static_cast<unsigned char>(byte);

  switch (escape_state_) {
    case EscapeState::Escape:
      if (byte == '[') {
        escape_state_ = EscapeState::ControlSequence;
        csi_parameters_.clear();
        return {};
      }
      if (byte == 'O') {
        escape_state_ = EscapeState::Ss3;
        return {};
      }
      if (byte == ']') {
        escape_state_ = EscapeState::OperatingSystemCommand;
        return {};
      }
      if (byte == 'b' || byte == 'B') {
        moveWordLeft();
        resetEscape();
        return {};
      }
      if (byte == 'f' || byte == 'F') {
        moveWordRight();
        resetEscape();
        return {};
      }
      if (byte == 'd' || byte == 'D') {
        deleteWordRight();
        resetEscape();
        return {};
      }
      if (byte == kBackspace || byte == kDelete) {
        deleteWordLeft();
        resetEscape();
        return {};
      }
      resetEscape();
      return {};
    case EscapeState::Ss3:
      switch (byte) {
        case 'M':
          insertLineBreak();
          break;
        case 'C':
          moveRight();
          break;
        case 'D':
          moveLeft();
          break;
        default:
          break;
      }
      resetEscape();
      return {};
    case EscapeState::ControlSequence:
      if ((byte >= '0' && byte <= '9') || byte == ';' || byte == '?' ||
          byte == ':') {
        csi_parameters_.push_back(byte);
        return {};
      }
      if (byte >= '@' && byte <= '~') {
        return dispatchControlSequence(byte);
      }
      resetEscape();
      return {};
    case EscapeState::DeleteSequence:
      if (byte == '~') {
        deleteAtCursor();
      }
      resetEscape();
      return {};
    case EscapeState::OperatingSystemCommand:
      if (byte == '\x07') {
        resetEscape();
      } else if (byte == kEscape) {
        escape_state_ = EscapeState::OperatingSystemCommandEscape;
      }
      return {};
    case EscapeState::OperatingSystemCommandEscape:
      if (byte == '\\') {
        resetEscape();
      } else if (byte != kEscape) {
        escape_state_ = EscapeState::OperatingSystemCommand;
      }
      return {};
    case EscapeState::None:
      break;
  }

  if (!utf8_pending_.empty()) {
    if ((unsigned_byte & 0xC0U) == 0x80U) {
      utf8_pending_.push_back(byte);
      if (utf8_pending_.size() == utf8_expected_) {
        const auto decoded = cli_detail::decodeCodePoint(utf8_pending_, 0);
        if (decoded.valid && decoded.bytes == utf8_pending_.size()) {
          insertText(utf8_pending_);
        }
        utf8_pending_.clear();
        utf8_expected_ = 0;
      }
      return {};
    }
    utf8_pending_.clear();
    utf8_expected_ = 0;
  }

  if (byte == kEscape) {
    escape_state_ = EscapeState::Escape;
    return {};
  }
  if (byte == kCtrlC) {
    line_.clear();
    pending_.clear();
    utf8_pending_.clear();
    utf8_expected_ = 0;
    cursor_ = 0;
    return {TerminalInputEventType::Cancelled, ""};
  }
  if (byte == kCtrlD) {
    if (line_.empty() && pending_.empty()) {
      return {TerminalInputEventType::EndOfFile, ""};
    }
    return {};
  }
  if (byte == '\r' || byte == '\n') {
    if (paste_mode_) {
      insertLineBreak();
      return {};
    }
    return handleEnter();
  }
  if (paste_mode_) {
    if (byte == '\t' || (unsigned_byte >= 0x20U && unsigned_byte < 0x7FU)) {
      insertPrintable(byte);
    } else if (unsigned_byte >= 0x80U) {
      acceptUtf8Byte(byte);
    }
    return {};
  }
  if (byte == kCtrlW) {
    deleteWordLeft();
    return {};
  }
  if (byte == kBackspace || byte == kDelete) {
    backspace();
    return {};
  }
  if (std::isprint(unsigned_byte) != 0 && unsigned_byte < 128) {
    insertPrintable(byte);
  } else if (unsigned_byte >= 0x80U) {
    acceptUtf8Byte(byte);
  }

  return {};
}

TerminalInputEvent TerminalLineEditor::handleEnter() {
  utf8_pending_.clear();
  utf8_expected_ = 0;
  if (endsWithContinuation(line_)) {
    line_.pop_back();
    appendLine(pending_, line_);
    line_.clear();
    cursor_ = 0;
    return {};
  }

  appendLine(pending_, line_);
  TerminalInputEvent event;
  event.type = TerminalInputEventType::Submitted;
  event.text = std::move(pending_);
  pending_.clear();
  line_.clear();
  cursor_ = 0;
  return event;
}

TerminalInputEvent TerminalLineEditor::dispatchControlSequence(
    char final_byte) {
  const bool modified_word_motion =
      csi_parameters_.find(";3") != std::string::npos ||
      csi_parameters_.find(";5") != std::string::npos ||
      csi_parameters_ == "3" || csi_parameters_ == "5";
  const bool alt_or_ctrl_modified =
      csi_parameters_.find(";3") != std::string::npos ||
      csi_parameters_.find(";5") != std::string::npos;

  if (final_byte == '~') {
    if (csi_parameters_ == "3") {
      deleteAtCursor();
    } else if (csi_parameters_.find("3;3") == 0 ||
               csi_parameters_.find("3;5") == 0) {
      deleteWordRight();
    } else if (csi_parameters_ == "200") {
      paste_mode_ = true;
    } else if (csi_parameters_ == "201") {
      paste_mode_ = false;
    } else if (csi_parameters_ == "13;2") {
      insertLineBreak();
    }
    resetEscape();
    return {};
  }

  switch (final_byte) {
    case 'A':
    case 'B':
      break;
    case 'C':
      if (modified_word_motion) {
        moveWordRight();
      } else {
        moveRight();
      }
      break;
    case 'D':
      if (modified_word_motion) {
        moveWordLeft();
      } else {
        moveLeft();
      }
      break;
    case 'M':
      if (csi_parameters_.empty() || csi_parameters_ == "13;2") {
        insertLineBreak();
      }
      break;
    case 'u':
      if (csi_parameters_ == "13;2") {
        insertLineBreak();
      } else if ((csi_parameters_.find("127;") == 0 ||
                  csi_parameters_.find("8;") == 0) &&
                 alt_or_ctrl_modified) {
        deleteWordLeft();
      } else if (csi_parameters_.find("3;") == 0 && alt_or_ctrl_modified) {
        deleteWordRight();
      }
      break;
    default:
      break;
  }

  resetEscape();
  return {};
}

void TerminalLineEditor::insertPrintable(char byte) {
  line_.insert(line_.begin() + static_cast<std::ptrdiff_t>(cursor_), byte);
  ++cursor_;
}

void TerminalLineEditor::insertText(std::string_view text) {
  line_.insert(cursor_, text);
  cursor_ += text.size();
}

void TerminalLineEditor::acceptUtf8Byte(char byte) {
  const auto first = static_cast<unsigned char>(byte);
  if (first >= 0xC2U && first <= 0xDFU) {
    utf8_expected_ = 2;
  } else if (first >= 0xE0U && first <= 0xEFU) {
    utf8_expected_ = 3;
  } else if (first >= 0xF0U && first <= 0xF4U) {
    utf8_expected_ = 4;
  } else {
    return;
  }
  utf8_pending_.assign(1, byte);
}

void TerminalLineEditor::backspace() {
  if (cursor_ == 0) {
    return;
  }
  const std::size_t previous =
      cli_detail::previousCodePointBoundary(line_, cursor_);
  line_.erase(previous, cursor_ - previous);
  cursor_ = previous;
}

void TerminalLineEditor::deleteAtCursor() {
  if (cursor_ >= line_.size()) {
    return;
  }
  const std::size_t next = cli_detail::nextCodePointBoundary(line_, cursor_);
  line_.erase(cursor_, next - cursor_);
}

void TerminalLineEditor::deleteWordLeft() {
  const std::size_t original_cursor = cursor_;
  while (cursor_ > 0) {
    const std::size_t previous =
        cli_detail::previousCodePointBoundary(line_, cursor_);
    const auto decoded = cli_detail::decodeCodePoint(line_, previous);
    if (!decoded.valid || decoded.value > 0x7FU ||
        std::isspace(static_cast<unsigned char>(decoded.value)) == 0) {
      break;
    }
    cursor_ = previous;
  }
  while (cursor_ > 0) {
    const std::size_t previous =
        cli_detail::previousCodePointBoundary(line_, cursor_);
    const auto decoded = cli_detail::decodeCodePoint(line_, previous);
    if (decoded.valid && decoded.value <= 0x7FU &&
        std::isspace(static_cast<unsigned char>(decoded.value)) != 0) {
      break;
    }
    cursor_ = previous;
  }
  line_.erase(cursor_, original_cursor - cursor_);
}

void TerminalLineEditor::deleteWordRight() {
  std::size_t end = cursor_;
  while (end < line_.size()) {
    const auto decoded = cli_detail::decodeCodePoint(line_, end);
    if (!decoded.valid || decoded.value > 0x7FU ||
        std::isspace(static_cast<unsigned char>(decoded.value)) == 0) {
      break;
    }
    end = cli_detail::nextCodePointBoundary(line_, end);
  }
  while (end < line_.size()) {
    const auto decoded = cli_detail::decodeCodePoint(line_, end);
    if (decoded.valid && decoded.value <= 0x7FU &&
        std::isspace(static_cast<unsigned char>(decoded.value)) != 0) {
      break;
    }
    end = cli_detail::nextCodePointBoundary(line_, end);
  }
  line_.erase(cursor_, end - cursor_);
}

void TerminalLineEditor::insertLineBreak() {
  line_.insert(line_.begin() + static_cast<std::ptrdiff_t>(cursor_), '\n');
  ++cursor_;
}

void TerminalLineEditor::moveLeft() {
  if (cursor_ > 0) {
    cursor_ = cli_detail::previousCodePointBoundary(line_, cursor_);
  }
}

void TerminalLineEditor::moveRight() {
  if (cursor_ < line_.size()) {
    cursor_ = cli_detail::nextCodePointBoundary(line_, cursor_);
  }
}

void TerminalLineEditor::moveWordLeft() {
  while (cursor_ > 0) {
    const std::size_t previous =
        cli_detail::previousCodePointBoundary(line_, cursor_);
    const auto decoded = cli_detail::decodeCodePoint(line_, previous);
    if (!decoded.valid || decoded.value > 0x7FU ||
        std::isspace(static_cast<unsigned char>(decoded.value)) == 0) {
      break;
    }
    cursor_ = previous;
  }
  while (cursor_ > 0) {
    const std::size_t previous =
        cli_detail::previousCodePointBoundary(line_, cursor_);
    const auto decoded = cli_detail::decodeCodePoint(line_, previous);
    if (decoded.valid && decoded.value <= 0x7FU &&
        std::isspace(static_cast<unsigned char>(decoded.value)) != 0) {
      break;
    }
    cursor_ = previous;
  }
}

void TerminalLineEditor::moveWordRight() {
  while (cursor_ < line_.size()) {
    const auto decoded = cli_detail::decodeCodePoint(line_, cursor_);
    if (decoded.valid && decoded.value <= 0x7FU &&
        std::isspace(static_cast<unsigned char>(decoded.value)) != 0) {
      break;
    }
    cursor_ = cli_detail::nextCodePointBoundary(line_, cursor_);
  }
  while (cursor_ < line_.size()) {
    const auto decoded = cli_detail::decodeCodePoint(line_, cursor_);
    if (!decoded.valid || decoded.value > 0x7FU ||
        std::isspace(static_cast<unsigned char>(decoded.value)) == 0) {
      break;
    }
    cursor_ = cli_detail::nextCodePointBoundary(line_, cursor_);
  }
}

void TerminalLineEditor::resetEscape() noexcept {
  escape_state_ = EscapeState::None;
  csi_parameters_.clear();
}

TerminalReadResult readTerminalInput(std::istream& in, std::ostream& out,
                                     std::string_view prompt,
                                     std::string_view continuation_prompt) {
  const TerminalCapabilities capabilities = detectTerminalCapabilities(in, out);
  if (capabilities.cursor_control && capabilities.columns.has_value()) {
    return readRawInput(out, prompt, continuation_prompt);
  }
  return readCookedInput(in, out, prompt);
}

TerminalInputMetrics measureTerminalInput(std::string_view prompt,
                                          std::string_view continuation_prompt,
                                          std::string_view line,
                                          std::size_t cursor,
                                          std::size_t columns) noexcept {
  TerminalInputMetrics metrics;
  TerminalScreenPosition position;
  const std::size_t safe_columns = std::max<std::size_t>(columns, 1);

  const auto advanceCells = [&](std::size_t cells,
                                TerminalScreenPosition& target) {
    while (cells > 0) {
      if (target.column == safe_columns) {
        ++target.row;
        target.column = 0;
      }
      const std::size_t available = safe_columns - target.column;
      const std::size_t advanced = std::min(cells, available);
      target.column += advanced;
      cells -= advanced;
    }
  };

  for (std::size_t offset = 0; offset < prompt.size();) {
    const auto decoded = cli_detail::decodeCodePoint(prompt, offset);
    advanceCells(
        decoded.valid ? cli_detail::terminalCellWidth(decoded.value) : 1,
        position);
    offset += decoded.valid ? decoded.bytes : 1;
  }

  const std::size_t safe_cursor = std::min(cursor, line.size());
  for (std::size_t offset = 0; offset < line.size();) {
    if (offset == safe_cursor) {
      metrics.cursor = position;
    }
    const auto decoded = cli_detail::decodeCodePoint(line, offset);
    if (decoded.valid && decoded.value == '\n') {
      ++position.row;
      position.column = 0;
      for (std::size_t prompt_offset = 0;
           prompt_offset < continuation_prompt.size();) {
        const auto prompt_code_point =
            cli_detail::decodeCodePoint(continuation_prompt, prompt_offset);
        advanceCells(prompt_code_point.valid ? cli_detail::terminalCellWidth(
                                                   prompt_code_point.value)
                                             : 1,
                     position);
        prompt_offset += prompt_code_point.valid ? prompt_code_point.bytes : 1;
      }
    } else if (decoded.valid && decoded.value == '\t') {
      const std::size_t tab_width = 8 - (position.column % 8);
      advanceCells(tab_width, position);
    } else {
      advanceCells(
          decoded.valid ? cli_detail::terminalCellWidth(decoded.value) : 1,
          position);
    }
    offset += decoded.valid ? decoded.bytes : 1;
  }
  if (safe_cursor == line.size()) {
    metrics.cursor = position;
  }
  metrics.end = position;
  metrics.rows = position.row + 1;
  return metrics;
}

}  // namespace octopus
