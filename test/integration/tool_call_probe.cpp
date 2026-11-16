#include "models/alibaba/qwen3_5/integration.hpp"
#include "octopus/chat/runtime.hpp"
#include "octopus/cli/ask.hpp"
#include "octopus/inference/backend/llama_cpp_backend.hpp"

#include <iostream>

class RecordingCompiler final : public octopus::llm::PromptCompiler {
  public:
    explicit RecordingCompiler(const octopus::llm::PromptCompiler &inner) : inner_(inner) {}

    octopus::llm::CompileResult
    compile(const octopus::llm::Conversation &conversation) const override {
        auto result = inner_.compile(conversation);
        if (result.hasValue())
            prompts.push_back(result.value().text);
        return result;
    }

    mutable std::vector<std::string> prompts;

  private:
    const octopus::llm::PromptCompiler &inner_;
};

int main(int argc, char **argv) {
    if (argc != 3 && argc != 4) {
        std::cerr << "usage: tool_call_probe MODEL TOOL_ROOT [os.exec]\n";
        return 2;
    }
    octopus::inference::LlamaCppBackend backend({argv[1], 99, true});
    octopus::models::alibaba::qwen3_5::Qwen35Integration qwen(backend);
    RecordingCompiler compiler(qwen.modelIntegration().compiler());
    octopus::llm::ModelIntegration model(qwen.modelIntegration().capabilities(), compiler,
                                         qwen.modelIntegration().parser());
    octopus::llm::Runtime llm(model, backend);
    octopus::tool::Registry registry;
    octopus::tool::loadTools(registry, argv[2]);
    octopus::tool::AllowAllSecurityPolicy policy;
    octopus::tool::Executor executor(registry, policy);
    octopus::CliOptions options;
    options.n_predict = 128;
    auto base = octopus::makeConversationRequest(options, {});
    octopus::chat::Runtime chat(llm, registry, executor, base);
    if (argc == 4) {
        if (std::string(argv[3]) != "os.exec")
            return 2;
        const auto result = chat.turn("Call os.exec exactly once with program /usr/bin/printf and "
                                      "args [\"octopus-exec-probe\"]. Report the exact stdout.",
                                      nullptr);
        if (!result) {
            std::cerr << result.error() << '\n';
            return 1;
        }
        std::size_t count = 0;
        for (const auto &message : chat.history().messages) {
            if (message.role != octopus::llm::Role::Tool)
                continue;
            ++count;
            auto payload = nlohmann::json::parse(message.content);
            bool included = false;
            for (const auto &prompt : compiler.prompts)
                included |= prompt.find(message.content) != std::string::npos;
            if (payload.at("status") != "success" || !payload.at("output").is_object() ||
                payload.at("output").at("stdout") != "octopus-exec-probe" ||
                payload.at("output").at("exit_code") != 0 || !included) {
                std::cerr << "os.exec verification failed: included_in_prompt=" << included
                          << " payload=" << payload.dump() << '\n';
                return 1;
            }
            std::cout << "os.exec: id=" << message.tool_call_id
                      << " included_in_prompt=" << included << " result=" << payload.dump() << '\n';
        }
        const bool matched =
            result.value().response.text.find("octopus-exec-probe") != std::string::npos;
        if (count != 1 || !matched)
            std::cerr << "os.exec probe failed: tool_results=" << count
                      << " response=" << result.value().response.text << '\n';
        return count == 1 && matched ? 0 : 1;
    }
    std::string previous_id;
    for (int i = 0; i < 2; ++i) {
        const auto before = chat.history().messages.size();
        auto result =
            chat.turn("Call misc.hello_world and report its exact result, unchanged.", nullptr);
        if (!result) {
            std::cerr << result.error() << '\n';
            return 1;
        }
        const auto &history = chat.history().messages;
        std::size_t count = 0;
        for (std::size_t j = before; j < history.size(); ++j) {
            const auto &message = history[j];
            if (message.role != octopus::llm::Role::Tool)
                continue;
            ++count;
            auto payload = nlohmann::json::parse(message.content);
            const auto output = payload.at("output").get<std::string>();
            bool in_prompt = false;
            for (const auto &prompt : compiler.prompts)
                in_prompt = in_prompt || prompt.find(message.content) != std::string::npos;
            const bool matched = result.value().response.text.find(output) != std::string::npos;
            std::cout << "executor: id=" << message.tool_call_id << " output=" << output
                      << " included_in_prompt=" << in_prompt << " matched_answer=" << matched
                      << '\n';
            if (!in_prompt || !matched || message.tool_call_id.empty() ||
                message.tool_call_id == previous_id || payload.at("status") != "success")
                return 1;
            previous_id = message.tool_call_id;
        }
        if (count != 1) {
            std::cerr << "expected exactly one call in turn\n";
            return 1;
        }
        std::cout << "assistant: " << result.value().response.text << '\n';
    }
    const auto before_normal = chat.history().messages.size();
    auto normal = chat.turn("Reply with one word: ready", nullptr);
    if (!normal) {
        std::cerr << normal.error() << '\n';
        return 1;
    }
    for (std::size_t j = before_normal; j < chat.history().messages.size(); ++j)
        if (chat.history().messages[j].role == octopus::llm::Role::Tool)
            return 1;
    std::cout << "normal: " << normal.value().response.text << '\n';
}
