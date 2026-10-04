#include "cli/terminal/text.hpp"

#include <algorithm>

namespace octopus::cli_detail {
namespace {

bool isContinuation(unsigned char byte) noexcept {
  return (byte & 0xC0U) == 0x80U;
}

bool isCombining(std::uint32_t value) noexcept {
  return (value >= 0x0300U && value <= 0x036FU) ||
         (value >= 0x1AB0U && value <= 0x1AFFU) ||
         (value >= 0x1DC0U && value <= 0x1DFFU) ||
         (value >= 0x20D0U && value <= 0x20FFU) ||
         (value >= 0xFE20U && value <= 0xFE2FU);
}

bool isWide(std::uint32_t value) noexcept {
  return value >= 0x1100U &&
         (value <= 0x115FU || value == 0x2329U || value == 0x232AU ||
          (value >= 0x2E80U && value <= 0xA4CFU && value != 0x303FU) ||
          (value >= 0xAC00U && value <= 0xD7A3U) ||
          (value >= 0xF900U && value <= 0xFAFFU) ||
          (value >= 0xFE10U && value <= 0xFE19U) ||
          (value >= 0xFE30U && value <= 0xFE6FU) ||
          (value >= 0xFF00U && value <= 0xFF60U) ||
          (value >= 0xFFE0U && value <= 0xFFE6U) ||
          (value >= 0x1F300U && value <= 0x1FAFFU) ||
          (value >= 0x20000U && value <= 0x3FFFDU));
}

}  // namespace

DecodedCodePoint decodeCodePoint(std::string_view text,
                                 std::size_t offset) noexcept {
  if (offset >= text.size()) {
    return {};
  }

  const auto first = static_cast<unsigned char>(text[offset]);
  if (first < 0x80U) {
    return {first, 1, true};
  }

  std::size_t length = 0;
  std::uint32_t value = 0;
  std::uint32_t minimum = 0;
  if (first >= 0xC2U && first <= 0xDFU) {
    length = 2;
    value = first & 0x1FU;
    minimum = 0x80U;
  } else if (first >= 0xE0U && first <= 0xEFU) {
    length = 3;
    value = first & 0x0FU;
    minimum = 0x800U;
  } else if (first >= 0xF0U && first <= 0xF4U) {
    length = 4;
    value = first & 0x07U;
    minimum = 0x10000U;
  } else {
    return {};
  }

  if (length > text.size() - offset) {
    return {};
  }
  for (std::size_t index = 1; index < length; ++index) {
    const auto byte = static_cast<unsigned char>(text[offset + index]);
    if (!isContinuation(byte)) {
      return {};
    }
    value = (value << 6U) | (byte & 0x3FU);
  }

  if (value < minimum || value > 0x10FFFFU ||
      (value >= 0xD800U && value <= 0xDFFFU)) {
    return {};
  }
  return {value, length, true};
}

std::size_t previousCodePointBoundary(std::string_view text,
                                      std::size_t offset) noexcept {
  offset = std::min(offset, text.size());
  if (offset == 0) {
    return 0;
  }
  --offset;
  while (offset > 0 &&
         isContinuation(static_cast<unsigned char>(text[offset]))) {
    --offset;
  }
  return offset;
}

std::size_t nextCodePointBoundary(std::string_view text,
                                  std::size_t offset) noexcept {
  if (offset >= text.size()) {
    return text.size();
  }
  const auto decoded = decodeCodePoint(text, offset);
  return std::min(text.size(), offset + (decoded.valid ? decoded.bytes : 1));
}

std::size_t terminalCellWidth(std::uint32_t code_point) noexcept {
  if (code_point == 0 || code_point < 0x20U ||
      (code_point >= 0x7FU && code_point <= 0x9FU) || isCombining(code_point)) {
    return 0;
  }
  return isWide(code_point) ? 2 : 1;
}

std::size_t textCellWidth(std::string_view text) noexcept {
  std::size_t width = 0;
  for (std::size_t offset = 0; offset < text.size();) {
    const auto decoded = decodeCodePoint(text, offset);
    width += decoded.valid ? terminalCellWidth(decoded.value) : 1;
    offset += decoded.valid ? decoded.bytes : 1;
  }
  return width;
}

}  // namespace octopus::cli_detail
