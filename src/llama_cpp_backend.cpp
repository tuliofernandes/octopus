#include "octopus/llama_cpp_backend.hpp"

#include "octopus/completion.hpp"
#include "octopus/prompt.hpp"

#include "llama.h"

#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace octopus {
namespace {

CompletionResult backend_error(std::string message) {
  CompletionResult result;
  result.finish_reason = FinishReason::BackendError;
  result.error = std::move(message);
  return result;
}

bool fits_int32(std::size_t value) {
  return value <= static_cast<std::size_t>(std::numeric_limits<int32_t>::max());
}

bool fits_uint32(std::size_t value) {
  return value <=
         static_cast<std::size_t>(std::numeric_limits<uint32_t>::max());
}

CompletionResult tokenize_prompt(const llama_vocab *vocab,
                                 const std::string &prompt,
                                 std::vector<llama_token> &tokens) {
  if (!fits_int32(prompt.size())) {
    return backend_error("prompt is too large to tokenize");
  }

  const auto prompt_size = static_cast<int32_t>(prompt.size());
  const int32_t sized =
      llama_tokenize(vocab, prompt.c_str(), prompt_size, nullptr, 0, true, true);
  if (sized == std::numeric_limits<int32_t>::min() || sized >= 0) {
    return backend_error("failed to size prompt tokens");
  }

  const int32_t token_count = -sized;
  if (token_count <= 0) {
    return backend_error("prompt produced no tokens");
  }

  tokens.resize(static_cast<std::size_t>(token_count));
  const int32_t actual = llama_tokenize(
      vocab, prompt.c_str(), prompt_size, tokens.data(), token_count, true, true);
  if (actual != token_count) {
    return backend_error("failed to tokenize prompt");
  }

  CompletionResult ok;
  return ok;
}

CompletionResult token_to_piece(const llama_vocab *vocab, llama_token token,
                                std::string &piece) {
  std::vector<char> buffer(128);
  int32_t written = llama_token_to_piece(
      vocab, token, buffer.data(), static_cast<int32_t>(buffer.size()), 0, true);

  if (written < 0) {
    const auto needed = static_cast<std::size_t>(-written);
    if (!fits_int32(needed) || needed == 0) {
      return backend_error("failed to size token piece");
    }

    buffer.resize(needed);
    written = llama_token_to_piece(vocab, token, buffer.data(),
                                   static_cast<int32_t>(buffer.size()), 0, true);
  }

  if (written < 0 ||
      static_cast<std::size_t>(written) > static_cast<std::size_t>(buffer.size())) {
    return backend_error("failed to convert token to piece");
  }

  piece.assign(buffer.data(), static_cast<std::size_t>(written));
  CompletionResult ok;
  return ok;
}

bool failed(const CompletionResult &result) {
  return result.finish_reason == FinishReason::BackendError;
}

} // namespace

struct LlamaCppBackend::Impl {
  explicit Impl(LlamaCppBackendOptions init_options)
      : options(std::move(init_options)),
        model(nullptr, llama_model_free) {
    if (options.quiet) {
      llama_log_set([](ggml_log_level, const char *, void *) {}, nullptr);
    }

    ggml_backend_load_all();

    llama_model_params model_params = llama_model_default_params();
    model_params.n_gpu_layers = options.n_gpu_layers;
    model.reset(llama_model_load_from_file(options.model_path.c_str(),
                                           model_params));
  }

  CompletionResult complete(const CompletionRequest &request) {
    const auto validation = validate_generation_options(request.generation);
    if (!validation.ok) {
      return backend_error(validation.error);
    }

    const auto rendered =
        render_prompt(request.conversation, request.model_profile);
    std::vector<std::string> stop_strings = rendered.stop_strings;
    stop_strings.insert(stop_strings.end(),
                        request.generation.stop_strings.begin(),
                        request.generation.stop_strings.end());
    StopDetector stop_detector(std::move(stop_strings));
    LoopDetector loop_detector;

    if (model == nullptr) {
      return backend_error("unable to load model: " + options.model_path);
    }

    const llama_vocab *vocab = llama_model_get_vocab(model.get());
    if (vocab == nullptr) {
      return backend_error("model does not expose a vocabulary");
    }

    std::vector<llama_token> prompt_tokens;
    auto tokenized = tokenize_prompt(vocab, rendered.text, prompt_tokens);
    if (failed(tokenized)) {
      return tokenized;
    }

    const auto context_tokens =
        prompt_tokens.size() +
        static_cast<std::size_t>(request.generation.max_tokens);
    if (!fits_uint32(context_tokens) || !fits_int32(prompt_tokens.size())) {
      return backend_error("requested context is too large");
    }

    llama_context_params ctx_params = llama_context_default_params();
    ctx_params.n_ctx = static_cast<uint32_t>(context_tokens);
    ctx_params.n_batch = static_cast<uint32_t>(prompt_tokens.size());
    ctx_params.no_perf = options.quiet || request.generation.quiet;

    std::unique_ptr<llama_context, decltype(&llama_free)> ctx(
        llama_init_from_model(model.get(), ctx_params), llama_free);
    if (ctx == nullptr) {
      return backend_error("failed to create llama context");
    }

    auto sampler_params = llama_sampler_chain_default_params();
    sampler_params.no_perf = options.quiet || request.generation.quiet;
    std::unique_ptr<llama_sampler, decltype(&llama_sampler_free)> sampler(
        llama_sampler_chain_init(sampler_params), llama_sampler_free);
    if (sampler == nullptr) {
      return backend_error("failed to create llama sampler");
    }
    if (repeat_penalty_enabled(request.generation)) {
      std::unique_ptr<llama_sampler, decltype(&llama_sampler_free)> penalties(
          llama_sampler_init_penalties(
              request.generation.repeat_last_n,
              request.generation.repeat_penalty,
              request.generation.frequency_penalty,
              request.generation.presence_penalty),
          llama_sampler_free);
      if (penalties == nullptr) {
        return backend_error("failed to create penalties sampler");
      }
      llama_sampler_chain_add(sampler.get(), penalties.release());
    }
    std::unique_ptr<llama_sampler, decltype(&llama_sampler_free)> greedy(
        llama_sampler_init_greedy(), llama_sampler_free);
    if (greedy == nullptr) {
      return backend_error("failed to create greedy sampler");
    }
    llama_sampler_chain_add(sampler.get(), greedy.release());

    llama_batch batch =
        llama_batch_get_one(prompt_tokens.data(),
                            static_cast<int32_t>(prompt_tokens.size()));

    CompletionResult result;
    result.finish_reason = FinishReason::MaxTokens;

    llama_token sampled_token = LLAMA_TOKEN_NULL;
    while (result.generated_tokens < request.generation.max_tokens) {
      if (llama_decode(ctx.get(), batch)) {
        return backend_error("failed to evaluate llama batch");
      }

      sampled_token = llama_sampler_sample(sampler.get(), ctx.get(), -1);

      if (llama_vocab_is_eog(vocab, sampled_token)) {
        result.finish_reason = FinishReason::EndOfGeneration;
        break;
      }

      std::string piece;
      auto converted = token_to_piece(vocab, sampled_token, piece);
      if (failed(converted)) {
        return converted;
      }

      ++result.generated_tokens;

      if (stop_detector.append(piece)) {
        result.finish_reason = FinishReason::Stop;
        break;
      }

      if (loop_detector.append(piece)) {
        stop_detector.truncate(loop_detector.trim_size());
        result.finish_reason = FinishReason::LoopDetected;
        break;
      }

      batch = llama_batch_get_one(&sampled_token, 1);
    }

    result.text = stop_detector.text();
    return result;
  }

  LlamaCppBackendOptions options;
  std::unique_ptr<llama_model, decltype(&llama_model_free)> model;
};

LlamaCppBackend::LlamaCppBackend(LlamaCppBackendOptions options)
    : impl_(std::make_unique<Impl>(std::move(options))) {}

LlamaCppBackend::~LlamaCppBackend() = default;

LlamaCppBackend::LlamaCppBackend(LlamaCppBackend &&) noexcept = default;

LlamaCppBackend &
LlamaCppBackend::operator=(LlamaCppBackend &&) noexcept = default;

CompletionResult
LlamaCppBackend::complete(const CompletionRequest &request) {
  return impl_->complete(request);
}

} // namespace octopus
