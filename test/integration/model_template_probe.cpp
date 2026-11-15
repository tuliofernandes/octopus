#include "octopus/inference/backend/llama_cpp_backend.hpp"
#include "octopus/llm/contracts.hpp"

#include <iostream>
#include <string>

int main(int argc, char** argv) {
  if (argc != 2 && argc != 3) {
    std::cerr << "usage: octopus_model_template_probe <model.gguf> [tools]\n";
    return 2;
  }

  octopus::inference::LlamaCppBackend backend({argv[1], 0, true});
  octopus::llm::ChatTemplateRequest request;
  request.messages.push_back({"user", "Say hello."});
  if (argc == 3 && std::string(argv[2]) == "tools") {
    request.tools.push_back({"misc",
                             "hello_world",
                             "Returns a greeting",
                             {{"type", "object"},
                              {"properties", nlohmann::json::object()},
                              {"additionalProperties", false}},
                             {}});
    request.messages.push_back(
        {"assistant",
         "",
         {{"call-1", "misc.hello_world", nlohmann::json::object()}},
         {}});
    request.messages.push_back(
        {"tool", "Hello, world! Random number 1234", {}, "call-1"});
  }
  request.add_generation_prompt = true;
  request.reasoning = octopus::llm::TemplateReasoningPolicy::Disabled;

  const auto rendered = backend.render(request);
  if (!rendered.hasValue()) {
    std::cerr << rendered.error() << '\n';
    return 1;
  }

  std::cout << rendered.value();
  return 0;
}
