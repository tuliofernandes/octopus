#include "octopus/cli/terminal_input.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

octopus::TerminalInputEvent feedBytes(octopus::TerminalLineEditor& editor,
                                      const std::string& bytes) {
  octopus::TerminalInputEvent event;
  for (const char byte : bytes) {
    event = editor.feed(byte);
  }
  return event;
}

class ScopedFd final {
 public:
  explicit ScopedFd(int fd) : fd_(fd) {}
  ScopedFd(const ScopedFd&) = delete;
  ScopedFd& operator=(const ScopedFd&) = delete;
  ~ScopedFd() { reset(); }

  int get() const noexcept { return fd_; }
  void reset() noexcept {
    if (fd_ >= 0) {
      close(fd_);
      fd_ = -1;
    }
  }

 private:
  int fd_;
};

bool readPtyUntil(int fd, std::string& output, std::string_view marker,
                  bool wait_for_close) {
  using Clock = std::chrono::steady_clock;
  const auto deadline = Clock::now() + std::chrono::seconds(5);
  while (Clock::now() < deadline) {
    pollfd descriptor{fd, static_cast<short>(POLLIN | POLLHUP), 0};
    const int ready = poll(&descriptor, 1, 100);
    if (ready < 0) {
      if (errno == EINTR) {
        continue;
      }
      return false;
    }
    if (ready == 0) {
      continue;
    }

    char bytes[512];
    const ssize_t count = read(fd, bytes, sizeof(bytes));
    if (count > 0) {
      output.append(bytes, static_cast<std::size_t>(count));
      if (!wait_for_close && output.find(marker) != std::string::npos) {
        return true;
      }
      continue;
    }
    if (count == 0 || (count < 0 && errno == EIO)) {
      return wait_for_close;
    }
    if (count < 0 && errno != EINTR) {
      return false;
    }
  }
  return false;
}

bool writeAll(int fd, std::string_view bytes) {
  while (!bytes.empty()) {
    const ssize_t count = write(fd, bytes.data(), bytes.size());
    if (count < 0) {
      if (errno == EINTR) {
        continue;
      }
      return false;
    }
    bytes.remove_prefix(static_cast<std::size_t>(count));
  }
  return true;
}

class SimpleTerminalScreen final {
 public:
  explicit SimpleTerminalScreen(std::size_t columns)
      : columns_(columns), rows_(1, std::string(columns, ' ')) {}

  void consume(std::string_view bytes) {
    for (std::size_t offset = 0; offset < bytes.size();) {
      const unsigned char byte = static_cast<unsigned char>(bytes[offset]);
      if (byte == 0x1BU && offset + 1 < bytes.size() &&
          bytes[offset + 1] == '[') {
        offset = consumeControlSequence(bytes, offset + 2);
        continue;
      }
      ++offset;
      if (byte == '\r') {
        column_ = 0;
        pending_wrap_ = false;
      } else if (byte == '\n') {
        ++row_;
        ensureRow();
        pending_wrap_ = false;
      } else if (byte >= 0x20U && byte < 0x7FU) {
        put(static_cast<char>(byte));
      }
    }
  }

  std::vector<std::string> nonEmptyRows() const {
    std::vector<std::string> result;
    for (std::string row : rows_) {
      while (!row.empty() && row.back() == ' ') {
        row.pop_back();
      }
      if (!row.empty()) {
        result.push_back(std::move(row));
      }
    }
    return result;
  }

 private:
  std::size_t consumeControlSequence(std::string_view bytes,
                                     std::size_t offset) {
    std::size_t value = 0;
    bool has_value = false;
    bool private_mode = false;
    for (; offset < bytes.size(); ++offset) {
      const char byte = bytes[offset];
      if (byte == '?') {
        private_mode = true;
        continue;
      }
      if (byte >= '0' && byte <= '9') {
        has_value = true;
        value = value * 10 + static_cast<std::size_t>(byte - '0');
        continue;
      }
      if (byte == ';') {
        continue;
      }
      if (byte < '@' || byte > '~') {
        continue;
      }

      const std::size_t count = has_value ? value : 1;
      if (!private_mode && byte == 'A') {
        row_ -= std::min(row_, count);
      } else if (!private_mode && byte == 'C') {
        column_ = std::min(columns_ - 1, column_ + count);
        pending_wrap_ = false;
      } else if (!private_mode && byte == 'J') {
        eraseToEnd();
      }
      return offset + 1;
    }
    return offset;
  }

  void put(char byte) {
    if (pending_wrap_) {
      ++row_;
      column_ = 0;
      pending_wrap_ = false;
      ensureRow();
    }
    rows_[row_][column_] = byte;
    if (column_ + 1 == columns_) {
      pending_wrap_ = true;
    } else {
      ++column_;
    }
  }

  void eraseToEnd() {
    ensureRow();
    std::fill(rows_[row_].begin() + static_cast<std::ptrdiff_t>(column_),
              rows_[row_].end(), ' ');
    rows_.resize(row_ + 1);
    pending_wrap_ = false;
  }

  void ensureRow() {
    while (rows_.size() <= row_) {
      rows_.push_back(std::string(columns_, ' '));
    }
  }

  std::size_t columns_;
  std::vector<std::string> rows_;
  std::size_t row_ = 0;
  std::size_t column_ = 0;
  bool pending_wrap_ = false;
};

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

TEST_CASE("terminal input accepts and edits complete UTF-8 code points",
          "[terminal-input]") {
  octopus::TerminalLineEditor submit_editor;
  const auto submit =
      feedBytes(submit_editor, "a\xC3\xA7\xC3\xA3o mem\xC3\xB3ria\n");
  CHECK(submit.type == octopus::TerminalInputEventType::Submitted);
  CHECK(submit.text == "a\xC3\xA7\xC3\xA3o mem\xC3\xB3ria");

  octopus::TerminalLineEditor insert_editor;
  const auto inserted = feedBytes(insert_editor,
                                  "a\xC3\xA7"
                                  "b\x1b[D\xC3\xA3\n");
  CHECK(inserted.text ==
        "a\xC3\xA7\xC3\xA3"
        "b");

  octopus::TerminalLineEditor delete_editor;
  const auto deleted = feedBytes(delete_editor,
                                 "a\xC3\xA7\xC3\xA3"
                                 "b\x1b[D\x7f\x1b[3~\n");
  CHECK(deleted.text == "a\xC3\xA7");
}

TEST_CASE("terminal input discards invalid and incomplete UTF-8 safely",
          "[terminal-input]") {
  octopus::TerminalLineEditor editor;

  CHECK(editor.feed(static_cast<char>(0xC3)).type ==
        octopus::TerminalInputEventType::None);
  CHECK(editor.currentLine().empty());
  CHECK(editor.hasPendingUtf8());

  CHECK(editor.feed('X').type == octopus::TerminalInputEventType::None);
  CHECK(editor.currentLine() == "X");
  CHECK_FALSE(editor.hasPendingUtf8());

  const auto event = editor.feed('\n');
  CHECK(event.type == octopus::TerminalInputEventType::Submitted);
  CHECK(event.text == "X");
}

TEST_CASE("terminal input layout accounts for wrapping and UTF-8 cells",
          "[terminal-input]") {
  const auto exact = octopus::measureTerminalInput("> ", "..",
                                                   "a\xC3\xA7"
                                                   "b",
                                                   4, 5);
  CHECK(exact.rows == 1);
  CHECK(exact.end.row == 0);
  CHECK(exact.end.column == 5);

  const auto wrapped = octopus::measureTerminalInput("> ", "..", "abcX", 6, 5);
  CHECK(wrapped.rows == 2);
  CHECK(wrapped.end.row == 1);
  CHECK(wrapped.end.column == 1);

  const auto cursor_before_wrap =
      octopus::measureTerminalInput("> ", "..", "abcX", 3, 5);
  CHECK(cursor_before_wrap.cursor.row == 0);
  CHECK(cursor_before_wrap.cursor.column == 5);
  CHECK(cursor_before_wrap.end.row == 1);
}

TEST_CASE("terminal redraw clears stale text after right-margin wrapping",
          "[terminal-input][pty]") {
  ScopedFd master(posix_openpt(O_RDWR | O_NOCTTY));
  REQUIRE(master.get() >= 0);
  REQUIRE(grantpt(master.get()) == 0);
  REQUIRE(unlockpt(master.get()) == 0);
  const char* slave_name = ptsname(master.get());
  REQUIRE(slave_name != nullptr);
  ScopedFd slave(open(slave_name, O_RDWR | O_NOCTTY));
  REQUIRE(slave.get() >= 0);
  winsize size{};
  size.ws_row = 24;
  size.ws_col = 5;
  REQUIRE(ioctl(slave.get(), TIOCSWINSZ, &size) == 0);

  std::cout.flush();
  std::cerr.flush();
  std::fflush(nullptr);
  const pid_t child = fork();
  REQUIRE(child >= 0);
  if (child == 0) {
    master.reset();
    if (dup2(slave.get(), STDIN_FILENO) < 0 ||
        dup2(slave.get(), STDOUT_FILENO) < 0) {
      _exit(2);
    }
    slave.reset();
    setenv("TERM", "xterm-256color", 1);
    setenv("LANG", "C.UTF-8", 1);
    const auto result = octopus::readTerminalInput(std::cin, std::cout, "", "");
    const bool ok = result.status == octopus::TerminalReadStatus::Submitted &&
                    result.text == "abcde";
    _exit(ok ? 0 : 3);
  }

  slave.reset();
  std::string output;
  REQUIRE(readPtyUntil(master.get(), output, "\x1b[?2004h", false));
  REQUIRE(writeAll(master.get(), "abcdef\x7f\n"));
  REQUIRE(readPtyUntil(master.get(), output, "", true));

  int status = 0;
  REQUIRE(waitpid(child, &status, 0) == child);
  REQUIRE(WIFEXITED(status));
  REQUIRE(WEXITSTATUS(status) == 0);

  SimpleTerminalScreen screen(5);
  screen.consume(output);
  CHECK(screen.nonEmptyRows() == std::vector<std::string>{"abcde"});
}
