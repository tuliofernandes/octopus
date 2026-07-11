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

// A byte-oriented line editor for interactive terminal input. It deliberately
// starts with ASCII editing; UTF-8 display width can be added as a later
// policy.
class TerminalLineEditor {
 public:
  TerminalInputEvent feed(char byte);

  const std::string& currentLine() const noexcept { return line_; }
  std::size_t cursor() const noexcept { return cursor_; }
  bool hasPendingInput() const noexcept { return !pending_.empty(); }
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
  };

  TerminalInputEvent handleEnter();
  void insertPrintable(char byte);
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
  std::size_t cursor_ = 0;
  EscapeState escape_state_ = EscapeState::None;
  bool paste_mode_ = false;
};

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
