#pragma once

#include "octopus/llm/types.hpp"

#include <string>
#include <vector>

namespace octopus::llm {

struct ModelCapabilities {
    bool tools = false;
    bool structured_output = false;
    bool reasoning = false;
};

struct CompiledPrompt {
    std::string text;
    std::vector<std::string> stop_strings;
};

using CompileResult = Result<CompiledPrompt>;
using ParseResult = Result<AssistantResponse>;

struct TemplateMessage {
    std::string role;
    std::string content;
    std::vector<tool::Call> tool_calls{};
    std::string tool_call_id{};
};

enum class TemplateReasoningPolicy {
    ModelDefault,
    Disabled,
};

struct ChatTemplateRequest {
    std::vector<TemplateMessage> messages;
    bool add_generation_prompt = true;
    TemplateReasoningPolicy reasoning = TemplateReasoningPolicy::ModelDefault;
    std::vector<tool::Definition> tools{};
};

using TemplateRenderResult = Result<std::string>;

class PromptCompiler {
  public:
    virtual ~PromptCompiler() = default;
    virtual CompileResult compile(const Conversation &conversation) const = 0;
};

class AssistantResponseParser {
  public:
    virtual ~AssistantResponseParser() = default;
    virtual ParseResult parse(const RawCompletion &completion) const = 0;

    virtual ParseResult parse(const RawCompletion &completion,
                              const std::vector<tool::Definition> &) const {
        return parse(completion);
    }
};

class InferenceBackend {
  public:
    virtual ~InferenceBackend() = default;
    virtual InferenceResult generate(const CompiledPrompt &prompt,
                                     const GenerationOptions &generation,
                                     const CancellationToken *cancellation,
                                     CompletionSink *sink) = 0;
};

class ChatTemplateEngine {
  public:
    virtual ~ChatTemplateEngine() = default;
    /* The engine may borrow request storage only for this blocking call. Results */
    /* own rendered text and never expose backend-native handles or string views. */
    virtual TemplateRenderResult render(const ChatTemplateRequest &request) const = 0;
};

/**
 * Borrows compiler and parser; both must outlive this integration and any
 * runtime that uses it. The owning model integration keeps them stable.
 */
class ModelIntegration final {
  public:
    ModelIntegration(ModelCapabilities capabilities, const PromptCompiler &compiler,
                     const AssistantResponseParser &parser)
        : capabilities_(capabilities), compiler_(compiler), parser_(parser) {}

    ModelIntegration(const ModelIntegration &) = delete;
    ModelIntegration &operator=(const ModelIntegration &) = delete;
    ModelIntegration(ModelIntegration &&) = delete;
    ModelIntegration &operator=(ModelIntegration &&) = delete;

    const ModelCapabilities &capabilities() const noexcept { return capabilities_; }

    const PromptCompiler &compiler() const noexcept { return compiler_; }

    const AssistantResponseParser &parser() const noexcept { return parser_; }

  private:
    ModelCapabilities capabilities_;
    const PromptCompiler &compiler_;
    const AssistantResponseParser &parser_;
};

} /* namespace octopus::llm */
