#include "octopus/cli.hpp"

#include "llama.h"
#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace {

std::vector<std::string> argv_to_strings(int argc, char **argv) {
  std::vector<std::string> arguments;
  arguments.reserve(static_cast<std::size_t>(argc));
  for (int index = 0; index < argc; ++index) {
    arguments.emplace_back(argv[index]);
  }
  return arguments;
}

} // namespace

int main(int argc, char **argv) {
  const auto cli = octopus::parse_cli(argv_to_strings(argc, argv));
  if (cli.ok && !cli.help.empty()) {
    std::cout << cli.help;
  }
  if (!cli.error.empty()) {
    std::cerr << cli.error << '\n' << cli.help;
  }
  if (!cli.ok || !cli.help.empty()) {
    return cli.exit_code;
  }

  const auto &options = cli.options;
  if (options.mode == octopus::CliMode::Interactive) {
    std::cerr << "octo interactive mode is not available yet" << std::endl;
    return 1;
  }

  // load dynamic backends

  if (options.quiet) {
    llama_log_set([](ggml_log_level, const char *, void *) {}, nullptr);
  }

  ggml_backend_load_all();

  // initialize the model

  llama_model_params model_params = llama_model_default_params();
  model_params.n_gpu_layers = options.n_gpu_layers;

  std::unique_ptr<llama_model, decltype(&llama_model_free)> model(
      llama_model_load_from_file(options.model_path.c_str(), model_params),
      llama_model_free);

  if (model == nullptr) {
    std::cerr << __func__ << ": error: unable to load model" << std::endl;
    return 1;
  }

  const llama_vocab *vocab = llama_model_get_vocab(model.get());
  // tokenize the prompt

  // find the number of tokens in the prompt
  const int n_prompt =
      -llama_tokenize(vocab, options.prompt.c_str(), options.prompt.size(),
                      nullptr, 0, true, true);
  if (n_prompt <= 0) {
    std::cerr << __func__ << ": error: failed to size prompt tokens"
              << std::endl;
    return 1;
  }

  // allocate space for the tokens and tokenize the prompt
  std::vector<llama_token> prompt_tokens(static_cast<std::size_t>(n_prompt));
  if (llama_tokenize(vocab, options.prompt.c_str(), options.prompt.size(),
                     prompt_tokens.data(),
                     static_cast<int32_t>(prompt_tokens.size()), true, true) <
      0) {
    std::cerr << __func__ << ": error: failed to tokenize the prompt"
              << std::endl;
    return 1;
  }

  // initialize the context

  llama_context_params ctx_params = llama_context_default_params();
  // n_ctx is the context size
  ctx_params.n_ctx = n_prompt + options.n_predict - 1;
  // n_batch is the maximum number of tokens that can be processed in a single
  // call to llama_decode
  ctx_params.n_batch = n_prompt;
  ctx_params.no_perf = options.quiet;

  std::unique_ptr<llama_context, decltype(&llama_free)> ctx(
      llama_init_from_model(model.get(), ctx_params), llama_free);

  if (ctx == nullptr) {
    std::cerr << __func__ << ": error: failed to create the llama_context"
              << std::endl;
    return 1;
  }

  // initialize the sampler

  auto sparams = llama_sampler_chain_default_params();
  sparams.no_perf = options.quiet;
  std::unique_ptr<llama_sampler, decltype(&llama_sampler_free)> smpl(
      llama_sampler_chain_init(sparams), llama_sampler_free);
  if (smpl == nullptr) {
    std::cerr << __func__ << ": error: failed to create the llama_sampler"
              << std::endl;
    return 1;
  }

  llama_sampler_chain_add(smpl.get(), llama_sampler_init_greedy());

  // print the prompt token-by-token

  for (auto id : prompt_tokens) {
    char buf[128];
    int n = llama_token_to_piece(vocab, id, buf, sizeof(buf), 0, true);
    if (n < 0) {
      std::cerr << __func__ << ": error: failed to convert token to piece"
                << std::endl;
      return 1;
    }
    std::string s(buf, n);
    std::cout << s;
  }

  // prepare a batch for the prompt

  llama_batch batch =
      llama_batch_get_one(prompt_tokens.data(),
                          static_cast<int32_t>(prompt_tokens.size()));

  // main loop

  const auto t_main_start = ggml_time_us();
  int n_decode = 0;
  llama_token new_token_id;

  for (int n_pos = 0; n_pos + batch.n_tokens < n_prompt + options.n_predict;) {
    // evaluate the current batch with the transformer model
    if (llama_decode(ctx.get(), batch)) {
      std::cerr << __func__ << " : failed to eval, return code " << 1
                << std::endl;
      return 1;
    }

    n_pos += batch.n_tokens;

    // sample the next token
    {
      new_token_id = llama_sampler_sample(smpl.get(), ctx.get(), -1);

      // is it an end of generation?
      if (llama_vocab_is_eog(vocab, new_token_id)) {
        break;
      }

      char buf[128];
      int n =
          llama_token_to_piece(vocab, new_token_id, buf, sizeof(buf), 0, true);
      if (n < 0) {
        std::cerr << __func__ << ": error: failed to convert token to piece"
                  << std::endl;
        return 1;
      }
      std::string s(buf, n);
      std::cout << s;
      // fflush(stdout); <-- removed

      // prepare the next batch with the sampled token
      batch = llama_batch_get_one(&new_token_id, 1);

      n_decode += 1;
    }
  }

  std::cout << "\n";

  const auto t_main_end = ggml_time_us();
  if (!options.quiet) {
    std::cerr << __func__ << ": decoded " << n_decode << " tokens in "
              << (t_main_end - t_main_start) / 1000000.0f << " s, speed: "
              << n_decode / ((t_main_end - t_main_start) / 1000000.0f)
              << " t/s\n";

    std::cerr << std::endl;
    llama_perf_sampler_print(smpl.get());
    llama_perf_context_print(ctx.get());
    std::cerr << std::endl;
  }

  return 0;
}
