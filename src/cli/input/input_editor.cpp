#include "octopus/cli/input_editor.hpp"

#include "octopus/cli/terminal_input.hpp"

#include <istream>
#include <ostream>
#include <utility>

namespace octopus {
namespace {

InputEditorStatus toInputEditorStatus(TerminalReadStatus status) {
  switch (status) {
    case TerminalReadStatus::Submitted:
      return InputEditorStatus::Submitted;
    case TerminalReadStatus::EndOfFile:
      return InputEditorStatus::EndOfFile;
    case TerminalReadStatus::Cancelled:
      return InputEditorStatus::Cancelled;
  }

  return InputEditorStatus::EndOfFile;
}

}  // namespace

InputEditor::InputEditor(std::istream& in, std::ostream& out,
                         InputEditorPrompts prompts)
    : in_(in), out_(out), prompts_(std::move(prompts)) {}

InputEditorResult InputEditor::read() {
  const auto result =
      readTerminalInput(in_, out_, prompts_.primary, prompts_.continuation);
  return {toInputEditorStatus(result.status), result.text};
}

std::optional<std::string> InputEditor::readText() {
  const auto result = read();
  if (result.status != InputEditorStatus::Submitted) {
    return std::nullopt;
  }
  return result.text;
}

}  // namespace octopus
