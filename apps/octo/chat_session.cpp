#include "chat_session.hpp"

#include "octopus/cli/ask.hpp"
#include "octopus/cli/chat.hpp"
#include "octopus/tool/resources.hpp"
#include "octopus/tool/tool.hpp"

#include <exception>
#include <iostream>
#include <utility>

int runChatSession(const octopus::CliOptions &options, octopus::llm::Runtime &runtime,
                   const std::string &command) {
    try {
        octopus::tool::Registry registry;
        octopus::tool::loadTools(registry, octopus::tool::resourceRoot(command));
        octopus::tool::AllowAllSecurityPolicy policy;
        octopus::tool::Executor executor(registry, policy);
        auto base = octopus::makeConversationRequest(options, {});
        octopus::chat::Runtime chat(runtime, registry, executor, std::move(base));
        return octopus::runCliChat(options, runtime, std::cin, std::cout, std::cerr, &chat)
            .exit_code;
    } catch (const std::exception &error) {
        std::cerr << "tool initialization failed: " << error.what() << '\n';
        return 1;
    }
}
