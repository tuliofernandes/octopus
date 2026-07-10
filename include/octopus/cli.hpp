#pragma once

#include <string>
#include <vector>

namespace octopus {

enum class CliMode {
  Interactive,
  Ask,
};

struct CliOptions {
  CliMode mode = CliMode::Interactive;
  std::string model_path = "./models/gemma-3-1b-it-Q4_K_M.gguf";
  std::string prompt;
  int n_predict = 512;
  int n_gpu_layers = 99;
  bool quiet = true;
};

struct CliParseResult {
  bool ok = false;
  int exit_code = 1;
  CliOptions options;
  std::string error;
  std::string help;
};

CliParseResult parse_cli(const std::vector<std::string>& arguments);

}  // namespace octopus
