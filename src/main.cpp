#include "octopus/ask.hpp"
#include "octopus/chat.hpp"
#include "octopus/cli.hpp"
#include "octopus/llama_cpp_backend.hpp"

#include <iostream>
#include <string>
#include <vector>

namespace {

std::vector<std::string> argvToStrings(int argc, char** argv) {
  std::vector<std::string> arguments;
  arguments.reserve(static_cast<std::size_t>(argc));
  for (int index = 0; index < argc; ++index) {
    arguments.emplace_back(argv[index]);
  }
  return arguments;
}

}  // namespace

int main(int argc, char** argv) {
  const auto cli = octopus::parseCli(argvToStrings(argc, argv));
  if (cli.ok && !cli.help.empty()) {
    std::cout << cli.help;
  }
  if (!cli.error.empty()) {
    std::cerr << cli.error << '\n' << cli.help;
  }
  if (!cli.ok || !cli.help.empty()) {
    return cli.exit_code;
  }

  const auto& options = cli.options;
  /**
   * main wires CLI to the harness: parse user intent, build one concrete
   * backend, then let mode runners handle request construction and output.
   */
  octopus::LlamaCppBackend backend(
      {options.model_path, options.n_gpu_layers, options.quiet});
  if (options.mode == octopus::CliMode::Interactive) {
    return octopus::runCliChat(options, backend, std::cin, std::cout, std::cerr)
        .exit_code;
  }

  return octopus::runOneShotAsk(options, backend, std::cout, std::cerr)
      .exit_code;
}
