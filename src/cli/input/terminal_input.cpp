#include "octopus/cli/terminal_input.hpp"

#include <cerrno>
#include <cctype>
#include <cstdlib>
#include <iostream>
#include <istream>
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
constexpr std::string_view kQuietUserTextStyle = "\x1b[2m";
constexpr std::string_view kResetStyle = "\x1b[0m";
bool endsWithContinuation(const std::string& line) {
  return !line.empty() && line.back() == '\\';
}

void appendLine(std::string& pending, const std::string& line) {
  if (!pending.empty()) {
    pending.push_back('\n');
  }
  pending += line;
}

bool isInteractiveTerminal(const std::istream& in, const std::ostream& out) {
  return &in == &std::cin && &out == &std::cout && isatty(STDIN_FILENO) != 0 &&
         isatty(STDOUT_FILENO) != 0;
}

bool hasNoColorEnvironment() { return std::getenv("NO_COLOR") != nullptr; }

std::string environmentValue(const char* name) {
  const char* value = std::getenv(name);
  if (value == nullptr) {
    return "";
  }
  return value;
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

  ~ScopedBracketedPaste() {
    out_ << "\x1b[?2004l";
    out_.flush();
  }

 private:
  std::ostream& out_;
};

struct ScreenPosition {
  std::size_t row = 0;
  std::size_t column = 0;
};

struct RenderMetrics {
  std::size_t rows = 1;
  ScreenPosition cursor;
  ScreenPosition end;
};

void advancePosition(ScreenPosition& position, char byte,
                     std::string_view continuation_prompt) {
  if (byte == '\n') {
    ++position.row;
    position.column = continuation_prompt.size();
    return;
  }
  ++position.column;
}

RenderMetrics measureInput(std::string_view prompt,
                           std::string_view continuation_prompt,
                           const TerminalLineEditor& editor) {
  RenderMetrics metrics;
  ScreenPosition position{0, prompt.size()};
  const auto& line = editor.currentLine();

  for (std::size_t index = 0; index < line.size(); ++index) {
    if (index == editor.cursor()) {
      metrics.cursor = position;
    }
    advancePosition(position, line[index], continuation_prompt);
  }
  if (editor.cursor() == line.size()) {
    metrics.cursor = position;
  }
  metrics.end = position;
  metrics.rows = position.row + 1;
  return metrics;
}

void writeStyledInput(std::ostream& out, std::string_view prompt,
                      std::string_view continuation_prompt,
                      const TerminalLineEditor& editor,
                      const TerminalStylePolicy& style) {
  out << style.user_start;
  out << prompt;
  const auto& line = editor.currentLine();
  for (const char byte : line) {
    if (byte == '\n') {
      out << '\n' << continuation_prompt;
      continue;
    }
    out << byte;
  }
  out << style.user_end;
}

std::size_t redrawInput(std::ostream& out, std::string_view prompt,
                        std::string_view continuation_prompt,
                        const TerminalLineEditor& editor,
                        std::size_t previous_rows,
                        const TerminalStylePolicy& style) {
  if (previous_rows > 1) {
    out << "\r\x1b[" << (previous_rows - 1) << "A";
  } else {
    out << '\r';
  }

  out << "\x1b[J";
  writeStyledInput(out, prompt, continuation_prompt, editor, style);

  const auto metrics = measureInput(prompt, continuation_prompt, editor);
  if (metrics.end.row > metrics.cursor.row) {
    out << "\x1b[" << (metrics.end.row - metrics.cursor.row) << "A";
  }
  out << '\r';
  if (metrics.cursor.column > 0) {
    out << "\x1b[" << metrics.cursor.column << "C";
  }
  out.flush();
  return metrics.rows;
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
                                std::string_view continuation_prompt,
                                const TerminalStylePolicy& style) {
  ScopedRawTerminal terminal(STDIN_FILENO);
  if (!terminal.active()) {
    return {TerminalReadStatus::EndOfFile, ""};
  }

  ScopedBracketedPaste bracketed_paste(out);
  TerminalLineEditor editor;
  std::size_t rendered_rows = 1;
  out << style.user_start << prompt << style.user_end;
  out.flush();

  while (true) {
    char byte = '\0';
    const ssize_t read_count = read(STDIN_FILENO, &byte, 1);
    if (read_count == 0) {
      out << style.user_end << '\n';
      return {TerminalReadStatus::EndOfFile, ""};
    }
    if (read_count < 0) {
      if (errno == EINTR) {
        continue;
      }
      out << style.user_end << '\n';
      return {TerminalReadStatus::EndOfFile, ""};
    }

    const bool was_pasting = editor.isPasting();
    const auto event = editor.feed(byte);
    switch (event.type) {
      case TerminalInputEventType::Submitted:
        out << style.user_end << '\n';
        return {TerminalReadStatus::Submitted, event.text};
      case TerminalInputEventType::EndOfFile:
        out << style.user_end << '\n';
        return {TerminalReadStatus::EndOfFile, ""};
      case TerminalInputEventType::Cancelled:
        out << style.user_end << "^C\n";
        return {TerminalReadStatus::Cancelled, ""};
      case TerminalInputEventType::None:
        break;
    }

    if (was_pasting || editor.isPasting()) {
      if (was_pasting && !editor.isPasting()) {
        rendered_rows = redrawInput(out, prompt, continuation_prompt, editor,
                                    rendered_rows, style);
      }
      continue;
    }

    if (editor.isParsingEscape()) {
      continue;
    }

    rendered_rows = redrawInput(out, prompt, continuation_prompt, editor,
                                rendered_rows, style);
  }
}

}  // namespace

TerminalStylePolicy makeTerminalStylePolicy(bool is_interactive_tty,
                                            bool no_color,
                                            std::string_view term) noexcept {
  if (!is_interactive_tty || no_color || term == "dumb") {
    return {};
  }
  return {kQuietUserTextStyle, kResetStyle};
}

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
    case EscapeState::None:
      break;
  }

  if (byte == kEscape) {
    escape_state_ = EscapeState::Escape;
    return {};
  }
  if (byte == kCtrlC) {
    line_.clear();
    pending_.clear();
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
    insertPrintable(byte);
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
  }

  return {};
}

TerminalInputEvent TerminalLineEditor::handleEnter() {
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

void TerminalLineEditor::backspace() {
  if (cursor_ == 0) {
    return;
  }
  line_.erase(line_.begin() + static_cast<std::ptrdiff_t>(cursor_ - 1));
  --cursor_;
}

void TerminalLineEditor::deleteAtCursor() {
  if (cursor_ >= line_.size()) {
    return;
  }
  line_.erase(line_.begin() + static_cast<std::ptrdiff_t>(cursor_));
}

void TerminalLineEditor::deleteWordLeft() {
  const std::size_t original_cursor = cursor_;
  while (cursor_ > 0 &&
         std::isspace(static_cast<unsigned char>(line_[cursor_ - 1])) != 0) {
    --cursor_;
  }
  while (cursor_ > 0 &&
         std::isspace(static_cast<unsigned char>(line_[cursor_ - 1])) == 0) {
    --cursor_;
  }
  line_.erase(cursor_, original_cursor - cursor_);
}

void TerminalLineEditor::deleteWordRight() {
  std::size_t end = cursor_;
  while (end < line_.size() &&
         std::isspace(static_cast<unsigned char>(line_[end])) != 0) {
    ++end;
  }
  while (end < line_.size() &&
         std::isspace(static_cast<unsigned char>(line_[end])) == 0) {
    ++end;
  }
  line_.erase(cursor_, end - cursor_);
}

void TerminalLineEditor::insertLineBreak() {
  line_.insert(line_.begin() + static_cast<std::ptrdiff_t>(cursor_), '\n');
  ++cursor_;
}

void TerminalLineEditor::moveLeft() {
  if (cursor_ > 0) {
    --cursor_;
  }
}

void TerminalLineEditor::moveRight() {
  if (cursor_ < line_.size()) {
    ++cursor_;
  }
}

void TerminalLineEditor::moveWordLeft() {
  while (cursor_ > 0 &&
         std::isspace(static_cast<unsigned char>(line_[cursor_ - 1])) != 0) {
    --cursor_;
  }
  while (cursor_ > 0 &&
         std::isspace(static_cast<unsigned char>(line_[cursor_ - 1])) == 0) {
    --cursor_;
  }
}

void TerminalLineEditor::moveWordRight() {
  while (cursor_ < line_.size() &&
         std::isspace(static_cast<unsigned char>(line_[cursor_])) == 0) {
    ++cursor_;
  }
  while (cursor_ < line_.size() &&
         std::isspace(static_cast<unsigned char>(line_[cursor_])) != 0) {
    ++cursor_;
  }
}

void TerminalLineEditor::resetEscape() noexcept {
  escape_state_ = EscapeState::None;
  csi_parameters_.clear();
}

TerminalReadResult readTerminalInput(std::istream& in, std::ostream& out,
                                     std::string_view prompt,
                                     std::string_view continuation_prompt) {
  if (isInteractiveTerminal(in, out)) {
    const std::string term = environmentValue("TERM");
    const TerminalStylePolicy style =
        makeTerminalStylePolicy(true, hasNoColorEnvironment(), term);
    return readRawInput(out, prompt, continuation_prompt, style);
  }
  return readCookedInput(in, out, prompt);
}

}  // namespace octopus
