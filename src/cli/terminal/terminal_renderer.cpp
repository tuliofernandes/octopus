#include "octopus/cli/terminal_renderer.hpp"

#include "cli/terminal/text.hpp"

#include <md4c.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <optional>
#include <ostream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <poll.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

namespace octopus {
namespace {

constexpr std::string_view kReplacementCharacter = "\xEF\xBF\xBD";
constexpr std::size_t kMaximumMarkdownBytes = 4U * 1024U * 1024U;
constexpr std::size_t kMaximumNesting = 64;
constexpr std::size_t kMaximumTerminalResponseBytes = 256;

std::optional<std::string> environmentValue(const char* name) {
  const char* value = std::getenv(name);
  if (value == nullptr) {
    return std::nullopt;
  }
  return std::string(value);
}

bool localeIsUtf8() {
  std::optional<std::string> locale = environmentValue("LC_ALL");
  if (!locale.has_value() || locale->empty()) {
    locale = environmentValue("LC_CTYPE");
  }
  if (!locale.has_value() || locale->empty()) {
    locale = environmentValue("LANG");
  }
  if (!locale.has_value()) {
    return false;
  }
  std::string normalized = *locale;
  std::transform(
      normalized.begin(), normalized.end(), normalized.begin(),
      [](unsigned char byte) { return static_cast<char>(std::tolower(byte)); });
  return normalized.find("utf-8") != std::string::npos ||
         normalized.find("utf8") != std::string::npos;
}

bool environmentSupportsTruecolor() {
  const auto colorterm = environmentValue("COLORTERM");
  if (!colorterm.has_value()) {
    return false;
  }
  std::string normalized = *colorterm;
  std::transform(
      normalized.begin(), normalized.end(), normalized.begin(),
      [](unsigned char byte) { return static_cast<char>(std::tolower(byte)); });
  return normalized == "truecolor" || normalized == "24bit";
}

std::optional<unsigned> parseUnsigned(std::string_view text, unsigned base,
                                      unsigned maximum) noexcept {
  if (text.empty()) {
    return std::nullopt;
  }
  unsigned value = 0;
  for (const char character : text) {
    const auto byte = static_cast<unsigned char>(character);
    unsigned digit = 0;
    if (byte >= '0' && byte <= '9') {
      digit = byte - '0';
    } else if (base == 16 && byte >= 'a' && byte <= 'f') {
      digit = byte - 'a' + 10U;
    } else if (base == 16 && byte >= 'A' && byte <= 'F') {
      digit = byte - 'A' + 10U;
    } else {
      return std::nullopt;
    }
    if (digit >= base || value > (maximum - digit) / base) {
      return std::nullopt;
    }
    value = value * base + digit;
  }
  return value;
}

std::uint8_t scaleHexComponent(std::string_view component) noexcept {
  const unsigned maximum = (1U << (component.size() * 4U)) - 1U;
  const auto value = parseUnsigned(component, 16, maximum);
  if (!value.has_value()) {
    return 0;
  }
  return static_cast<std::uint8_t>((*value * 255U + maximum / 2U) / maximum);
}

std::optional<TerminalRgb> xtermIndexedColor(unsigned index) noexcept {
  constexpr std::array<TerminalRgb, 16> kAnsiColors{{
      {0, 0, 0},
      {205, 0, 0},
      {0, 205, 0},
      {205, 205, 0},
      {0, 0, 238},
      {205, 0, 205},
      {0, 205, 205},
      {229, 229, 229},
      {127, 127, 127},
      {255, 0, 0},
      {0, 255, 0},
      {255, 255, 0},
      {92, 92, 255},
      {255, 0, 255},
      {0, 255, 255},
      {255, 255, 255},
  }};
  if (index < kAnsiColors.size()) {
    return kAnsiColors[index];
  }
  if (index <= 231) {
    constexpr std::array<std::uint8_t, 6> kCube{0, 95, 135, 175, 215, 255};
    const unsigned cube = index - 16;
    return TerminalRgb{kCube[cube / 36], kCube[(cube / 6) % 6],
                       kCube[cube % 6]};
  }
  if (index <= 255) {
    const auto gray = static_cast<std::uint8_t>(8 + (index - 232) * 10);
    return TerminalRgb{gray, gray, gray};
  }
  return std::nullopt;
}

std::optional<TerminalRgb> colorFromColorFgbg(
    std::optional<std::string> value) noexcept {
  if (!value.has_value() || value->empty()) {
    return std::nullopt;
  }
  const std::size_t separator = value->find_last_of(';');
  const std::string_view background =
      separator == std::string::npos
          ? std::string_view(*value)
          : std::string_view(*value).substr(separator + 1);
  const auto index = parseUnsigned(background, 10, 255);
  return index.has_value() ? xtermIndexedColor(*index) : std::nullopt;
}

double relativeLuminance(TerminalRgb color) noexcept {
  const auto linearize = [](std::uint8_t component) {
    const double srgb = static_cast<double>(component) / 255.0;
    return srgb <= 0.04045 ? srgb / 12.92
                           : std::pow((srgb + 0.055) / 1.055, 2.4);
  };
  return 0.2126 * linearize(color.red) + 0.7152 * linearize(color.green) +
         0.0722 * linearize(color.blue);
}

unsigned nearestXtermColorIndex(
    TerminalRgb color, std::optional<unsigned> excluded_index,
    std::optional<TerminalRgb> contrast_background) noexcept {
  unsigned best_index = 16;
  unsigned best_distance = std::numeric_limits<unsigned>::max();
  bool found = false;
  const double target_luminance = relativeLuminance(color);
  const double background_luminance =
      contrast_background.has_value() ? relativeLuminance(*contrast_background)
                                      : target_luminance;
  const bool enforce_direction = contrast_background.has_value() &&
                                 target_luminance != background_luminance;
  const bool target_is_lighter = target_luminance > background_luminance;
  for (unsigned index = 16; index <= 255; ++index) {
    if (excluded_index.has_value() && index == *excluded_index) {
      continue;
    }
    const auto candidate = xtermIndexedColor(index);
    if (!candidate.has_value()) {
      continue;
    }
    if (enforce_direction) {
      const double candidate_luminance = relativeLuminance(*candidate);
      if ((target_is_lighter && candidate_luminance <= background_luminance) ||
          (!target_is_lighter && candidate_luminance >= background_luminance)) {
        continue;
      }
    }
    const int red = static_cast<int>(candidate->red) - color.red;
    const int green = static_cast<int>(candidate->green) - color.green;
    const int blue = static_cast<int>(candidate->blue) - color.blue;
    const unsigned distance =
        static_cast<unsigned>(red * red + green * green + blue * blue);
    if (distance < best_distance) {
      found = true;
      best_distance = distance;
      best_index = index;
    }
  }
  if (!found && enforce_direction) {
    return nearestXtermColorIndex(color, excluded_index, std::nullopt);
  }
  return best_index;
}

unsigned xtermColorIndex(TerminalRgb color) noexcept {
  return nearestXtermColorIndex(color, std::nullopt, std::nullopt);
}

unsigned xtermResponseColorIndex(
    TerminalRgb color,
    std::optional<TerminalRgb> terminal_background) noexcept {
  const std::optional<unsigned> background_index =
      terminal_background.has_value()
          ? std::optional<unsigned>{xtermColorIndex(*terminal_background)}
          : std::nullopt;
  return nearestXtermColorIndex(color, background_index, terminal_background);
}

std::string backgroundPrefix(TerminalRgb color, bool truecolor,
                             std::optional<TerminalRgb> terminal_background) {
  if (truecolor) {
    return "\x1b[48;2;" + std::to_string(color.red) + ";" +
           std::to_string(color.green) + ";" + std::to_string(color.blue) + "m";
  }
  return "\x1b[48;5;" +
         std::to_string(xtermResponseColorIndex(color, terminal_background)) +
         "m";
}

void appendUtf8(std::string& output, std::uint32_t value) {
  if (value <= 0x7FU) {
    output.push_back(static_cast<char>(value));
  } else if (value <= 0x7FFU) {
    output.push_back(static_cast<char>(0xC0U | (value >> 6U)));
    output.push_back(static_cast<char>(0x80U | (value & 0x3FU)));
  } else if (value <= 0xFFFFU) {
    output.push_back(static_cast<char>(0xE0U | (value >> 12U)));
    output.push_back(static_cast<char>(0x80U | ((value >> 6U) & 0x3FU)));
    output.push_back(static_cast<char>(0x80U | (value & 0x3FU)));
  } else {
    output.push_back(static_cast<char>(0xF0U | (value >> 18U)));
    output.push_back(static_cast<char>(0x80U | ((value >> 12U) & 0x3FU)));
    output.push_back(static_cast<char>(0x80U | ((value >> 6U) & 0x3FU)));
    output.push_back(static_cast<char>(0x80U | (value & 0x3FU)));
  }
}

std::string decodeEntity(std::string_view entity) {
  if (entity == "&amp;") {
    return "&";
  }
  if (entity == "&lt;") {
    return "<";
  }
  if (entity == "&gt;") {
    return ">";
  }
  if (entity == "&quot;") {
    return "\"";
  }
  if (entity == "&apos;") {
    return "'";
  }
  if (entity == "&nbsp;") {
    return " ";
  }
  if (entity.size() < 4 || entity.front() != '&' || entity[1] != '#' ||
      entity.back() != ';') {
    return std::string(entity);
  }

  std::size_t offset = 2;
  int base = 10;
  if (offset < entity.size() &&
      (entity[offset] == 'x' || entity[offset] == 'X')) {
    base = 16;
    ++offset;
  }
  std::uint32_t value = 0;
  bool has_digit = false;
  for (; offset + 1 < entity.size(); ++offset) {
    const unsigned char byte = static_cast<unsigned char>(entity[offset]);
    unsigned digit = 0;
    if (byte >= '0' && byte <= '9') {
      digit = byte - '0';
    } else if (base == 16 && byte >= 'a' && byte <= 'f') {
      digit = byte - 'a' + 10U;
    } else if (base == 16 && byte >= 'A' && byte <= 'F') {
      digit = byte - 'A' + 10U;
    } else {
      return std::string(entity);
    }
    has_digit = true;
    if (value > (0x10FFFFU - digit) / static_cast<unsigned>(base)) {
      return std::string(kReplacementCharacter);
    }
    value = value * static_cast<unsigned>(base) + digit;
  }
  if (!has_digit || value == 0 || value > 0x10FFFFU ||
      (value >= 0xD800U && value <= 0xDFFFU)) {
    return std::string(kReplacementCharacter);
  }
  std::string result;
  appendUtf8(result, value);
  return result;
}

std::string copyAttribute(const MD_ATTRIBUTE& attribute) {
  if (attribute.text == nullptr || attribute.size == 0) {
    return {};
  }
  if (attribute.substr_types == nullptr ||
      attribute.substr_offsets == nullptr) {
    return std::string(attribute.text, attribute.size);
  }

  std::string result;
  std::size_t index = 0;
  while (attribute.substr_offsets[index] < attribute.size) {
    const std::size_t begin = attribute.substr_offsets[index];
    const std::size_t end = attribute.substr_offsets[index + 1];
    if (end < begin || end > attribute.size) {
      return {};
    }
    const std::string_view part(attribute.text + begin, end - begin);
    if (attribute.substr_types[index] == MD_TEXT_ENTITY) {
      result += decodeEntity(part);
    } else if (attribute.substr_types[index] == MD_TEXT_NULLCHAR) {
      result += kReplacementCharacter;
    } else {
      result.append(part);
    }
    ++index;
  }
  return result;
}

bool lineEmpty(const TerminalLine& line) noexcept { return line.runs.empty(); }

class MarkdownBuilder final {
 public:
  int enterBlock(MD_BLOCKTYPE type, void* detail) {
    switch (type) {
      case MD_BLOCK_DOC:
        break;
      case MD_BLOCK_QUOTE:
        separateBlock();
        if (++quote_depth_ > kMaximumNesting) {
          return fail();
        }
        break;
      case MD_BLOCK_UL:
        separateListIfNeeded();
        if (lists_.size() >= kMaximumNesting) {
          return fail();
        }
        lists_.push_back({false, 1});
        break;
      case MD_BLOCK_OL: {
        separateListIfNeeded();
        if (lists_.size() >= kMaximumNesting || detail == nullptr) {
          return fail();
        }
        const auto& ordered = *static_cast<MD_BLOCK_OL_DETAIL*>(detail);
        lists_.push_back({true, ordered.start});
        break;
      }
      case MD_BLOCK_LI:
        if (++list_item_depth_ > kMaximumNesting || lists_.empty()) {
          return fail();
        }
        if (!lineEmpty(current_)) {
          finishLine();
        }
        addQuotePrefix();
        if (lists_.size() > 1) {
          appendRun(std::string((lists_.size() - 1) * 2, ' '), {});
        }
        appendListMarker(detail);
        break;
      case MD_BLOCK_H:
        separateBlock();
        if (heading_depth_ >= kMaximumNesting) {
          return fail();
        }
        ++heading_depth_;
        break;
      case MD_BLOCK_CODE:
        separateBlock();
        if (code_depth_ >= kMaximumNesting) {
          return fail();
        }
        ++code_depth_;
        break;
      case MD_BLOCK_P:
        if (list_item_depth_ == 0) {
          separateBlock();
        }
        ensureLinePrefix();
        break;
      case MD_BLOCK_HR:
        separateBlock();
        ensureLinePrefix();
        appendRun("---", {});
        finishLine();
        break;
      case MD_BLOCK_HTML:
      case MD_BLOCK_TABLE:
      case MD_BLOCK_THEAD:
      case MD_BLOCK_TBODY:
      case MD_BLOCK_TR:
      case MD_BLOCK_TH:
      case MD_BLOCK_TD:
        return fail();
    }
    return 0;
  }

  int leaveBlock(MD_BLOCKTYPE type) {
    switch (type) {
      case MD_BLOCK_DOC:
        break;
      case MD_BLOCK_QUOTE:
        if (!lineEmpty(current_)) {
          finishLine();
        }
        if (quote_depth_ == 0) {
          return fail();
        }
        --quote_depth_;
        break;
      case MD_BLOCK_UL:
      case MD_BLOCK_OL:
        if (lists_.empty()) {
          return fail();
        }
        lists_.pop_back();
        break;
      case MD_BLOCK_LI:
        if (!lineEmpty(current_)) {
          finishLine();
        }
        if (list_item_depth_ == 0) {
          return fail();
        }
        --list_item_depth_;
        break;
      case MD_BLOCK_H:
        if (!lineEmpty(current_)) {
          finishLine();
        }
        if (heading_depth_ == 0) {
          return fail();
        }
        --heading_depth_;
        break;
      case MD_BLOCK_CODE:
        if (!lineEmpty(current_)) {
          finishLine();
        }
        if (code_depth_ == 0) {
          return fail();
        }
        --code_depth_;
        break;
      case MD_BLOCK_P:
        if (!lineEmpty(current_)) {
          finishLine();
        }
        break;
      case MD_BLOCK_HR:
      case MD_BLOCK_HTML:
      case MD_BLOCK_TABLE:
      case MD_BLOCK_THEAD:
      case MD_BLOCK_TBODY:
      case MD_BLOCK_TR:
      case MD_BLOCK_TH:
      case MD_BLOCK_TD:
        break;
    }
    return 0;
  }

  int enterSpan(MD_SPANTYPE type, void* detail) {
    switch (type) {
      case MD_SPAN_EM:
        if (emphasis_depth_ >= kMaximumNesting) {
          return fail();
        }
        ++emphasis_depth_;
        break;
      case MD_SPAN_STRONG:
        if (strong_depth_ >= kMaximumNesting) {
          return fail();
        }
        ++strong_depth_;
        break;
      case MD_SPAN_CODE:
        if (inline_code_depth_ >= kMaximumNesting) {
          return fail();
        }
        ++inline_code_depth_;
        break;
      case MD_SPAN_DEL:
        if (strikethrough_depth_ >= kMaximumNesting) {
          return fail();
        }
        ++strikethrough_depth_;
        break;
      case MD_SPAN_A: {
        if (detail == nullptr || links_.size() >= kMaximumNesting) {
          return fail();
        }
        const auto& link = *static_cast<MD_SPAN_A_DETAIL*>(detail);
        links_.push_back({copyAttribute(link.href), link.is_autolink != 0});
        break;
      }
      case MD_SPAN_IMG: {
        if (detail == nullptr || links_.size() >= kMaximumNesting) {
          return fail();
        }
        const auto& image = *static_cast<MD_SPAN_IMG_DETAIL*>(detail);
        appendRun("Image: ", currentStyle());
        links_.push_back({copyAttribute(image.src), false});
        break;
      }
      case MD_SPAN_LATEXMATH:
      case MD_SPAN_LATEXMATH_DISPLAY:
      case MD_SPAN_WIKILINK:
      case MD_SPAN_U:
        return fail();
    }
    return 0;
  }

  int leaveSpan(MD_SPANTYPE type) {
    switch (type) {
      case MD_SPAN_EM:
        if (emphasis_depth_ == 0) {
          return fail();
        }
        --emphasis_depth_;
        break;
      case MD_SPAN_STRONG:
        if (strong_depth_ == 0) {
          return fail();
        }
        --strong_depth_;
        break;
      case MD_SPAN_CODE:
        if (inline_code_depth_ == 0) {
          return fail();
        }
        --inline_code_depth_;
        break;
      case MD_SPAN_DEL:
        if (strikethrough_depth_ == 0) {
          return fail();
        }
        --strikethrough_depth_;
        break;
      case MD_SPAN_A:
      case MD_SPAN_IMG:
        if (links_.empty()) {
          return fail();
        }
        if (!links_.back().autolink && !links_.back().destination.empty()) {
          appendRun(" (" + links_.back().destination + ")", currentStyle());
        }
        links_.pop_back();
        break;
      case MD_SPAN_LATEXMATH:
      case MD_SPAN_LATEXMATH_DISPLAY:
      case MD_SPAN_WIKILINK:
      case MD_SPAN_U:
        break;
    }
    return 0;
  }

  int text(MD_TEXTTYPE type, const char* text, MD_SIZE size) {
    if (text == nullptr && size != 0) {
      return fail();
    }
    const std::string_view value(text == nullptr ? "" : text, size);
    switch (type) {
      case MD_TEXT_NORMAL:
      case MD_TEXT_CODE:
        appendText(value, type == MD_TEXT_CODE);
        break;
      case MD_TEXT_NULLCHAR:
        appendRun(std::string(kReplacementCharacter), currentStyle());
        break;
      case MD_TEXT_BR:
        finishLine();
        ensureLinePrefix();
        break;
      case MD_TEXT_SOFTBR:
        appendRun(" ", currentStyle());
        break;
      case MD_TEXT_ENTITY:
        appendRun(decodeEntity(value), currentStyle());
        break;
      case MD_TEXT_HTML:
      case MD_TEXT_LATEXMATH:
        return fail();
    }
    return 0;
  }

  TerminalDocument finish() {
    if (!lineEmpty(current_)) {
      finishLine();
    }
    while (!document_.lines.empty() && lineEmpty(document_.lines.back())) {
      document_.lines.pop_back();
    }
    return std::move(document_);
  }

  bool failed() const noexcept { return failed_; }

 private:
  struct ListState {
    bool ordered = false;
    unsigned next = 1;
  };

  struct LinkState {
    std::string destination;
    bool autolink = false;
  };

  int fail() noexcept {
    failed_ = true;
    return 1;
  }

  TerminalTextStyle currentStyle() const noexcept {
    TerminalTextStyle style;
    style.bold = strong_depth_ > 0 || heading_depth_ > 0;
    style.italic = emphasis_depth_ > 0;
    style.code = inline_code_depth_ > 0 || code_depth_ > 0;
    style.strikethrough = strikethrough_depth_ > 0;
    style.accent = heading_depth_ > 0;
    return style;
  }

  void appendRun(std::string text, TerminalTextStyle style) {
    if (text.empty()) {
      return;
    }
    if (!current_.runs.empty()) {
      auto& previous = current_.runs.back();
      if (previous.style.bold == style.bold &&
          previous.style.italic == style.italic &&
          previous.style.code == style.code &&
          previous.style.strikethrough == style.strikethrough &&
          previous.style.accent == style.accent) {
        previous.text += text;
        return;
      }
    }
    current_.runs.push_back({std::move(text), style});
  }

  void appendText(std::string_view text, bool preserve_breaks) {
    std::size_t begin = 0;
    for (std::size_t index = 0; index < text.size(); ++index) {
      if (text[index] != '\n') {
        continue;
      }
      appendRun(std::string(text.substr(begin, index - begin)), currentStyle());
      finishLine();
      if (preserve_breaks && index + 1 < text.size()) {
        ensureLinePrefix();
      }
      begin = index + 1;
    }
    appendRun(std::string(text.substr(begin)), currentStyle());
  }

  void finishLine() {
    document_.lines.push_back(std::move(current_));
    current_ = {};
  }

  void separateBlock() {
    if (!lineEmpty(current_)) {
      finishLine();
    }
    if (!document_.lines.empty() && !lineEmpty(document_.lines.back())) {
      document_.lines.push_back({});
    }
  }

  void separateListIfNeeded() {
    if (lists_.empty() && list_item_depth_ == 0) {
      separateBlock();
    }
  }

  void addQuotePrefix() {
    if (quote_depth_ > 0) {
      appendRun(std::string(quote_depth_, '>') + " ", {});
    }
  }

  void ensureLinePrefix() {
    if (lineEmpty(current_)) {
      addQuotePrefix();
    }
  }

  void appendListMarker(void* detail) {
    auto& list = lists_.back();
    std::string marker;
    if (list.ordered) {
      marker = std::to_string(list.next++) + ". ";
    } else {
      marker = "- ";
    }
    if (detail != nullptr) {
      const auto& item = *static_cast<MD_BLOCK_LI_DETAIL*>(detail);
      if (item.is_task != 0) {
        marker +=
            (item.task_mark == 'x' || item.task_mark == 'X') ? "[x] " : "[ ] ";
      }
    }
    appendRun(std::move(marker), {});
  }

  TerminalDocument document_;
  TerminalLine current_;
  std::vector<ListState> lists_;
  std::vector<LinkState> links_;
  std::size_t quote_depth_ = 0;
  std::size_t list_item_depth_ = 0;
  std::size_t heading_depth_ = 0;
  std::size_t code_depth_ = 0;
  std::size_t emphasis_depth_ = 0;
  std::size_t strong_depth_ = 0;
  std::size_t inline_code_depth_ = 0;
  std::size_t strikethrough_depth_ = 0;
  bool failed_ = false;
};

template <typename Callback>
int protectCallback(void* userdata, Callback&& callback) noexcept {
  auto* builder = static_cast<MarkdownBuilder*>(userdata);
  if (builder == nullptr) {
    return 1;
  }
  try {
    return callback(*builder);
  } catch (...) {
    return 1;
  }
}

int enterBlock(MD_BLOCKTYPE type, void* detail, void* userdata) noexcept {
  return protectCallback(userdata, [type, detail](MarkdownBuilder& builder) {
    return builder.enterBlock(type, detail);
  });
}

int leaveBlock(MD_BLOCKTYPE type, void*, void* userdata) noexcept {
  return protectCallback(userdata, [type](MarkdownBuilder& builder) {
    return builder.leaveBlock(type);
  });
}

int enterSpan(MD_SPANTYPE type, void* detail, void* userdata) noexcept {
  return protectCallback(userdata, [type, detail](MarkdownBuilder& builder) {
    return builder.enterSpan(type, detail);
  });
}

int leaveSpan(MD_SPANTYPE type, void*, void* userdata) noexcept {
  return protectCallback(userdata, [type](MarkdownBuilder& builder) {
    return builder.leaveSpan(type);
  });
}

int renderText(MD_TEXTTYPE type, const MD_CHAR* text, MD_SIZE size,
               void* userdata) noexcept {
  return protectCallback(userdata,
                         [type, text, size](MarkdownBuilder& builder) {
                           return builder.text(type, text, size);
                         });
}

std::string stylePrefix(const TerminalTextStyle& style,
                        TerminalEncodingPolicy policy) {
  if (!policy.sgr_attributes) {
    return {};
  }
  std::vector<std::string> codes;
  if (style.bold) {
    codes.push_back("1");
  }
  if (style.italic) {
    codes.push_back("3");
  }
  if (style.strikethrough) {
    codes.push_back("9");
  }
  if (policy.color && (style.accent || style.code)) {
    codes.push_back("36");
  }
  if (policy.background.has_value()) {
    if (policy.truecolor) {
      codes.push_back("48");
      codes.push_back("2");
      codes.push_back(std::to_string(policy.background->red));
      codes.push_back(std::to_string(policy.background->green));
      codes.push_back(std::to_string(policy.background->blue));
    } else {
      codes.push_back("48");
      codes.push_back("5");
      codes.push_back(std::to_string(xtermResponseColorIndex(
          *policy.background, policy.terminal_background)));
    }
  }
  const bool has_semantic_style =
      style.bold || style.italic || style.strikethrough ||
      (policy.color && (style.accent || style.code));
  if (!has_semantic_style) {
    return {};
  }
  if (codes.empty()) {
    return {};
  }
  std::string prefix = "\x1b[";
  for (std::size_t index = 0; index < codes.size(); ++index) {
    if (index != 0) {
      prefix.push_back(';');
    }
    prefix += codes[index];
  }
  prefix.push_back('m');
  return prefix;
}

void ensureTranscriptEnding(std::string& output) {
  while (!output.empty() && output.back() == '\n') {
    output.pop_back();
  }
  output += "\n\n";
}

}  // namespace

TerminalCapabilities makeTerminalCapabilities(
    bool input_is_tty, bool output_is_tty, std::string_view term,
    std::optional<std::string_view> no_color, bool utf8,
    std::optional<std::size_t> columns, bool truecolor,
    std::optional<TerminalRgb> terminal_background) noexcept {
  TerminalCapabilities capabilities;
  capabilities.interactive_session = input_is_tty && output_is_tty;
  const bool vt_baseline =
      capabilities.interactive_session && !term.empty() && term != "dumb";
  capabilities.cursor_control = vt_baseline;
  capabilities.erase_line = vt_baseline;
  capabilities.sgr_attributes = vt_baseline;
  capabilities.color =
      vt_baseline && (!no_color.has_value() || no_color->empty());
  capabilities.utf8 = capabilities.interactive_session && utf8;
  capabilities.truecolor = capabilities.color && truecolor;
  if (capabilities.color && terminal_background.has_value()) {
    capabilities.terminal_background = terminal_background;
    capabilities.response_background =
        deriveResponseBackground(*terminal_background);
  }
  if (output_is_tty && columns.has_value() && *columns > 0) {
    capabilities.columns = columns;
  }
  return capabilities;
}

TerminalCapabilities detectTerminalCapabilities(std::istream& in,
                                                std::ostream& out) {
  const bool standard_input = &in == &std::cin;
  const bool standard_output = &out == &std::cout;
  const bool input_is_tty = standard_input && isatty(STDIN_FILENO) != 0;
  const bool output_is_tty = standard_output && isatty(STDOUT_FILENO) != 0;
  const std::optional<std::string> term = environmentValue("TERM");
  const std::optional<std::string> no_color = environmentValue("NO_COLOR");
  const auto terminal_background =
      colorFromColorFgbg(environmentValue("COLORFGBG"));
  return makeTerminalCapabilities(
      input_is_tty, output_is_tty, term.value_or(""),
      no_color.has_value()
          ? std::optional<std::string_view>{std::string_view(*no_color)}
          : std::nullopt,
      localeIsUtf8(),
      output_is_tty ? queryTerminalWidth(STDOUT_FILENO) : std::nullopt,
      environmentSupportsTruecolor(), terminal_background);
}

std::optional<std::size_t> queryTerminalWidth(int fd) noexcept {
  winsize size{};
  if (fd < 0 || ioctl(fd, TIOCGWINSZ, &size) != 0 || size.ws_col == 0) {
    return std::nullopt;
  }
  return static_cast<std::size_t>(size.ws_col);
}

std::optional<TerminalRgb> parseTerminalBackgroundResponse(
    std::string_view response) noexcept {
  const std::size_t marker = response.find("]11;");
  if (marker == std::string_view::npos) {
    return std::nullopt;
  }
  const std::string_view payload = response.substr(marker + 4);
  if (payload.size() >= 7 && payload.front() == '#') {
    const auto red = parseUnsigned(payload.substr(1, 2), 16, 255);
    const auto green = parseUnsigned(payload.substr(3, 2), 16, 255);
    const auto blue = parseUnsigned(payload.substr(5, 2), 16, 255);
    if (red.has_value() && green.has_value() && blue.has_value()) {
      return TerminalRgb{static_cast<std::uint8_t>(*red),
                         static_cast<std::uint8_t>(*green),
                         static_cast<std::uint8_t>(*blue)};
    }
    return std::nullopt;
  }
  if (payload.size() < 4 || payload.substr(0, 4) != "rgb:") {
    return std::nullopt;
  }

  const std::size_t first_slash = payload.find('/', 4);
  const std::size_t second_slash = first_slash == std::string_view::npos
                                       ? std::string_view::npos
                                       : payload.find('/', first_slash + 1);
  if (first_slash == std::string_view::npos ||
      second_slash == std::string_view::npos) {
    return std::nullopt;
  }
  std::size_t end = payload.find_first_of("\a\x1b", second_slash + 1);
  if (end == std::string_view::npos) {
    end = payload.size();
  }
  const std::string_view red = payload.substr(4, first_slash - 4);
  const std::string_view green =
      payload.substr(first_slash + 1, second_slash - first_slash - 1);
  const std::string_view blue =
      payload.substr(second_slash + 1, end - second_slash - 1);
  const auto valid_component = [](std::string_view component) {
    return !component.empty() && component.size() <= 4 &&
           parseUnsigned(component, 16, (1U << (component.size() * 4U)) - 1U)
               .has_value();
  };
  if (!valid_component(red) || !valid_component(green) ||
      !valid_component(blue)) {
    return std::nullopt;
  }
  return TerminalRgb{scaleHexComponent(red), scaleHexComponent(green),
                     scaleHexComponent(blue)};
}

std::optional<TerminalRgb> queryTerminalBackground(int input_fd,
                                                   int output_fd) noexcept {
  if (input_fd < 0 || output_fd < 0 || isatty(input_fd) == 0 ||
      isatty(output_fd) == 0) {
    return std::nullopt;
  }

  termios original{};
  if (tcgetattr(input_fd, &original) != 0) {
    return std::nullopt;
  }
  termios query_mode = original;
  query_mode.c_lflag &= static_cast<tcflag_t>(~(ICANON | ECHO));
  query_mode.c_cc[VMIN] = 0;
  query_mode.c_cc[VTIME] = 0;
  if (tcsetattr(input_fd, TCSANOW, &query_mode) != 0) {
    return std::nullopt;
  }

  constexpr std::string_view query = "\x1b]11;?\x1b\\";
  std::size_t written = 0;
  bool write_failed = false;
  while (written < query.size()) {
    const ssize_t result =
        write(output_fd, query.data() + written, query.size() - written);
    if (result > 0) {
      written += static_cast<std::size_t>(result);
    } else if (result < 0 && errno == EINTR) {
      continue;
    } else {
      write_failed = true;
      break;
    }
  }

  std::optional<TerminalRgb> background;
  std::array<char, kMaximumTerminalResponseBytes> response{};
  std::size_t response_size = 0;
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds{100};
  while (!write_failed && response_size < response.size()) {
    const auto remaining =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now());
    if (remaining.count() <= 0) {
      break;
    }
    pollfd descriptor{input_fd, POLLIN, 0};
    const int poll_result =
        poll(&descriptor, 1, static_cast<int>(remaining.count()));
    if (poll_result == 0) {
      break;
    }
    if (poll_result < 0) {
      if (errno == EINTR) {
        continue;
      }
      break;
    }
    const ssize_t count = read(input_fd, response.data() + response_size,
                               response.size() - response_size);
    if (count > 0) {
      response_size += static_cast<std::size_t>(count);
      background = parseTerminalBackgroundResponse(
          std::string_view(response.data(), response_size));
      if (background.has_value()) {
        break;
      }
    } else if (count < 0 && errno == EINTR) {
      continue;
    } else {
      break;
    }
  }

  tcsetattr(input_fd, TCSANOW, &original);
  return background;
}

TerminalRgb deriveResponseBackground(TerminalRgb background) noexcept {
  const bool lighten = relativeLuminance(background) < 0.5;
  const auto adjust = [lighten](std::uint8_t component) {
    if (lighten) {
      if (component == 255) {
        return component;
      }
      const unsigned distance = 255U - component;
      const unsigned increase = std::max(1U, (distance * 6U + 50U) / 100U);
      return static_cast<std::uint8_t>(
          std::min(255U, static_cast<unsigned>(component) + increase));
    }
    if (component == 0) {
      return component;
    }
    const unsigned decrease =
        std::max(1U, (static_cast<unsigned>(component) * 6U + 50U) / 100U);
    return static_cast<std::uint8_t>(component - decrease);
  };
  return {adjust(background.red), adjust(background.green),
          adjust(background.blue)};
}

std::string sanitizeTerminalText(std::string_view text) {
  std::string output;
  output.reserve(text.size());
  for (std::size_t offset = 0; offset < text.size();) {
    const auto byte = static_cast<unsigned char>(text[offset]);
    if (byte == '\r') {
      output.push_back('\n');
      ++offset;
      if (offset < text.size() && text[offset] == '\n') {
        ++offset;
      }
      continue;
    }
    if (byte == '\n' || byte == '\t') {
      output.push_back(static_cast<char>(byte));
      ++offset;
      continue;
    }
    if (byte < 0x20U || byte == 0x7FU || (byte >= 0x80U && byte <= 0x9FU)) {
      ++offset;
      continue;
    }

    const auto decoded = cli_detail::decodeCodePoint(text, offset);
    if (!decoded.valid) {
      output += kReplacementCharacter;
      ++offset;
      continue;
    }
    if (decoded.value < 0x20U ||
        (decoded.value >= 0x7FU && decoded.value <= 0x9FU)) {
      offset += decoded.bytes;
      continue;
    }
    output.append(text.substr(offset, decoded.bytes));
    offset += decoded.bytes;
  }
  return output;
}

std::string formatElapsed(std::chrono::steady_clock::duration elapsed) {
  using Seconds = std::chrono::seconds;
  auto total_seconds = std::chrono::duration_cast<Seconds>(elapsed).count();
  total_seconds = std::max<std::int64_t>(1, total_seconds);
  const std::int64_t minutes = total_seconds / 60;
  const std::int64_t seconds = total_seconds % 60;

  std::string result;
  if (minutes > 0) {
    result += std::to_string(minutes);
    result += minutes == 1 ? " minute" : " minutes";
  }
  if (seconds > 0) {
    if (!result.empty()) {
      result.push_back(' ');
    }
    result += std::to_string(seconds);
    result += seconds == 1 ? " second" : " seconds";
  }
  return result;
}

std::optional<TerminalDocument> parseMarkdown(std::string_view markdown) {
  if (markdown.size() > kMaximumMarkdownBytes ||
      markdown.size() > std::numeric_limits<MD_SIZE>::max()) {
    return std::nullopt;
  }

  MarkdownBuilder builder;
  MD_PARSER parser{};
  parser.abi_version = 0;
  parser.flags = MD_FLAG_NOHTMLBLOCKS | MD_FLAG_NOHTMLSPANS |
                 MD_FLAG_PERMISSIVEURLAUTOLINKS |
                 MD_FLAG_PERMISSIVEEMAILAUTOLINKS | MD_FLAG_STRIKETHROUGH |
                 MD_FLAG_TASKLISTS;
  parser.enter_block = enterBlock;
  parser.leave_block = leaveBlock;
  parser.enter_span = enterSpan;
  parser.leave_span = leaveSpan;
  parser.text = renderText;

  const int status =
      md_parse(markdown.empty() ? "" : markdown.data(),
               static_cast<MD_SIZE>(markdown.size()), &parser, &builder);
  if (status != 0 || builder.failed()) {
    return std::nullopt;
  }
  return builder.finish();
}

std::string encodeTerminalDocument(const TerminalDocument& document,
                                   TerminalEncodingPolicy policy) {
  std::string output;
  const std::string line_background =
      policy.background.has_value()
          ? backgroundPrefix(*policy.background, policy.truecolor,
                             policy.terminal_background)
          : std::string{};
  for (std::size_t line_index = 0; line_index < document.lines.size();
       ++line_index) {
    output += line_background;
    const auto& line = document.lines[line_index];
    for (const auto& run : line.runs) {
      const std::string prefix = stylePrefix(run.style, policy);
      if (!prefix.empty()) {
        output += prefix;
      }
      output += sanitizeTerminalText(run.text);
      if (!prefix.empty()) {
        output += "\x1b[0m";
        output += line_background;
      }
    }
    if (!line_background.empty()) {
      if (policy.erase_line) {
        output += "\x1b[K";
      }
      output += "\x1b[0m";
    }
    if (line_index + 1 < document.lines.size()) {
      output.push_back('\n');
    }
  }
  return output;
}

TranscriptRenderer::TranscriptRenderer(
    std::ostream& out, TerminalCapabilities capabilities,
    WidthQuery width_query, std::chrono::milliseconds spinner_interval)
    : out_(out),
      capabilities_(std::move(capabilities)),
      width_query_(std::move(width_query)),
      spinner_interval_(
          std::max(spinner_interval, std::chrono::milliseconds{1})) {}

TranscriptRenderer::~TranscriptRenderer() noexcept { clearActivity(); }

void TranscriptRenderer::showThinking() {
  stopSpinner();
  writeActivity("Thinking...", false);
}

void TranscriptRenderer::finishThinking(
    std::chrono::steady_clock::duration elapsed) {
  std::lock_guard<std::mutex> lock(output_mutex_);
  if (!activity_visible_) {
    return;
  }
  writeActivityLocked("Thought for " + formatElapsed(elapsed) + ".", true);
}

void TranscriptRenderer::startSpinner() {
  stopSpinner();
  const auto width = currentWidth();
  if (!capabilities_.interactive_session || !capabilities_.cursor_control ||
      !capabilities_.erase_line || !width.has_value() || *width < 4) {
    return;
  }

  {
    std::lock_guard<std::mutex> lock(output_mutex_);
    spinner_stop_requested_ = false;
    writeActivityLocked(capabilities_.utf8 ? "\xE2\xA0\x8B" : "|", false);
  }
  try {
    spinner_thread_ = std::thread(&TranscriptRenderer::runSpinner, this);
  } catch (...) {
    std::lock_guard<std::mutex> lock(output_mutex_);
    spinner_stop_requested_ = true;
    clearActivityLocked();
  }
}

void TranscriptRenderer::stopSpinner() noexcept {
  {
    std::lock_guard<std::mutex> lock(output_mutex_);
    spinner_stop_requested_ = true;
  }
  spinner_wakeup_.notify_all();
  if (spinner_thread_.joinable()) {
    spinner_thread_.join();
  }
  std::lock_guard<std::mutex> lock(output_mutex_);
  clearActivityLocked();
}

void TranscriptRenderer::clearActivity() noexcept { stopSpinner(); }

void TranscriptRenderer::renderAssistant(std::string_view markdown) {
  clearActivity();
  const std::string sanitized = sanitizeTerminalText(markdown);
  std::string encoded;
  if (capabilities_.interactive_session && capabilities_.sgr_attributes) {
    try {
      const auto document = parseMarkdown(sanitized);
      if (document.has_value()) {
        encoded = encodeTerminalDocument(
            *document, {capabilities_.sgr_attributes, capabilities_.color,
                        capabilities_.color ? capabilities_.response_background
                                            : std::nullopt,
                        capabilities_.truecolor, capabilities_.erase_line,
                        capabilities_.terminal_background});
      } else {
        encoded = sanitized;
      }
    } catch (...) {
      encoded = sanitized;
    }
  } else {
    encoded = sanitized;
  }
  ensureTranscriptEnding(encoded);
  if (capabilities_.interactive_session) {
    encoded.insert(encoded.begin(), '\n');
  }
  std::lock_guard<std::mutex> lock(output_mutex_);
  out_.write(encoded.data(), static_cast<std::streamsize>(encoded.size()));
  out_.flush();
}

void TranscriptRenderer::finishTurn() noexcept {}

std::optional<std::size_t> TranscriptRenderer::currentWidth() const {
  if (width_query_) {
    return width_query_();
  }
  return capabilities_.columns;
}

void TranscriptRenderer::writeActivity(std::string_view text, bool permanent) {
  std::lock_guard<std::mutex> lock(output_mutex_);
  writeActivityLocked(text, permanent);
}

void TranscriptRenderer::writeActivityLocked(std::string_view text,
                                             bool permanent) {
  if (!capabilities_.interactive_session || !capabilities_.cursor_control ||
      !capabilities_.erase_line) {
    return;
  }
  const std::optional<std::size_t> width = currentWidth();
  if (!width.has_value() || *width < 4) {
    return;
  }

  std::string displayed(text);
  if (displayed.size() >= *width) {
    const std::size_t retained = *width > 4 ? *width - 4 : 0;
    displayed.resize(retained);
    displayed += "...";
  }
  std::string output = "\r\x1b[2K" + displayed + "\x1b[0m";
  if (permanent) {
    output.push_back('\n');
  }
  out_.write(output.data(), static_cast<std::streamsize>(output.size()));
  out_.flush();
  activity_visible_ = !permanent;
}

void TranscriptRenderer::clearActivityLocked() noexcept {
  if (!activity_visible_) {
    return;
  }
  try {
    constexpr std::string_view clear = "\r\x1b[2K\x1b[0m";
    out_.write(clear.data(), static_cast<std::streamsize>(clear.size()));
    out_.flush();
  } catch (...) {
  }
  activity_visible_ = false;
}

void TranscriptRenderer::runSpinner() noexcept {
  constexpr std::array<std::string_view, 8> kUtf8Frames{
      "\xE2\xA0\x8B", "\xE2\xA0\x99", "\xE2\xA0\xB9", "\xE2\xA0\xB8",
      "\xE2\xA0\xBC", "\xE2\xA0\xB4", "\xE2\xA0\xA6", "\xE2\xA0\xA7"};
  constexpr std::array<std::string_view, 4> kAsciiFrames{"|", "/", "-", "\\"};
  std::size_t frame = 1;
  std::unique_lock<std::mutex> lock(output_mutex_);
  while (!spinner_stop_requested_) {
    if (spinner_wakeup_.wait_for(lock, spinner_interval_,
                                 [this] { return spinner_stop_requested_; })) {
      break;
    }
    try {
      if (capabilities_.utf8) {
        writeActivityLocked(kUtf8Frames[frame % kUtf8Frames.size()], false);
        frame = (frame + 1) % kUtf8Frames.size();
      } else {
        writeActivityLocked(kAsciiFrames[frame % kAsciiFrames.size()], false);
        frame = (frame + 1) % kAsciiFrames.size();
      }
    } catch (...) {
      spinner_stop_requested_ = true;
    }
  }
}

}  // namespace octopus
