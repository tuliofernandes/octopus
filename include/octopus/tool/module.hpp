#pragma once

#include "octopus/tool/tool.hpp"

#include <nlohmann/json.hpp>
#include <string>

namespace octopus::tool {

/**
 * This is a native C++ ABI, not a portable plugin wire format. Version 2
 * changes the execute return type; modules must share the host ABI and
 * dependencies and must be rebuilt together. Manifest versioning is separate.
 */
struct ToolModule {
    unsigned version;
    std::string (*validate)(const nlohmann::json &);
    Outcome (*execute)(const nlohmann::json &, const core::CancellationToken *);
    std::string (*label)(const nlohmann::json &) = nullptr;
};

using ToolModuleEntry = const ToolModule *(*)();
inline constexpr unsigned kToolModuleVersion = 2;

} /* namespace octopus::tool */
