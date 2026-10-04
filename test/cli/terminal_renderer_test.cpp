#include "octopus/cli/terminal_renderer.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstddef>
#include <optional>
#include <ostream>
#include <streambuf>
#include <string>
#include <string_view>

#include <fcntl.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <unistd.h>

namespace {

class RecordingBuffer final : public std::streambuf {
 public:
  std::string text;
  std::size_t writes = 0;
  std::size_t flushes = 0;

 protected:
  std::streamsize xsputn(const char* data, std::streamsize count) override {
    ++writes;
    text.append(data, static_cast<std::size_t>(count));
    return count;
  }

  int_type overflow(int_type character) override {
    if (!traits_type::eq_int_type(character, traits_type::eof())) {
      text.push_back(traits_type::to_char_type(character));
    }
    return character;
  }

  int sync() override {
    ++flushes;
    return 0;
  }
};

class ScopedFd final {
 public:
  explicit ScopedFd(int fd) : fd_(fd) {}
  ScopedFd(const ScopedFd&) = delete;
  ScopedFd& operator=(const ScopedFd&) = delete;
  ~ScopedFd() {
    if (fd_ >= 0) {
      close(fd_);
    }
  }
  int get() const noexcept { return fd_; }

 private:
  int fd_;
};

octopus::TerminalCapabilities enhancedCapabilities() {
  return {true, true, true, true, true, true, std::size_t{80}};
}

}  // namespace

TEST_CASE("terminal capability policy separates NO_COLOR from controls",
          "[terminal-renderer]") {
  const auto colored = octopus::makeTerminalCapabilities(
      true, true, "xterm-256color", std::nullopt, true, 80);
  CHECK(colored.interactive_session);
  CHECK(colored.cursor_control);
  CHECK(colored.erase_line);
  CHECK(colored.sgr_attributes);
  CHECK(colored.color);

  const auto no_color = octopus::makeTerminalCapabilities(
      true, true, "xterm-256color", std::string_view{"1"}, true, 80);
  CHECK(no_color.cursor_control);
  CHECK(no_color.sgr_attributes);
  CHECK_FALSE(no_color.color);

  const auto empty_no_color = octopus::makeTerminalCapabilities(
      true, true, "xterm-256color", std::string_view{}, true, 80);
  CHECK(empty_no_color.color);
}

TEST_CASE("terminal capability policy degrades dumb and widthless terminals",
          "[terminal-renderer]") {
  const auto dumb = octopus::makeTerminalCapabilities(true, true, "dumb",
                                                      std::nullopt, true, 80);
  CHECK(dumb.interactive_session);
  CHECK_FALSE(dumb.cursor_control);
  CHECK_FALSE(dumb.sgr_attributes);

  const auto widthless = octopus::makeTerminalCapabilities(
      true, true, "xterm", std::nullopt, true, std::nullopt);
  CHECK(widthless.cursor_control);
  CHECK_FALSE(widthless.columns.has_value());

  const auto missing_term =
      octopus::makeTerminalCapabilities(true, true, "", std::nullopt, true, 80);
  CHECK(missing_term.interactive_session);
  CHECK_FALSE(missing_term.cursor_control);

  const auto zero_width = octopus::makeTerminalCapabilities(
      true, true, "xterm", std::nullopt, true, 0);
  CHECK_FALSE(zero_width.columns.has_value());

  const auto redirected = octopus::makeTerminalCapabilities(
      true, false, "xterm", std::nullopt, true, 80);
  CHECK_FALSE(redirected.interactive_session);
  CHECK_FALSE(redirected.cursor_control);
}

TEST_CASE("terminal width query reads a pseudo-terminal size",
          "[terminal-renderer]") {
  ScopedFd terminal(posix_openpt(O_RDWR | O_NOCTTY));
  REQUIRE(terminal.get() >= 0);
  REQUIRE(grantpt(terminal.get()) == 0);
  REQUIRE(unlockpt(terminal.get()) == 0);
  winsize size{};
  size.ws_row = 24;
  size.ws_col = 5;
  REQUIRE(ioctl(terminal.get(), TIOCSWINSZ, &size) == 0);

  CHECK(octopus::queryTerminalWidth(terminal.get()) == std::size_t{5});
}

TEST_CASE("terminal sanitizer normalizes lines and removes unsafe controls",
          "[terminal-renderer]") {
  const std::string input =
      "ok\r\nnext\rline\t\x1b]52;c;secret\a\x7f"
      "\xC2\x85"
      " bad\xC3X fim";

  CHECK(octopus::sanitizeTerminalText(input) ==
        "ok\nnext\nline\t]52;c;secret bad\xEF\xBF\xBDX fim");
}

TEST_CASE("duration formatting has fixed singular and minute boundaries",
          "[terminal-renderer]") {
  using namespace std::chrono_literals;
  CHECK(octopus::formatElapsed(0ms) == "1 second");
  CHECK(octopus::formatElapsed(1s) == "1 second");
  CHECK(octopus::formatElapsed(2s) == "2 seconds");
  CHECK(octopus::formatElapsed(59s) == "59 seconds");
  CHECK(octopus::formatElapsed(60s) == "1 minute");
  CHECK(octopus::formatElapsed(61s) == "1 minute 1 second");
  CHECK(octopus::formatElapsed(120s) == "2 minutes");
  CHECK(octopus::formatElapsed(121s) == "2 minutes 1 second");
}

TEST_CASE("activity replaces thinking and clears idempotently",
          "[terminal-renderer]") {
  using namespace std::chrono_literals;
  RecordingBuffer buffer;
  std::ostream out(&buffer);
  octopus::TranscriptRenderer renderer(out, enhancedCapabilities(), [] {
    return std::optional<std::size_t>{80};
  });

  renderer.showThinking();
  renderer.finishThinking(12s);
  renderer.clearActivity();

  CHECK(buffer.text ==
        "\r\x1b[2KThinking...\x1b[0m\r\x1b[2KThought for 12 "
        "seconds.\x1b[0m\n");
}

TEST_CASE("activity is suppressed without safe cursor control or width",
          "[terminal-renderer]") {
  using namespace std::chrono_literals;
  RecordingBuffer buffer;
  std::ostream out(&buffer);
  auto capabilities = enhancedCapabilities();
  capabilities.cursor_control = false;
  octopus::TranscriptRenderer plain_renderer(out, capabilities);
  plain_renderer.showThinking();
  plain_renderer.finishThinking(2s);

  capabilities.cursor_control = true;
  capabilities.columns.reset();
  octopus::TranscriptRenderer widthless_renderer(out, capabilities);
  widthless_renderer.showThinking();
  widthless_renderer.clearActivity();

  CHECK(buffer.text.empty());
}

TEST_CASE("activity destructor restores a live transient line",
          "[terminal-renderer]") {
  RecordingBuffer buffer;
  std::ostream out(&buffer);
  {
    octopus::TranscriptRenderer renderer(out, enhancedCapabilities());
    renderer.showThinking();
  }

  CHECK(buffer.text == "\r\x1b[2KThinking...\x1b[0m\r\x1b[2K\x1b[0m");
}

TEST_CASE("Markdown adapter owns semantic content for completed documents",
          "[terminal-renderer]") {
  const auto document = octopus::parseMarkdown(
      "# Heading\n\nAn *emphasis* and **strong** with `code` and "
      "[site](https://example.com).\n\n- first\n- second\n\n> quote\n\n"
      "```cpp\nint main() {}\n```\n");

  REQUIRE(document.has_value());
  const std::string plain = octopus::encodeTerminalDocument(
      *document, octopus::TerminalEncodingPolicy{false, false});
  CHECK(plain.find("Heading") != std::string::npos);
  CHECK(plain.find("An emphasis and strong with code and site "
                   "(https://example.com).") != std::string::npos);
  CHECK(plain.find("- first\n- second") != std::string::npos);
  CHECK(plain.find("> quote") != std::string::npos);
  CHECK(plain.find("int main() {}") != std::string::npos);
}

TEST_CASE("Markdown extensions stay safe and raw HTML remains literal",
          "[terminal-renderer]") {
  const auto document =
      octopus::parseMarkdown("- [x] done\n\n~~old~~ <b>literal</b> &#27;safe");

  REQUIRE(document.has_value());
  const std::string encoded = octopus::encodeTerminalDocument(
      *document, octopus::TerminalEncodingPolicy{true, false});
  CHECK(encoded.find("- [x] done") != std::string::npos);
  CHECK(encoded.find("<b>literal</b>") != std::string::npos);
  CHECK(encoded.find("safe") != std::string::npos);
  CHECK(encoded.find('\x1b') != std::string::npos);
  CHECK(encoded.find("\x1b[9mold\x1b[0m") != std::string::npos);
  CHECK(encoded.find("\x1b\x1b") == std::string::npos);
}

TEST_CASE("Markdown response is encoded and committed in one write",
          "[terminal-renderer]") {
  RecordingBuffer buffer;
  std::ostream out(&buffer);
  octopus::TranscriptRenderer renderer(out, enhancedCapabilities());

  renderer.renderAssistant(
      "# Heading\n\nFirst paragraph.\n\nSecond paragraph.");

  CHECK(buffer.writes == 1);
  CHECK(buffer.flushes == 1);
  CHECK(buffer.text.find("\x1b[1;36mHeading\x1b[0m") != std::string::npos);
  CHECK(buffer.text.find("First paragraph.\n\nSecond paragraph.") !=
        std::string::npos);
}

TEST_CASE("NO_COLOR preserves semantic attributes but removes color",
          "[terminal-renderer]") {
  const auto document = octopus::parseMarkdown("# Heading\n\n**Strong**");
  REQUIRE(document.has_value());

  const std::string colored = octopus::encodeTerminalDocument(
      *document, octopus::TerminalEncodingPolicy{true, true});
  const std::string no_color = octopus::encodeTerminalDocument(
      *document, octopus::TerminalEncodingPolicy{true, false});

  CHECK(colored.find("\x1b[1;36mHeading") != std::string::npos);
  CHECK(no_color.find("\x1b[1mHeading") != std::string::npos);
  CHECK(no_color.find("36") == std::string::npos);
  CHECK(no_color.find("\x1b[1mStrong") != std::string::npos);
}

TEST_CASE("plain response preserves sanitized Markdown without chrome",
          "[terminal-renderer]") {
  RecordingBuffer buffer;
  std::ostream out(&buffer);
  octopus::TranscriptRenderer renderer(out, {});

  renderer.showThinking();
  renderer.renderAssistant("# Raw\r\n\x1b[31mtext");

  CHECK(buffer.writes == 1);
  CHECK(buffer.text == "# Raw\n[31mtext\n\n");
  CHECK(buffer.text.find('\x1b') == std::string::npos);
}
