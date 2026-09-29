#include "thin_agent/local/GgufModel.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <stdexcept>

#ifdef THIN_AGENT_WITH_LLAMA_CPP
#include "llama.h"
#endif

namespace thin_agent {
namespace local {

GgufModel::GgufModel(const std::string& model_path,
                     std::vector<ModelCapability> caps,
                     int n_ctx, int n_threads)
    : model_path_(model_path),
      caps_(std::move(caps)),
      n_ctx_(n_ctx),
      n_threads_(n_threads) {
  auto pos = model_path_.find_last_of("/\\");
  name_ = pos != std::string::npos ? model_path_.substr(pos + 1) : model_path_;
  if (auto dot = name_.rfind('.'); dot != std::string::npos) {
    name_ = name_.substr(0, dot);
  }
}

GgufModel::~GgufModel() {
#ifdef THIN_AGENT_WITH_LLAMA_CPP
  if (sampler_) llama_sampler_free(static_cast<llama_sampler*>(sampler_));
  if (ctx_) llama_free(static_cast<llama_context*>(ctx_));
  if (model_) llama_model_free(static_cast<llama_model*>(model_));
#endif
}

void GgufModel::unload() {
#ifdef THIN_AGENT_WITH_LLAMA_CPP
  if (sampler_) { llama_sampler_free(static_cast<llama_sampler*>(sampler_)); sampler_ = nullptr; }
  if (ctx_) { llama_free(static_cast<llama_context*>(ctx_)); ctx_ = nullptr; }
  if (model_) { llama_model_free(static_cast<llama_model*>(model_)); model_ = nullptr; }
#endif
  vocab_ = nullptr;
  memory_bytes_ = 0;
  loaded_ = false;
}

bool GgufModel::load() {
#ifndef THIN_AGENT_WITH_LLAMA_CPP
  return false;
#else
  if (loaded_) return true;
  if (!std::filesystem::exists(model_path_)) return false;

  llama_model_params model_params = llama_model_default_params();
  llama_context_params ctx_params = llama_context_default_params();
  ctx_params.n_ctx = n_ctx_;
  ctx_params.n_threads = n_threads_;
  ctx_params.n_threads_batch = n_threads_;

  llama_model* m = llama_load_model_from_file(model_path_.c_str(), model_params);
  if (!m) return false;
  model_ = m;

  llama_context* c = llama_new_context_with_model(m, ctx_params);
  if (!c) {
    llama_free_model(m);
    model_ = nullptr;
    return false;
  }
  ctx_ = c;

  // get vocab for tokenizer + eog check
  vocab_ = const_cast<llama_vocab*>(llama_model_get_vocab(m));

  // init greedy sampler
  auto sparams = llama_sampler_chain_default_params();
  llama_sampler* chain = llama_sampler_chain_init(sparams);
  llama_sampler_chain_add(chain, llama_sampler_init_greedy());
  sampler_ = chain;

  memory_bytes_ = llama_model_size(m) + llama_state_get_size(c);
  loaded_ = true;
  return true;
#endif
}

size_t GgufModel::memory_bytes() const {
  return memory_bytes_;
}

std::string GgufModel::infer(const std::string& input,
                              int max_tokens,
                              const std::string& /*capability*/) {
#ifndef THIN_AGENT_WITH_LLAMA_CPP
  return "";
#else
  if (!loaded_ && !load()) return "";
  if (input.empty()) return "";

  auto* c = static_cast<llama_context*>(ctx_);
  auto* m = static_cast<llama_model*>(model_);
  auto* v = static_cast<const llama_vocab*>(vocab_);
  auto* smpl = static_cast<llama_sampler*>(sampler_);

  // tokenize input
  int n_tokens = static_cast<int>(input.size()) + 4;
  std::vector<llama_token> tokens(n_tokens);
  n_tokens = llama_tokenize(v, input.c_str(),
                            static_cast<int>(input.size()),
                            tokens.data(), n_tokens, true, false);
  if (n_tokens < 0) return "";
  tokens.resize(n_tokens);

  // decode input tokens in batches
  int n_batch = std::min(n_tokens, static_cast<int>(llama_n_batch(c)));
  for (int i = 0; i < n_tokens; i += n_batch) {
    int batch_size = std::min(n_batch, n_tokens - i);
    auto batch = llama_batch_get_one(tokens.data() + i, batch_size);
    if (llama_decode(c, batch)) return "";
  }

  // generate
  std::string output;
  output.reserve(max_tokens * 4);
  int generated = 0;

  while (generated < max_tokens) {
    llama_token new_token = llama_sampler_sample(smpl, c, -1);
    if (llama_vocab_is_eog(v, new_token)) break;

    char buf[256];
    int n = llama_token_to_piece(v, new_token, buf, sizeof(buf), 0, true);
    if (n > 0) output.append(buf, n);

    auto batch = llama_batch_get_one(&new_token, 1);
    if (llama_decode(c, batch)) break;
    llama_sampler_accept(smpl, new_token);
    ++generated;
  }

  return output;
#endif
}

void GgufModel::infer_stream(const std::string& input,
                              TokenCallback on_token,
                              int max_tokens,
                              const std::string& /*capability*/) {
#ifndef THIN_AGENT_WITH_LLAMA_CPP
  on_token("", true);
  return;
#else
  if (!on_token) { on_token("", true); return; }
  if (!loaded_ && !load()) { on_token("", true); return; }
  if (input.empty()) { on_token("", true); return; }

  auto* c = static_cast<llama_context*>(ctx_);
  auto* m = static_cast<llama_model*>(model_);
  auto* v = static_cast<const llama_vocab*>(vocab_);
  auto* smpl = static_cast<llama_sampler*>(sampler_);

  // tokenize input
  int n_tokens = static_cast<int>(input.size()) + 4;
  std::vector<llama_token> tokens(n_tokens);
  n_tokens = llama_tokenize(v, input.c_str(),
                            static_cast<int>(input.size()),
                            tokens.data(), n_tokens, true, false);
  if (n_tokens < 0) { on_token("", true); return; }
  tokens.resize(n_tokens);

  // decode input tokens in batches
  int n_batch = std::min(n_tokens, static_cast<int>(llama_n_batch(c)));
  for (int i = 0; i < n_tokens; i += n_batch) {
    int batch_size = std::min(n_batch, n_tokens - i);
    auto batch = llama_batch_get_one(tokens.data() + i, batch_size);
    if (llama_decode(c, batch)) { on_token("", true); return; }
  }

  // generate + stream
  int generated = 0;
  while (generated < max_tokens) {
    llama_token new_token = llama_sampler_sample(smpl, c, -1);
    if (llama_vocab_is_eog(v, new_token)) break;

    char buf[256];
    int n = llama_token_to_piece(v, new_token, buf, sizeof(buf), 0, true);
    if (n > 0) {
      on_token(std::string(buf, n), false);
    }

    auto batch = llama_batch_get_one(&new_token, 1);
    if (llama_decode(c, batch)) break;
    llama_sampler_accept(smpl, new_token);
    ++generated;
  }

  on_token("", true);
#endif
}

}  // namespace local
}  // namespace thin_agent
