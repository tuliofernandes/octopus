#include "octopus/cli/ask.hpp"

#include "octopus/cli/terminal_renderer.hpp"

#include <ostream>
#include <string>
#include <sys/utsname.h>
#include <utility>

namespace octopus {
namespace {

/**
 * The shared harness supplies operating context and tool-use guidance as
 * structured messages; model-specific rendering happens later.
 */
std::string systemPrompt() {
    std::string prompt = "You are Octopus, an AI agent for UNIX.";
    struct utsname host;
    if (uname(&host) == 0)
        prompt += " The local operating system is " + std::string(host.sysname) + ".";
    prompt += " Tools execute locally on this computer.";
    return prompt;
}

/**
 * Formatting examples reduce malformed calls without changing the executor's
 * literal-argv contract. They are model-neutral guidance, not argument
 * rewriting or security enforcement.
 */
constexpr const char *kDeveloperPrompt =
    "Answer the user's request directly. If you are unsure, say so. "
    "Keep the final answer brief: include only requested facts and essential columns, "
    "rather than reproducing the full command output. "
    "For process rankings, default to a compact table of PID, process name, CPU% and memory%; "
    "use other columns if the user requests them. "
    "For every request, assess whether any available tool is needed to fulfill it. "
    "Use tools when you need to retrieve information, verify facts about the environment, "
    "or carry out requested actions; the user does not need to mention tools explicitly. "
    "When you can answer adequately without tools, respond directly. "
    "Make this assessment internally without narrating it. "
    "For facts about this computer, inspect local files and interfaces with available tools. "
    "Use local system evidence, not external network services, for local machine state. "
    "A local IP means an address of a local network interface: inspect interfaces with "
    "a local utility such as ip addr. A public-IP lookup service answers a different question. "
    "Prefer a single focused command that obtains the information needed. "
    "Follow each tool's input contract exactly and use its examples to format calls. "
    "For os.exec, program is the executable and args is its argument vector. "
    "Direct-call example: {\"program\":\"printf\",\"args\":[\"%s\\n\",\"hello world\"]}. "
    "Pipeline example: {\"program\":\"/bin/sh\",\"args\":[\"-c\",\"printf hello | head -c 5\"]}. "
    "Keep separate arguments in separate array elements; only the explicit shell example "
    "passes a shell command as one argument. "
    "Use tool results as evidence. A tool status of success means the invocation completed, "
    "not necessarily that the requested operation succeeded: inspect returned exit codes "
    "and error output. A nonzero command exit code requires reviewing its diagnostics. "
    "If a command fails, revise your approach for the local "
    "operating system or explain the limitation instead of repeating the same failed call. "
    "Once you have enough information, answer the user.";

constexpr int kAskRepeatLastN = 64;
constexpr float kAskRepeatPenalty = 1.05F;

/**
 * Failed completion results make the CLI fail. Finish reasons alone,
 * including loop detection, can still carry a user-visible response.
 */
bool completionFailed(const llm::CompletionResult &completion) { return !completion.hasValue(); }

} /* namespace */

llm::CompletionRequest makeConversationRequest(const CliOptions &options,
                                               const llm::Conversation &conversation) {
    llm::CompletionRequest request;
    /**
     * Keep the conversation structured until the model integration/backend
     * renders it; CLI policy does not assemble model-specific protocol text.
     */
    request.conversation.messages.push_back({llm::Role::System, systemPrompt()});
    request.conversation.messages.push_back({llm::Role::Developer, kDeveloperPrompt});
    request.conversation.messages.insert(request.conversation.messages.end(),
                                         conversation.messages.begin(),
                                         conversation.messages.end());
    /**
     * Ask and chat share the same conservative greedy sampling profile.
     * Repeat penalties discourage repetitive output without adding randomness.
     */
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
