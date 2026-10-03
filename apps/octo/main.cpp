#include "octopus/cli/ask.hpp"
#include "octopus/cli/chat.hpp"
#include "octopus/cli/cli.hpp"
#include "octopus/inference/backend/llama_cpp_backend.hpp"
#include "octopus/llm/runtime.hpp"

#include "models/google/gemma/integration.hpp"

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
  // The composition root owns borrowed dependencies in destruction-safe order:
  // runtime, integration, then the native backend/template engine.
  octopus::inference::LlamaCppBackend backend(
      {options.model_path, options.n_gpu_layers, options.quiet});
  octopus::models::google::gemma::GemmaIntegration gemma(backend);
  octopus::llm::Runtime runtime(gemma.modelIntegration(), backend);
  if (options.mode == octopus::CliMode::Interactive) {
    return octopus::runCliChat(options, runtime, std::cin, std::cout, std::cerr)
        .exit_code;
  }

  return octopus::runOneShotAsk(options, runtime, std::cout, std::cerr)
      .exit_code;
}
