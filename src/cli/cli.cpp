#include "octopus/cli/cli.hpp"

#include <algorithm>
#include <iterator>
#include <numeric>
#include <sstream>
#include <string>
#include <vector>

namespace octopus {
namespace {

constexpr const char* kProgramName = "octo";

std::string helpText() {
  std::ostringstream output;
  output << "Usage: " << kProgramName << " [--help]\n"
         << "       " << kProgramName << " [--model <qwen35|gemma>]\n"
         << "       " << kProgramName
         << " [--model <qwen35|gemma>] ask <prompt...>\n\n"
         << "Modes:\n"
         << "  " << kProgramName
         << "                  Start a pure CLI multi-turn chat\n"
         << "  " << kProgramName
         << " ask <prompt...>  Ask one question and print one answer\n\n"
         << "Options:\n"
         << "  --model <qwen35|gemma>  Select the model (default: gemma)\n"
         << "  -h, --help              Show this help message and exit\n\n"
         << "Examples:\n"
         << "  " << kProgramName << " --model gemma ask \"Say hello\"\n";
  return output.str();
}

bool isHelpFlag(const std::string& argument) {
  return argument == "-h" || argument == "--help";
}

bool asksForHelp(const std::vector<std::string>& arguments) {
  return std::find_if(arguments.begin(), arguments.end(), isHelpFlag) !=
         arguments.end();
}

std::string joinPrompt(const std::vector<std::string>& prompt_tokens) {
  return std::accumulate(
      std::next(prompt_tokens.begin()), prompt_tokens.end(),
      prompt_tokens.empty() ? std::string{} : prompt_tokens[0],
      [](const std::string& left, const std::string& right) {
        return left + (left.empty() ? "" : " ") + right;
      });
}

CliParseResult errorResult(const std::string& message) {
  CliParseResult result;
  result.exit_code = 1;
  result.error = message;
  result.help = helpText();
  return result;
}

bool containsLowLevelFlag(const std::vector<std::string>& arguments,
                          std::string& flag) {
  const std::vector<std::string> low_level_flags{
      "-m", "-n", "--n_predict", "-ngl", "--n_gpu_layers", "-q", "--quiet"};

  const auto found =
      std::find_first_of(arguments.begin(), arguments.end(),
                         low_level_flags.begin(), low_level_flags.end());
  if (found == arguments.end()) {
    return false;
  }

  flag = *found;
  return true;
}

}  // namespace

CliParseResult parseCli(const std::vector<std::string>& arguments) {
  if (arguments.empty()) {
    return errorResult("missing program name");
  }

  if (asksForHelp(arguments)) {
    CliParseResult result;
    result.ok = true;
    result.exit_code = 0;
    result.help = helpText();
    return result;
  }

  std::string low_level_flag;
  if (containsLowLevelFlag(arguments, low_level_flag)) {
    return errorResult("unsupported runtime flag: " + low_level_flag);
  }

  CliParseResult result;
  std::size_t cursor = 1;
  bool model_seen = false;
  while (cursor < arguments.size() && arguments[cursor] == "--model") {
    if (model_seen) {
      return errorResult("--model may not be specified more than once");
    }
    model_seen = true;
    if (cursor + 1 >= arguments.size()) {
      return errorResult("--model requires one of: qwen35|gemma");
    }

    const auto& model = arguments[cursor + 1];
    if (model == "qwen35") {
      result.options.model = CliModel::Qwen35;
    } else if (model == "gemma") {
      result.options.model = CliModel::Gemma;
    } else {
      return errorResult("invalid --model value; expected qwen35|gemma");
    }
    cursor += 2;
  }

  if (cursor == arguments.size()) {
    result.ok = true;
    result.exit_code = 0;
    result.options.mode = CliMode::Interactive;
    return result;
  }

  if (arguments[cursor].rfind("--model=", 0) == 0) {
    return errorResult("unsupported option: " + arguments[cursor]);
  }

  if (arguments[cursor] != "ask") {
    return errorResult("unknown mode: " + arguments[cursor]);
  }

  ++cursor;
  if (cursor == arguments.size()) {
    return errorResult("ask requires a prompt");
  }

  if (std::find(arguments.begin() + static_cast<std::ptrdiff_t>(cursor),
                arguments.end(), "--model") != arguments.end()) {
    return errorResult("--model must precede the mode");
  }
  const auto equals_model =
      std::find_if(arguments.begin() + static_cast<std::ptrdiff_t>(cursor),
                   arguments.end(), [](const std::string& argument) {
                     return argument.rfind("--model=", 0) == 0;
                   });
  if (equals_model != arguments.end()) {
    return errorResult("unsupported option: " + *equals_model);
  }

  result.ok = true;
  result.exit_code = 0;
  result.options.mode = CliMode::Ask;
  result.options.prompt = joinPrompt(std::vector<std::string>{
      arguments.begin() + static_cast<std::ptrdiff_t>(cursor),
      arguments.end()});

  return result;
}

}  // namespace octopus
