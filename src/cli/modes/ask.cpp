#include "octopus/cli/ask.hpp"

#include "octopus/cli/terminal_renderer.hpp"

#include <ostream>
#include <string>
#include <sys/utsname.h>
#include <utility>

namespace octopus {
namespace {

/* These prompts are the first Octopus policy layer. They are intentionally */
/* short: the harness should shape behavior without drowning out the user. */
std::string systemPrompt() {
    std::string prompt = "You are Octopus, an AI agent for UNIX.";
    struct utsname host;
    if (uname(&host) == 0)
        prompt += " The local operating system is " + std::string(host.sysname) + ".";
    prompt += " Tools execute locally on this computer.";
    return prompt;
}

constexpr const char *kDeveloperPrompt =
    "Answer the user's request directly. If you are unsure, say so. "
    "For every request, assess whether any available tool is needed to fulfill it. "
    "Use tools when you need to retrieve information, verify facts about the environment, "
    "or carry out requested actions; the user does not need to mention tools explicitly. "
    "When you can answer adequately without tools, respond directly. "
    "Make this assessment internally without narrating it. "
    "For facts about this computer, inspect the local system with available tools. "
    "Use tool results as evidence. If a command fails, revise your approach for the local "
    "operating system or explain the limitation instead of repeating the same failed call. "
    "Once you have enough information, answer the user.";

constexpr int kAskRepeatLastN = 64;
constexpr float kAskRepeatPenalty = 1.05F;

/* Only transport/backend failures should make the CLI fail. Other finish */
/* reasons, including loop detection, can still produce user-visible text. */
bool completionFailed(const llm::CompletionResult &completion) { return !completion.hasValue(); }

} /* namespace */

llm::CompletionRequest makeConversationRequest(const CliOptions &options,
                                               const llm::Conversation &conversation) {
    llm::CompletionRequest request;
    /* Keep the conversation structured as long as possible. Rendering to one */
    /* model-specific prompt string happens later in the backend. */
    request.conversation.messages.push_back({llm::Role::System, systemPrompt()});
    request.conversation.messages.push_back({llm::Role::Developer, kDeveloperPrompt});
    request.conversation.messages.insert(request.conversation.messages.end(),
                                         conversation.messages.begin(),
                                         conversation.messages.end());
    /* CLI requests are deterministic and conservative for now: useful for */
    /* testing, reproducibility, and reducing small-model repetition. */
    request.generation.max_tokens = options.n_predict;
    request.generation.sampler_profile = llm::SamplerProfile::Deterministic;
    request.generation.quiet = options.quiet;
    request.generation.repeat_last_n = kAskRepeatLastN;
    request.generation.repeat_penalty = kAskRepeatPenalty;
    request.generation.frequency_penalty = 0.0F;
    request.generation.presence_penalty = 0.0F;
    return request;
}

llm::CompletionRequest makeAskRequest(const CliOptions &options) {
    llm::Conversation conversation;
    conversation.messages.push_back({llm::Role::User, options.prompt});
    return makeConversationRequest(options, conversation);
}

AskRunResult runOneShotAsk(const CliOptions &options, llm::Runtime &runtime, std::ostream &out,
                           std::ostream &err) {
    auto completion = runtime.complete(makeAskRequest(options));

    if (completionFailed(completion)) {
        err << (completion.error().empty() ? "LLM backend error" : completion.error()) << '\n';
        return {1, std::move(completion)};
    }

    const std::string text = sanitizeTerminalText(completion.value().response.text);
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    out.put('\n');
    return {0, std::move(completion)};
}

} /* namespace octopus */
