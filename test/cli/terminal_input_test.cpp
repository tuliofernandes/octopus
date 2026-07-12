#include "octopus/cli/terminal_input.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>

namespace {

octopus::TerminalInputEvent feedBytes(octopus::TerminalLineEditor& editor,
                                      const std::string& bytes) {
  octopus::TerminalInputEvent event;
  for (const char byte : bytes) {
    event = editor.feed(byte);
  }
  return event;
}

}  // namespace

TEST_CASE("terminal input inserts text at the cursor after left arrow",
          "[terminal-input]") {
  octopus::TerminalLineEditor editor;

  const auto event = feedBytes(editor, "ab\x1b[DX\n");

  CHECK(event.type == octopus::TerminalInputEventType::Submitted);
  CHECK(event.text == "aXb");
}

TEST_CASE("terminal input consumes up and down arrows", "[terminal-input]") {
  octopus::TerminalLineEditor editor;

  const auto event = feedBytes(editor, "a\x1b[A\x1b[B\n");

  CHECK(event.type == octopus::TerminalInputEventType::Submitted);
  CHECK(event.text == "a");
}

TEST_CASE("terminal input supports right arrow and backspace",
          "[terminal-input]") {
  octopus::TerminalLineEditor editor;

  const auto event = feedBytes(editor, "abc\x1b[D\x7fX\x1b[C!\n");

  CHECK(event.type == octopus::TerminalInputEventType::Submitted);
  CHECK(event.text == "aXc!");
}

TEST_CASE("terminal input supports modified arrow word movement",
          "[terminal-input]") {
  octopus::TerminalLineEditor editor;

  const auto event = feedBytes(editor, "hello world\x1b[1;5DX\x1b[1;5C!\n");

  CHECK(event.type == octopus::TerminalInputEventType::Submitted);
  CHECK(event.text == "hello Xworld!");
}

TEST_CASE("terminal input supports alt word movement escape shortcuts",
          "[terminal-input]") {
  octopus::TerminalLineEditor editor;

  const auto event = feedBytes(editor,
                               "hello world\x1b"
                               "bX\x1b"
                               "f!\n");

  CHECK(event.type == octopus::TerminalInputEventType::Submitted);
  CHECK(event.text == "hello Xworld!");
}

TEST_CASE("terminal input joins backslash-continued lines",
          "[terminal-input]") {
  octopus::TerminalLineEditor editor;

  const auto event = feedBytes(editor, "hello\\\nworld\n");

  CHECK(event.type == octopus::TerminalInputEventType::Submitted);
  CHECK(event.text == "hello\nworld");
}

TEST_CASE("terminal input inserts a line break for Shift Enter sequences",
          "[terminal-input]") {
  octopus::TerminalLineEditor kitty_editor;
  const auto kitty_event = feedBytes(kitty_editor, "hello\x1b[13;2uworld\n");

  CHECK(kitty_event.type == octopus::TerminalInputEventType::Submitted);
  CHECK(kitty_event.text == "hello\nworld");

  octopus::TerminalLineEditor m_variant_editor;
  const auto m_variant_event =
      feedBytes(m_variant_editor, "hello\x1b[13;2Mworld\n");

  CHECK(m_variant_event.type == octopus::TerminalInputEventType::Submitted);
  CHECK(m_variant_event.text == "hello\nworld");

  octopus::TerminalLineEditor ss3_editor;
  const auto ss3_event = feedBytes(ss3_editor, "hello\x1bOMworld\n");

  CHECK(ss3_event.type == octopus::TerminalInputEventType::Submitted);
  CHECK(ss3_event.text == "hello\nworld");
}

TEST_CASE("terminal input treats bracketed paste newlines as input text",
          "[terminal-input]") {
  octopus::TerminalLineEditor editor;

  const auto event = feedBytes(
      editor, "\x1b[200~#include <stdio.h>\n\tint main() {}\n\x1b[201~\n");

  CHECK(event.type == octopus::TerminalInputEventType::Submitted);
  CHECK(event.text == "#include <stdio.h>\n\tint main() {}\n");
}

TEST_CASE("terminal input backspaces naturally across multiline input",
          "[terminal-input]") {
  octopus::TerminalLineEditor editor;

  const auto event = feedBytes(editor, "ddd\x1bOMaaa\x7f\x7f\x7f\x7f\n");

  CHECK(event.type == octopus::TerminalInputEventType::Submitted);
  CHECK(event.text == "ddd");
}

TEST_CASE("terminal input supports word deletion before the cursor",
          "[terminal-input]") {
  octopus::TerminalLineEditor alt_backspace_editor;
  const auto alt_backspace_event =
      feedBytes(alt_backspace_editor, "hello brave world\x1b\x7f\n");

  CHECK(alt_backspace_event.type == octopus::TerminalInputEventType::Submitted);
  CHECK(alt_backspace_event.text == "hello brave ");

  octopus::TerminalLineEditor ctrl_w_editor;
  const auto ctrl_w_event = feedBytes(ctrl_w_editor, "hello brave world\x17\n");

  CHECK(ctrl_w_event.type == octopus::TerminalInputEventType::Submitted);
  CHECK(ctrl_w_event.text == "hello brave ");

  octopus::TerminalLineEditor kitty_ctrl_backspace_editor;
  const auto kitty_ctrl_backspace_event =
      feedBytes(kitty_ctrl_backspace_editor, "hello brave world\x1b[127;5u\n");

  CHECK(kitty_ctrl_backspace_event.type ==
        octopus::TerminalInputEventType::Submitted);
  CHECK(kitty_ctrl_backspace_event.text == "hello brave ");
}

TEST_CASE("terminal input supports word deletion after the cursor",
          "[terminal-input]") {
  octopus::TerminalLineEditor alt_delete_editor;
  const auto alt_delete_event =
      feedBytes(alt_delete_editor, "hello brave world\x1b[1;5D\x1b[3;3~\n");

  CHECK(alt_delete_event.type == octopus::TerminalInputEventType::Submitted);
  CHECK(alt_delete_event.text == "hello brave ");

  octopus::TerminalLineEditor ctrl_delete_editor;
  const auto ctrl_delete_event =
      feedBytes(ctrl_delete_editor, "hello brave world\x1b[1;5D\x1b[3;5~\n");

  CHECK(ctrl_delete_event.type == octopus::TerminalInputEventType::Submitted);
  CHECK(ctrl_delete_event.text == "hello brave ");

  octopus::TerminalLineEditor alt_d_editor;
  const auto alt_d_event = feedBytes(alt_d_editor,
                                     "hello brave world\x1b[1;5D\x1b"
                                     "d\n");

  CHECK(alt_d_event.type == octopus::TerminalInputEventType::Submitted);
  CHECK(alt_d_event.text == "hello brave ");
}

TEST_CASE("terminal input can submit empty continued input",
          "[terminal-input]") {
  octopus::TerminalLineEditor editor;

  const auto event = feedBytes(editor, "\\\n\n");

  CHECK(event.type == octopus::TerminalInputEventType::Submitted);
  CHECK(event.text.empty());
}

TEST_CASE("terminal input emits EOF on Ctrl+D at empty input",
          "[terminal-input]") {
  octopus::TerminalLineEditor editor;

  const auto event = editor.feed('\x04');

  CHECK(event.type == octopus::TerminalInputEventType::EndOfFile);
}

TEST_CASE("terminal input emits cancellation on Ctrl+C", "[terminal-input]") {
  octopus::TerminalLineEditor editor;

  const auto event = editor.feed('\x03');

  CHECK(event.type == octopus::TerminalInputEventType::Cancelled);
  CHECK(editor.currentLine().empty());
}

TEST_CASE("terminal style policy keeps non-TTY output plain",
          "[terminal-input]") {
  const auto style =
      octopus::makeTerminalStylePolicy(false, false, "xterm-256color");

  CHECK(style.user_start.empty());
  CHECK(style.user_end.empty());
}

TEST_CASE("terminal style policy honors NO_COLOR", "[terminal-input]") {
  const auto style =
      octopus::makeTerminalStylePolicy(true, true, "xterm-256color");

  CHECK(style.user_start.empty());
  CHECK(style.user_end.empty());
}

TEST_CASE("terminal style policy keeps TERM=dumb plain", "[terminal-input]") {
  const auto style = octopus::makeTerminalStylePolicy(true, false, "dumb");

  CHECK(style.user_start.empty());
  CHECK(style.user_end.empty());
}

TEST_CASE("terminal style policy quiets interactive TTY user input",
          "[terminal-input]") {
  const auto style =
      octopus::makeTerminalStylePolicy(true, false, "xterm-256color");

  CHECK(style.user_start == "\x1b[2m");
  CHECK(style.user_end == "\x1b[0m");
}
