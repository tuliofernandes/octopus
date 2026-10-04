#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace octopus::cli_detail {

struct DecodedCodePoint {
  std::uint32_t value = 0xFFFD;
  std::size_t bytes = 1;
  bool valid = false;
};

DecodedCodePoint decodeCodePoint(std::string_view text,
                                 std::size_t offset) noexcept;
std::size_t previousCodePointBoundary(std::string_view text,
                                      std::size_t offset) noexcept;
std::size_t nextCodePointBoundary(std::string_view text,
                                  std::size_t offset) noexcept;
std::size_t terminalCellWidth(std::uint32_t code_point) noexcept;
std::size_t textCellWidth(std::string_view text) noexcept;

}  // namespace octopus::cli_detail
