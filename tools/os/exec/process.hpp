#pragma once

#include "octopus/tool/tool.hpp"

namespace octopus::tool::os_exec {

struct CommandRequest {
    std::string program;
    std::vector<std::string> args;
};

Outcome run(const CommandRequest &request, const core::CancellationToken *cancellation);

} /* namespace octopus::tool::os_exec */
