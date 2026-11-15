#pragma once

#include <filesystem>
#include <string>

namespace octopus::tool {
std::filesystem::path resourceRoot(const std::string& command);
}
