#include "octopus/cli/ask.hpp"
#include "chat_session.hpp"
#include "octopus/cli/cli.hpp"
#include "octopus/inference/backend/llama_cpp_backend.hpp"
#include "octopus/llm/runtime.hpp"

#include "models/alibaba/qwen3_5/integration.hpp"

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

template <typename Integration>
int runWithModel(const octopus::CliOptions& options,
                 const std::string& model_path, const std::string& command) {
  octopus::inference::LlamaCppBackend backend(
      {model_path, options.n_gpu_layers, options.quiet});
  Integration integration(backend);
  octopus::llm::Runtime runtime(integration.modelIntegration(), backend);
  if (options.mode != octopus::CliMode::Interactive) {
    return octopus::runOneShotAsk(options, runtime, std::cout, std::cerr)
        .exit_code;
  }
  return runChatSession(options, runtime, command);
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
  switch (options.model) {
    case octopus::CliModel::Qwen35:
      return runWithModel<octopus::models::alibaba::qwen3_5::Qwen35Integration>(
          options, octopus::models::alibaba::qwen3_5::kDefaultModelPath,
          argv[0]);
  }

  return 1;
}
