#pragma once

#include "octopus/tool/tool.hpp"

namespace octopus::tool {

ArgumentValidator compileSchema(const nlohmann::json &schema);
void validateManifestEnvelope(const nlohmann::json &manifest);

} /* namespace octopus::tool */
