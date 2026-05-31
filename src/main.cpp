#include "llama.h"
#include <argparse/argparse.hpp>
#include <iostream>
#include <string>
#include <vector>

int main(int argc, char **argv) {
  // path to the model gguf file
  std::string model_path;
  // prompt to generate text from
  std::string prompt = "Hello my name is";
  // number of layers to offload to the GPU
  int ngl = 99;
  // number of tokens to predict
  int n_predict = 32;

  // parse command line arguments

  argparse::ArgumentParser args("LLamaPlayground");

  args.add_argument("-m", "--model").help("The model gguf file").required();
  args.add_argument("-n", "--n_predict")
      .help("Number of tokens to predict (default: 32)")
      .scan<'i', int>()
      .default_value(n_predict);
  args.add_argument("-ngl", "--n_gpu_layers")
      .help("Number of layers to offload to the GPU (default: 99)")
      .scan<'i', int>()
      .default_value(ngl);
  args.add_argument("prompt")
      .help("The prompt to generate text from (default: 'Hello my name is')")
      .remaining()
      .default_value(std::vector<std::string>{prompt});

  try {
    args.parse_args(argc, argv);
  } catch (const std::runtime_error &err) {
    std::cerr << err.what() << std::endl;
    std::cerr << args;
    return 1;
  }

  model_path = args.get<std::string>("model");
  n_predict = args.get<int>("--n_predict");
  ngl = args.get<int>("--n_gpu_layers");
  prompt = "";
  const auto &prompt_vec = args.get<std::vector<std::string>>("prompt");
  prompt = std::accumulate(std::next(prompt_vec.begin()), prompt_vec.end(),
                           prompt_vec.empty() ? "" : prompt_vec[0],
                           [](const std::string &a, const std::string &b) {
                             return a + (a.empty() ? "" : " ") + b;
                           });

  // load dynamic backends

  ggml_backend_load_all();

  // initialize the model

  llama_model_params model_params = llama_model_default_params();
  model_params.n_gpu_layers = ngl;

  llama_model *model =
      llama_model_load_from_file(model_path.c_str(), model_params);

  if (model == nullptr) {
    std::cerr << __func__ << ": error: unable to load model" << std::endl;
    return 1;
  }

  const llama_vocab *vocab = llama_model_get_vocab(model);
  // tokenize the prompt

  // find the number of tokens in the prompt
  const int n_prompt = -llama_tokenize(vocab, prompt.c_str(), prompt.size(),
                                       nullptr, 0, true, true);

  // allocate space for the tokens and tokenize the prompt
  std::vector<llama_token> prompt_tokens(n_prompt);
  if (llama_tokenize(vocab, prompt.c_str(), prompt.size(), prompt_tokens.data(),
                     prompt_tokens.size(), true, true) < 0) {
    std::cerr << __func__ << ": error: failed to tokenize the prompt"
              << std::endl;
    return 1;
  }

  // initialize the context

  llama_context_params ctx_params = llama_context_default_params();
  // n_ctx is the context size
  ctx_params.n_ctx = n_prompt + n_predict - 1;
  // n_batch is the maximum number of tokens that can be processed in a single
  // call to llama_decode
  ctx_params.n_batch = n_prompt;
  // enable performance counters
  ctx_params.no_perf = false;

  llama_context *ctx = llama_init_from_model(model, ctx_params);

  if (ctx == nullptr) {
    std::cerr << __func__ << ": error: failed to create the llama_context"
              << std::endl;
    return 1;
  }

  // initialize the sampler

  auto sparams = llama_sampler_chain_default_params();
  sparams.no_perf = false;
  llama_sampler *smpl = llama_sampler_chain_init(sparams);

  llama_sampler_chain_add(smpl, llama_sampler_init_greedy());

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
      llama_batch_get_one(prompt_tokens.data(), prompt_tokens.size());

  // main loop

  const auto t_main_start = ggml_time_us();
  int n_decode = 0;
  llama_token new_token_id;

  for (int n_pos = 0; n_pos + batch.n_tokens < n_prompt + n_predict;) {
    // evaluate the current batch with the transformer model
    if (llama_decode(ctx, batch)) {
      std::cerr << __func__ << " : failed to eval, return code " << 1
                << std::endl;
      return 1;
    }

    n_pos += batch.n_tokens;

    // sample the next token
    {
      new_token_id = llama_sampler_sample(smpl, ctx, -1);

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
  std::cerr << __func__ << ": decoded " << n_decode << " tokens in "
            << (t_main_end - t_main_start) / 1000000.0f << " s, speed: "
            << n_decode / ((t_main_end - t_main_start) / 1000000.0f)
            << " t/s\n";

  std::cerr << std::endl;
  llama_perf_sampler_print(smpl);
  llama_perf_context_print(ctx);
  std::cerr << std::endl;

  llama_sampler_free(smpl);
  llama_free(ctx);
  llama_model_free(model);

  return 0;
}
