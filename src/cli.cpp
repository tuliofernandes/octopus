#include "octopus/cli.hpp"

#include <algorithm>
#include <iterator>
#include <numeric>
#include <sstream>
#include <string>
#include <vector>

namespace octopus {
namespace {

constexpr const char *kProgramName = "octo";

std::string help_text() {
  std::ostringstream output;
  output << "Usage: " << kProgramName << " [--help]\n"
         << "       " << kProgramName << "\n"
         << "       " << kProgramName << " ask <prompt...>\n\n"
         << "Modes:\n"
         << "  " << kProgramName
         << "                  Open the interactive terminal UI (coming soon)\n"
         << "  " << kProgramName
         << " ask <prompt...>  Ask one question and print one answer\n\n"
         << "Options:\n"
         << "  -h, --help       Show this help message and exit\n";
  return output.str();
}

bool is_help_flag(const std::string &argument) {
  return argument == "-h" || argument == "--help";
}

bool asks_for_help(const std::vector<std::string> &arguments) {
  return std::find_if(arguments.begin(), arguments.end(), is_help_flag) !=
         arguments.end();
}

std::string join_prompt(const std::vector<std::string> &prompt_tokens) {
  return std::accumulate(std::next(prompt_tokens.begin()), prompt_tokens.end(),
                         prompt_tokens.empty() ? std::string{} : prompt_tokens[0],
                         [](const std::string &left,
                           const std::string &right) {
                           return left + (left.empty() ? "" : " ") + right;
                         });
}

CliParseResult error_result(const std::string &message) {
  CliParseResult result;
  result.exit_code = 1;
  result.error = message;
  result.help = help_text();
  return result;
}

bool contains_low_level_flag(const std::vector<std::string> &arguments,
                             std::string &flag) {
  const std::vector<std::string> low_level_flags{
      "-m", "--model", "-n", "--n_predict", "-ngl", "--n_gpu_layers", "-q",
      "--quiet"};

  const auto found =
      std::find_first_of(arguments.begin(), arguments.end(),
                         low_level_flags.begin(), low_level_flags.end());
  if (found == arguments.end()) {
    return false;
  }

  flag = *found;
  return true;
}

} // namespace

CliParseResult parse_cli(const std::vector<std::string> &arguments) {
  if (arguments.empty()) {
    return error_result("missing program name");
  }

  if (asks_for_help(arguments)) {
    CliParseResult result;
    result.ok = true;
    result.exit_code = 0;
    result.help = help_text();
    return result;
  }

  std::string low_level_flag;
  if (contains_low_level_flag(arguments, low_level_flag)) {
    return error_result("unsupported runtime flag: " + low_level_flag);
  }

  if (arguments.size() == 1) {
    CliParseResult result;
    result.ok = true;
    result.exit_code = 0;
    result.options.mode = CliMode::Interactive;
    return result;
  }

  if (arguments[1] != "ask") {
    return error_result("unknown mode: " + arguments[1]);
  }

  if (arguments.size() == 2) {
    return error_result("ask requires a prompt");
  }

  CliParseResult result;
  result.ok = true;
  result.exit_code = 0;
  result.options.mode = CliMode::Ask;
  result.options.prompt =
      join_prompt(std::vector<std::string>{arguments.begin() + 2,
                                           arguments.end()});

  return result;
}

} // namespace octopus
