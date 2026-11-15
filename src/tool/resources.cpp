#include "octopus/tool/resources.hpp"

#include <cstdlib>

namespace octopus::tool {
std::filesystem::path resourceRoot(const std::string &command) {
    namespace fs = std::filesystem;
    fs::path executable(command);
    if (!executable.has_parent_path()) {
        const char *path = std::getenv("PATH");
        std::string paths = path ? path : "";
        std::size_t start = 0;
        while (start <= paths.size()) {
            const auto end = paths.find(':', start);
            const auto directory = paths.substr(start, end - start);
            const auto candidate = fs::path(directory.empty() ? "." : directory) / executable;
            if (fs::exists(candidate)) {
                executable = candidate;
                break;
            }
            if (end == std::string::npos)
                break;
            start = end + 1;
        }
    }
    const auto directory = fs::canonical(executable).parent_path();
    const auto build = directory / "share/octopus/tools";
    if (fs::exists(build))
        return build;
    return directory.parent_path() / "share/octopus/tools";
}

} // namespace octopus::tool
