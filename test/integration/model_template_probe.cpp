#include "octopus/inference/backend/llama_cpp_backend.hpp"
#include "octopus/llm/contracts.hpp"

#include <iostream>
#include <string>

int main(int argc, char** argv) {
  if (argc != 2) {
    std::cerr << "usage: octopus_model_template_probe <model.gguf>\n";
    return 2;
  }

  octopus::inference::LlamaCppBackend backend({argv[1], 0, true});
  octopus::llm::ChatTemplateRequest request;
  request.messages.push_back({"user", "Say hello."});
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
