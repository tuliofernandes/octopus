#include "octopus/cli/ask.hpp"
#include "octopus/cli/chat.hpp"
#include "octopus/cli/cli.hpp"
#include "octopus/tool/tool.hpp"
#include "octopus/inference/backend/llama_cpp_backend.hpp"
#include "octopus/llm/runtime.hpp"

#include "models/alibaba/qwen3_5/integration.hpp"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

constexpr const char* kQwen35ModelPath = "./models/Qwen3.5-4B-Q4_K_M.gguf";

std::vector<std::string> argvToStrings(int argc, char** argv) {
  std::vector<std::string> arguments;
  arguments.reserve(static_cast<std::size_t>(argc));
  for (int index = 0; index < argc; ++index) {
    arguments.emplace_back(argv[index]);
  }
  return arguments;
}

std::filesystem::path resourceRoot(const std::string& command) {
  namespace fs = std::filesystem;
  fs::path executable(command);
  if (!executable.has_parent_path()) {
    const char* path = std::getenv("PATH");
    std::string paths = path ? path : "";
    std::size_t start = 0;
    while (start <= paths.size()) {
      const auto end = paths.find(':', start);
      const auto directory = paths.substr(start, end - start);
      const auto candidate =
          fs::path(directory.empty() ? "." : directory) / executable;
      if (fs::exists(candidate)) {
        executable = candidate;
        break;
      }
      if (end == std::string::npos) break;
      start = end + 1;
    }
  }
  const auto directory = fs::canonical(executable).parent_path();
  const auto build = directory / "share/octopus/tools";
  if (fs::exists(build)) return build;
  return directory.parent_path() / "share/octopus/tools";
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
  try {
    octopus::tool::Registry registry;
    octopus::tool::registerBuiltins(registry, resourceRoot(command));
    octopus::tool::AllowAllSecurityPolicy policy;
    octopus::tool::Executor executor(registry, policy);
    auto base = octopus::makeConversationRequest(options, {});
    octopus::chat::Runtime chat(runtime, registry, executor, std::move(base));
    return octopus::runCliChat(options, runtime, std::cin, std::cout, std::cerr,
                               &chat)
        .exit_code;
  } catch (const std::exception& error) {
    std::cerr << "tool initialization failed: " << error.what() << '\n';
    return 1;
  }
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
          options, kQwen35ModelPath, argv[0]);
  }

  return 1;
}
