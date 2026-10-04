#pragma once

#include <cstddef>
#include <iosfwd>
#include <string>
#include <string_view>

namespace octopus {

enum class TerminalInputEventType {
  None,
  Submitted,
  EndOfFile,
  Cancelled,
};

struct TerminalInputEvent {
  TerminalInputEventType type = TerminalInputEventType::None;
  std::string text;
};

// A byte-storing line editor that validates UTF-8 and keeps cursor/edit
// operations on code-point boundaries.
class TerminalLineEditor {
 public:
  TerminalInputEvent feed(char byte);

  const std::string& currentLine() const noexcept { return line_; }
  std::size_t cursor() const noexcept { return cursor_; }
  bool hasPendingInput() const noexcept { return !pending_.empty(); }
  bool hasPendingUtf8() const noexcept { return !utf8_pending_.empty(); }
  bool isParsingEscape() const noexcept {
    return escape_state_ != EscapeState::None;
  }
  bool isPasting() const noexcept { return paste_mode_; }

 private:
  enum class EscapeState {
    None,
    Escape,
    Ss3,
    ControlSequence,
    DeleteSequence,
    OperatingSystemCommand,
    OperatingSystemCommandEscape,
  };

  TerminalInputEvent handleEnter();
  void insertPrintable(char byte);
  void insertText(std::string_view text);
  void acceptUtf8Byte(char byte);
  void backspace();
  void deleteAtCursor();
  void deleteWordLeft();
  void deleteWordRight();
  void moveLeft();
  void moveRight();
  void moveWordLeft();
  void moveWordRight();
  void insertLineBreak();
  TerminalInputEvent dispatchControlSequence(char final_byte);
  void resetEscape() noexcept;

  std::string line_;
  std::string pending_;
  std::string csi_parameters_;
  std::string utf8_pending_;
  std::size_t utf8_expected_ = 0;
  std::size_t cursor_ = 0;
  EscapeState escape_state_ = EscapeState::None;
  bool paste_mode_ = false;
};

struct TerminalScreenPosition {
  std::size_t row = 0;
  std::size_t column = 0;
};

struct TerminalInputMetrics {
  std::size_t rows = 1;
  TerminalScreenPosition cursor;
  TerminalScreenPosition end;
};

// Pure display-cell layout used by the raw editor. Cursor offsets remain byte
// offsets but must be at UTF-8 code-point boundaries.
TerminalInputMetrics measureTerminalInput(std::string_view prompt,
                                          std::string_view continuation_prompt,
                                          std::string_view line,
                                          std::size_t cursor,
                                          std::size_t columns) noexcept;

enum class TerminalReadStatus {
  Submitted,
  EndOfFile,
  Cancelled,
};

struct TerminalReadResult {
  TerminalReadStatus status = TerminalReadStatus::EndOfFile;
  std::string text;
};

// Low-level terminal input reader used by InputEditor. Real TTY stdin/stdout
// uses raw-mode editing; all other streams use cooked std::getline semantics.
TerminalReadResult readTerminalInput(std::istream& in, std::ostream& out,
                                     std::string_view prompt,
                                     std::string_view continuation_prompt);

}  // namespace octopus
