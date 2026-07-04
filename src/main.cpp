#include "octopus/ask.hpp"
#include "octopus/cli.hpp"
#include "octopus/llama_cpp_backend.hpp"

#include <iostream>
#include <string>
#include <vector>

namespace {

std::vector<std::string> argv_to_strings(int argc, char **argv) {
  std::vector<std::string> arguments;
  arguments.reserve(static_cast<std::size_t>(argc));
  for (int index = 0; index < argc; ++index) {
    arguments.emplace_back(argv[index]);
  }
  return arguments;
}

} // namespace

int main(int argc, char **argv) {
  const auto cli = octopus::parse_cli(argv_to_strings(argc, argv));
  if (cli.ok && !cli.help.empty()) {
    std::cout << cli.help;
  }
  if (!cli.error.empty()) {
    std::cerr << cli.error << '\n' << cli.help;
  }
  if (!cli.ok || !cli.help.empty()) {
    return cli.exit_code;
  }

  const auto &options = cli.options;
  if (options.mode == octopus::CliMode::Interactive) {
    std::cerr << "octo interactive mode is not available yet" << std::endl;
    return 1;
  }

  /**
   * main wires CLI to the harness: parse user intent, build the concrete
   * backend, then let run_one_shot_ask handle request construction and output.
   */
  octopus::LlamaCppBackend backend({options.model_path, options.n_gpu_layers,
                                    options.quiet});
  return octopus::run_one_shot_ask(options, backend, std::cout, std::cerr)
      .exit_code;
}
