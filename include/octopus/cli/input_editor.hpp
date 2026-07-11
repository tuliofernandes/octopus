#pragma once

#include <iosfwd>
#include <optional>
#include <string>

namespace octopus {

struct InputEditorPrompts {
  std::string primary;
  std::string continuation;
};

enum class InputEditorStatus {
  Submitted,
  EndOfFile,
  Cancelled,
};

struct InputEditorResult {
  InputEditorStatus status = InputEditorStatus::EndOfFile;
  std::string text;
};

// Public REPL input facade. It owns prompt policy and hides terminal editing
// details from chat/session callers.
class InputEditor {
 public:
  InputEditor(std::istream& in, std::ostream& out, InputEditorPrompts prompts);

  InputEditorResult read();
  std::optional<std::string> readText();

 private:
  std::istream& in_;
  std::ostream& out_;
  InputEditorPrompts prompts_;
};

}  // namespace octopus
