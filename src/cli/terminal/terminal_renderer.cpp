#include "octopus/cli/terminal_renderer.hpp"

#include "cli/terminal/text.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <optional>
#include <ostream>
#include <poll.h>
#include <sstream>
#include <string>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace octopus {
namespace {

constexpr std::string_view kReplacementCharacter = "\xEF\xBF\xBD";
constexpr std::size_t kMaximumTerminalResponseBytes = 256;

std::optional<std::string> environmentValue(const char *name) {
    const char *value = std::getenv(name);
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
    std::transform(normalized.begin(), normalized.end(), normalized.begin(),
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
    std::transform(normalized.begin(), normalized.end(), normalized.begin(),
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
        return TerminalRgb{kCube[cube / 36], kCube[(cube / 6) % 6], kCube[cube % 6]};
    }
    if (index <= 255) {
        const auto gray = static_cast<std::uint8_t>(8 + (index - 232) * 10);
        return TerminalRgb{gray, gray, gray};
    }
    return std::nullopt;
}

std::optional<TerminalRgb> colorFromColorFgbg(std::optional<std::string> value) noexcept {
    if (!value.has_value() || value->empty()) {
        return std::nullopt;
    }
    const std::size_t separator = value->find_last_of(';');
    const std::string_view background = separator == std::string::npos
                                            ? std::string_view(*value)
                                            : std::string_view(*value).substr(separator + 1);
    const auto index = parseUnsigned(background, 10, 255);
    return index.has_value() ? xtermIndexedColor(*index) : std::nullopt;
}

double relativeLuminance(TerminalRgb color) noexcept {
    const auto linearize = [](std::uint8_t component) {
        const double srgb = static_cast<double>(component) / 255.0;
        return srgb <= 0.04045 ? srgb / 12.92 : std::pow((srgb + 0.055) / 1.055, 2.4);
    };
    return 0.2126 * linearize(color.red) + 0.7152 * linearize(color.green) +
           0.0722 * linearize(color.blue);
}

unsigned nearestXtermColorIndex(TerminalRgb color, std::optional<unsigned> excluded_index,
                                std::optional<TerminalRgb> contrast_background) noexcept {
    unsigned best_index = 16;
    unsigned best_distance = std::numeric_limits<unsigned>::max();
    bool found = false;
    const double target_luminance = relativeLuminance(color);
    const double background_luminance = contrast_background.has_value()
                                            ? relativeLuminance(*contrast_background)
                                            : target_luminance;
    const bool enforce_direction =
        contrast_background.has_value() && target_luminance != background_luminance;
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
        const unsigned distance = static_cast<unsigned>(red * red + green * green + blue * blue);
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

unsigned xtermResponseColorIndex(TerminalRgb color,
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
        return "\x1b[48;2;" + std::to_string(color.red) + ";" + std::to_string(color.green) + ";" +
               std::to_string(color.blue) + "m";
    }
    return "\x1b[48;5;" + std::to_string(xtermResponseColorIndex(color, terminal_background)) + "m";
}

std::string stylePrefix(const TerminalTextStyle &style, TerminalEncodingPolicy policy) {
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
    if (style.underline) {
        codes.push_back("4");
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
            codes.push_back(std::to_string(
                xtermResponseColorIndex(*policy.background, policy.terminal_background)));
        }
    }
    const bool has_semantic_style = style.bold || style.italic || style.underline ||
                                    style.strikethrough ||
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

} /* namespace */

TerminalCapabilities
makeTerminalCapabilities(bool input_is_tty, bool output_is_tty, std::string_view term,
                         std::optional<std::string_view> no_color, bool utf8,
                         std::optional<std::size_t> columns, bool truecolor,
                         std::optional<TerminalRgb> terminal_background) noexcept {
    TerminalCapabilities capabilities;
    capabilities.interactive_session = input_is_tty && output_is_tty;
    const bool vt_baseline = capabilities.interactive_session && !term.empty() && term != "dumb";
    capabilities.cursor_control = vt_baseline;
    capabilities.erase_line = vt_baseline;
    capabilities.sgr_attributes = vt_baseline;
    capabilities.color = vt_baseline && (!no_color.has_value() || no_color->empty());
    capabilities.utf8 = capabilities.interactive_session && utf8;
    capabilities.truecolor = capabilities.color && truecolor;
    if (capabilities.color && terminal_background.has_value()) {
        capabilities.terminal_background = terminal_background;
        capabilities.response_background = deriveResponseBackground(*terminal_background);
    }
    if (output_is_tty && columns.has_value() && *columns > 0) {
        capabilities.columns = columns;
    }
    return capabilities;
}

TerminalCapabilities detectTerminalCapabilities(std::istream &in, std::ostream &out) {
    const bool standard_input = &in == &std::cin;
    const bool standard_output = &out == &std::cout;
    const bool input_is_tty = standard_input && isatty(STDIN_FILENO) != 0;
    const bool output_is_tty = standard_output && isatty(STDOUT_FILENO) != 0;
    const std::optional<std::string> term = environmentValue("TERM");
    const std::optional<std::string> no_color = environmentValue("NO_COLOR");
    const auto terminal_background = colorFromColorFgbg(environmentValue("COLORFGBG"));
    return makeTerminalCapabilities(
        input_is_tty, output_is_tty, term.value_or(""),
        no_color.has_value() ? std::optional<std::string_view>{std::string_view(*no_color)}
                             : std::nullopt,
        localeIsUtf8(), output_is_tty ? queryTerminalWidth(STDOUT_FILENO) : std::nullopt,
        environmentSupportsTruecolor(), terminal_background);
}

std::optional<std::size_t> queryTerminalWidth(int fd) noexcept {
    winsize size{};
    if (fd < 0 || ioctl(fd, TIOCGWINSZ, &size) != 0 || size.ws_col == 0) {
        return std::nullopt;
    }
    return static_cast<std::size_t>(size.ws_col);
}

std::optional<TerminalRgb> parseTerminalBackgroundResponse(std::string_view response) noexcept {
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
            return TerminalRgb{static_cast<std::uint8_t>(*red), static_cast<std::uint8_t>(*green),
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
    if (first_slash == std::string_view::npos || second_slash == std::string_view::npos) {
        return std::nullopt;
    }
    std::size_t end = payload.find_first_of("\a\x1b", second_slash + 1);
    if (end == std::string_view::npos) {
        end = payload.size();
    }
    const std::string_view red = payload.substr(4, first_slash - 4);
    const std::string_view green = payload.substr(first_slash + 1, second_slash - first_slash - 1);
    const std::string_view blue = payload.substr(second_slash + 1, end - second_slash - 1);
    const auto valid_component = [](std::string_view component) {
        return !component.empty() && component.size() <= 4 &&
               parseUnsigned(component, 16, (1U << (component.size() * 4U)) - 1U).has_value();
    };
    if (!valid_component(red) || !valid_component(green) || !valid_component(blue)) {
        return std::nullopt;
    }
    return TerminalRgb{scaleHexComponent(red), scaleHexComponent(green), scaleHexComponent(blue)};
}

std::optional<TerminalRgb> queryTerminalBackground(int input_fd, int output_fd) noexcept {
    if (input_fd < 0 || output_fd < 0 || isatty(input_fd) == 0 || isatty(output_fd) == 0) {
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
        const ssize_t result = write(output_fd, query.data() + written, query.size() - written);
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
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds{100};
    while (!write_failed && response_size < response.size()) {
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now());
        if (remaining.count() <= 0) {
            break;
        }
        pollfd descriptor{input_fd, POLLIN, 0};
        const int poll_result = poll(&descriptor, 1, static_cast<int>(remaining.count()));
        if (poll_result == 0) {
            break;
        }
        if (poll_result < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        const ssize_t count =
            read(input_fd, response.data() + response_size, response.size() - response_size);
        if (count > 0) {
            response_size += static_cast<std::size_t>(count);
            background =
                parseTerminalBackgroundResponse(std::string_view(response.data(), response_size));
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
    return {adjust(background.red), adjust(background.green), adjust(background.blue)};
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
        if (decoded.value < 0x20U || (decoded.value >= 0x7FU && decoded.value <= 0x9FU)) {
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

std::string encodeTerminalDocument(const TerminalDocument &document,
                                   TerminalEncodingPolicy policy) {
    std::string output;
    const std::string line_background =
        policy.background.has_value()
            ? backgroundPrefix(*policy.background, policy.truecolor, policy.terminal_background)
            : std::string{};
    for (std::size_t line_index = 0; line_index < document.lines.size(); ++line_index) {
        output += line_background;
        const auto &line = document.lines[line_index];
        for (const auto &run : line.runs) {
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

} /* namespace octopus */
