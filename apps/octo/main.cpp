#include "octopus/cli/ask.hpp"
#include "octopus/cli/chat.hpp"
#include "octopus/cli/cli.hpp"
#include "octopus/inference/backend/llama_cpp_backend.hpp"
#include "octopus/llm/runtime.hpp"

#include "models/alibaba/qwen3_5/integration.hpp"
#include "models/google/gemma/integration.hpp"

#include <iostream>
#include <string>
#include <vector>

namespace {

constexpr const char* kQwen35ModelPath = "./models/Qwen3.5-4B-Q4_K_M.gguf";
constexpr const char* kGemmaModelPath = "./models/gemma-3-1b-it-Q4_K_M.gguf";

std::vector<std::string> argvToStrings(int argc, char** argv) {
  std::vector<std::string> arguments;
  arguments.reserve(static_cast<std::size_t>(argc));
  for (int index = 0; index < argc; ++index) {
    arguments.emplace_back(argv[index]);
  }
  return arguments;
}

int dispatchMode(const octopus::CliOptions& options,
                 octopus::llm::Runtime& runtime) {
  if (options.mode == octopus::CliMode::Interactive) {
    return octopus::runCliChat(options, runtime, std::cin, std::cout, std::cerr)
        .exit_code;
  }

  return octopus::runOneShotAsk(options, runtime, std::cout, std::cerr)
      .exit_code;
}

template <typename Integration>
int runWithModel(const octopus::CliOptions& options,
                 const std::string& model_path) {
  // Stack declaration order is the ownership contract: runtime is destroyed
  // before the integration it borrows, and the integration before the backend.
  octopus::inference::LlamaCppBackend backend(
      {model_path, options.n_gpu_layers, options.quiet});
  Integration integration(backend);
  octopus::llm::Runtime runtime(integration.modelIntegration(), backend);
  return dispatchMode(options, runtime);
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
          options, kQwen35ModelPath);
    case octopus::CliModel::Gemma:
      return runWithModel<octopus::models::google::gemma::GemmaIntegration>(
          options, kGemmaModelPath);
  }

  return 1;
}
