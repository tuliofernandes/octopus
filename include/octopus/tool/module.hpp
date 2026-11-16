#pragma once

#include "octopus/tool/tool.hpp"

#include <nlohmann/json.hpp>
#include <string>

namespace octopus::tool {
/* Tool modules must be built with the same C++ ABI and dependencies as Octopus. */
struct ToolModule {
    unsigned version;
    std::string (*validate)(const nlohmann::json &);
    Outcome (*execute)(const nlohmann::json &, const core::CancellationToken *);
    std::string (*label)(const nlohmann::json &) = nullptr;
};

using ToolModuleEntry = const ToolModule *(*)();
inline constexpr unsigned kToolModuleVersion = 2;
} /* namespace octopus::tool */
