#include "thin_agent/local/GgufModel.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

#include "thin_agent/core/Utf8Util.h"

#ifdef THIN_AGENT_WITH_LLAMA_CPP
#include "ggml-backend.h"
#include "llama.h"
#endif

namespace thin_agent {
namespace local {

namespace {

#ifdef THIN_AGENT_WITH_LLAMA_CPP

// 与 llama-server /apply-template 一致：无显式 system 时模板会注入 Qwen 默认人设。
// 这里显式带上同一条 system，避免不同 apply_template 实现漏注人设导致「能不能编程」这类答法漂移。
constexpr const char* kDefaultQwenSystem =
    "You are Qwen, created by Alibaba Cloud. You are a helpful assistant.";

using ChatTurn = GgufModel::ChatTurn;

std::string format_chat_prompt(llama_model* model,
                               const std::vector<ChatTurn>& turns_in) {
  if (!model || turns_in.empty()) return {};

  std::vector<ChatTurn> turns = turns_in;
  bool has_system = false;
  for (const auto& t : turns) {
    if (t.role == "system") { has_system = true; break; }
  }
  if (!has_system) {
    turns.insert(turns.begin(), ChatTurn{"system", kDefaultQwenSystem, {}});
  }

  std::vector<llama_chat_message> msgs;
  msgs.reserve(turns.size());
  for (const auto& t : turns) {
    msgs.push_back(llama_chat_message{t.role.c_str(), t.content.c_str()});
  }

  const char* tmpl = llama_model_chat_template(model, /*name=*/nullptr);
  if (!(tmpl && *tmpl)) return {};

  const int n_msg = static_cast<int>(msgs.size());
  int32_t need = llama_chat_apply_template(tmpl, msgs.data(), n_msg,
                                           /*add_ass=*/true,
                                           /*buf=*/nullptr, /*length=*/0);
  if (need <= 0) return {};
  std::string out(static_cast<size_t>(need) + 4, '\0');
  int32_t n = llama_chat_apply_template(tmpl, msgs.data(), n_msg, true,
                                        out.data(),
                                        static_cast<int32_t>(out.size()));
  if (n < 0) {
    out.resize(static_cast<size_t>(-n) + 4);
    n = llama_chat_apply_template(tmpl, msgs.data(), n_msg, true, out.data(),
                                  static_cast<int32_t>(out.size()));
  }
  if (n <= 0) return {};
  out.resize(static_cast<size_t>(n));
  return out;
}

// 简易 ChatML 拼装（当 apply_template 完全不可用）
std::string format_chatml_manual(const std::vector<ChatTurn>& turns_in) {
  std::vector<ChatTurn> turns = turns_in;
  bool has_system = false;
  for (const auto& t : turns) {
    if (t.role == "system") { has_system = true; break; }
  }
  if (!has_system) {
    turns.insert(turns.begin(), ChatTurn{"system", kDefaultQwenSystem, {}});
  }
  std::string out;
  out.reserve(512);
  for (const auto& t : turns) {
    out += "<|im_start|>";
    out += t.role;
    out += "\n";
    out += t.content;
    out += "<|im_end|>\n";
  }
  out += "<|im_start|>assistant\n";
  return out;
}

void clear_context(llama_context* ctx) {
  if (!ctx) return;
  llama_memory_t mem = llama_get_memory(ctx);
  if (mem) llama_memory_clear(mem, /*data=*/true);
}

/// 从 pending 前缀取出完整 UTF-8 字符；半截多字节留在 pending。
std::string take_complete_utf8(std::string& pending) {
  size_t i = 0;
  while (i < pending.size()) {
    const unsigned char c = static_cast<unsigned char>(pending[i]);
    size_t need = 1;
    if (c < 0x80) need = 1;
    else if (c >= 0xC2 && c <= 0xDF) need = 2;
    else if (c >= 0xE0 && c <= 0xEF) need = 3;
    else if (c >= 0xF0 && c <= 0xF4) need = 4;
    else {
      // 非法 lead：跳过
      pending.erase(0, 1);
      continue;
    }
    if (i + need > pending.size()) break;  // 半截，留给下次
    // 校验 continuation
    bool ok = true;
    for (size_t k = 1; k < need; ++k) {
      if ((static_cast<unsigned char>(pending[i + k]) & 0xC0) != 0x80) {
        ok = false;
        break;
      }
    }
    if (!ok) {
      pending.erase(i, 1);
      continue;
    }
    i += need;
  }
  std::string out = pending.substr(0, i);
  pending.erase(0, i);
  return out;
}

/// 收集带图 turn，并在对应 content 中插入 media marker（若尚未包含）。
std::vector<ChatTurn> turns_with_media_markers(
    const std::vector<ChatTurn>& turns_in, const char* marker,
    std::vector<std::vector<uint8_t>>* images_out) {
  std::vector<ChatTurn> turns = turns_in;
  if (images_out) images_out->clear();
  if (!marker || !*marker) return turns;
  for (auto& t : turns) {
    if (t.image_bytes.empty()) continue;
    if (images_out) images_out->push_back(t.image_bytes);
    if (t.content.find(marker) == std::string::npos) {
      if (!t.content.empty() && t.content.back() != '\n') t.content.push_back('\n');
      t.content += marker;
    }
  }
  return turns;
}

bool prepare_prompt_tokens(llama_model* m, llama_context* c, const llama_vocab* v,
                           llama_sampler* smpl,
                           const std::vector<ChatTurn>& turns,
                           std::vector<llama_token>* out_tokens,
                           MtmdSession* mtmd) {
  if (!m || !c || !v || !out_tokens || turns.empty()) return false;
  clear_context(c);
  if (smpl) llama_sampler_reset(smpl);

  std::vector<std::vector<uint8_t>> images;
  const char* marker =
      (mtmd && mtmd->ready()) ? mtmd->media_marker() : nullptr;
  std::vector<ChatTurn> turns_use =
      turns_with_media_markers(turns, marker, &images);

  // 无 vision 时丢掉 image_bytes，避免 marker 泄漏进纯文本路径
  if (!(mtmd && mtmd->ready()) || images.empty()) {
    for (auto& t : turns_use) {
      if (!t.image_bytes.empty()) {
        t.image_bytes.clear();
        // 无 encoder：用文字占位保留「有图」意图
        t.content +=
            "\n[Attached image present; local vision encoder not loaded.]";
      }
    }
    images.clear();
  }

  std::string prompt = format_chat_prompt(m, turns_use);
  if (prompt.empty()) prompt = format_chatml_manual(turns_use);

  if (mtmd && mtmd->ready() && !images.empty()) {
    if (mtmd->eval_prompt(c, prompt, images)) {
      // eval 已写入 KV；token 数未知时用粗估供 room 计算
      out_tokens->assign(1, 0);
      return true;
    }
    // mmproj/图像解码失败：退回纯文本，避免整轮空回复
    // 必须去掉已插入的 media marker，否则纯文本路径会留下无意义占位符
    const char* drop = marker;
    for (auto& t : turns_use) {
      if (t.image_bytes.empty() &&
          !(drop && *drop && t.content.find(drop) != std::string::npos))
        continue;
      t.image_bytes.clear();
      if (drop && *drop) {
        for (;;) {
          auto p = t.content.find(drop);
          if (p == std::string::npos) break;
          t.content.erase(p, std::strlen(drop));
        }
      }
      t.content +=
          "\n[Attached image present; local vision encode failed, "
          "text-only fallback.]";
    }
    prompt = format_chat_prompt(m, turns_use);
    if (prompt.empty()) prompt = format_chatml_manual(turns_use);
    clear_context(c);
    if (smpl) llama_sampler_reset(smpl);
  }

  int n_tokens = -llama_tokenize(v, prompt.c_str(), static_cast<int>(prompt.size()),
                                 nullptr, 0, true, true);
  if (n_tokens <= 0) n_tokens = static_cast<int>(prompt.size()) + 8;
  out_tokens->assign(static_cast<size_t>(n_tokens) + 8, 0);
  n_tokens = llama_tokenize(v, prompt.c_str(), static_cast<int>(prompt.size()),
                            out_tokens->data(), static_cast<int>(out_tokens->size()),
                            true, true);
  if (n_tokens < 0) {
    out_tokens->resize(static_cast<size_t>(-n_tokens));
    n_tokens = llama_tokenize(v, prompt.c_str(), static_cast<int>(prompt.size()),
                              out_tokens->data(), static_cast<int>(out_tokens->size()),
                              true, true);
  }
  if (n_tokens <= 0) return false;
  out_tokens->resize(static_cast<size_t>(n_tokens));

  const int n_batch = std::max(1, static_cast<int>(llama_n_batch(c)));
  for (int i = 0; i < n_tokens; i += n_batch) {
    int batch_size = std::min(n_batch, n_tokens - i);
    auto batch = llama_batch_get_one(out_tokens->data() + i, batch_size);
    if (llama_decode(c, batch)) return false;
  }
  return true;
}

#endif  // THIN_AGENT_WITH_LLAMA_CPP

}  // namespace

GgufModel::GgufModel(const std::string& model_path,
                     std::vector<ModelCapability> caps,
                     int n_ctx, int n_threads)
    : model_path_(model_path),
      caps_(std::move(caps)),
      n_ctx_requested_(n_ctx),
      n_ctx_(n_ctx > 0 ? n_ctx : 0),
      n_threads_(n_threads) {
  auto pos = model_path_.find_last_of("/\\");
  name_ = pos != std::string::npos ? model_path_.substr(pos + 1) : model_path_;
  if (auto dot = name_.rfind('.'); dot != std::string::npos) {
    name_ = name_.substr(0, dot);
  }
}

GgufModel::~GgufModel() {
  mtmd_.reset();
#ifdef THIN_AGENT_WITH_LLAMA_CPP
  if (sampler_) llama_sampler_free(static_cast<llama_sampler*>(sampler_));
  if (ctx_) llama_free(static_cast<llama_context*>(ctx_));
  if (model_) llama_model_free(static_cast<llama_model*>(model_));
#endif
}

void GgufModel::unload() {
  mtmd_.reset();
#ifdef THIN_AGENT_WITH_LLAMA_CPP
  if (sampler_) { llama_sampler_free(static_cast<llama_sampler*>(sampler_)); sampler_ = nullptr; }
  if (ctx_) { llama_free(static_cast<llama_context*>(ctx_)); ctx_ = nullptr; }
  if (model_) { llama_model_free(static_cast<llama_model*>(model_)); model_ = nullptr; }
#endif
  vocab_ = nullptr;
  memory_bytes_ = 0;
  n_ctx_train_ = 0;
  n_ctx_ = n_ctx_requested_ > 0 ? n_ctx_requested_ : 0;
  loaded_ = false;
}

bool GgufModel::load() {
#ifndef THIN_AGENT_WITH_LLAMA_CPP
  return false;
#else
  if (loaded_) return true;
  if (!std::filesystem::exists(model_path_)) return false;

  static std::once_flag backends_once;
  std::call_once(backends_once, []() { ggml_backend_load_all(); });

  llama_model_params model_params = llama_model_default_params();
  model_params.n_gpu_layers = -1;

  llama_model* m = llama_model_load_from_file(model_path_.c_str(), model_params);
  if (!m) return false;
  model_ = m;

  // 按模型能力定窗：n_ctx_requested_<=0 → 用训练窗口；显式 >0 才覆盖
  n_ctx_train_ = llama_model_n_ctx_train(m);
  int resolved = n_ctx_requested_;
  if (resolved <= 0) {
    const char* env = std::getenv("THIN_AGENT_GGUF_N_CTX");
    if (env && *env) {
      try { resolved = std::stoi(env); } catch (...) { resolved = 0; }
    }
  }
  if (resolved <= 0) {
    resolved = n_ctx_train_ > 0 ? n_ctx_train_ : 0;  // 0 = llama「跟模型」
  }
  // 不超过模型原生能力（有 train 值时）
  if (n_ctx_train_ > 0 && resolved > n_ctx_train_) resolved = n_ctx_train_;

  llama_context_params ctx_params = llama_context_default_params();
  ctx_params.n_ctx = static_cast<uint32_t>(resolved);  // 0 = from model
  ctx_params.n_threads = n_threads_;
  ctx_params.n_threads_batch = n_threads_;

  llama_context* c = llama_init_from_model(m, ctx_params);
  if (!c) {
    llama_model_free(m);
    model_ = nullptr;
    n_ctx_train_ = 0;
    return false;
  }
  ctx_ = c;
  vocab_ = const_cast<llama_vocab*>(llama_model_get_vocab(m));
  n_ctx_ = static_cast<int>(llama_n_ctx(c));
  if (n_ctx_ <= 0 && resolved > 0) n_ctx_ = resolved;

  // 能力表跟实际窗口对齐（chat 等）
  for (auto& cap : caps_) {
    if (cap.max_input_tokens <= 0 ||
        (n_ctx_ > 0 && cap.max_input_tokens > n_ctx_)) {
      cap.max_input_tokens = n_ctx_ > 0 ? n_ctx_ : cap.max_input_tokens;
    }
  }

  // 对齐当前 llama-server /props 默认采样（top_k=20 top_p=0.8 min_p=0.05 temp=0.7）
  // repeat_penalty=1.0 等价于关闭，故不挂 penalties
  auto sparams = llama_sampler_chain_default_params();
  llama_sampler* chain = llama_sampler_chain_init(sparams);
  llama_sampler_chain_add(chain, llama_sampler_init_top_k(20));
  llama_sampler_chain_add(chain, llama_sampler_init_top_p(0.80f, 1));
  llama_sampler_chain_add(chain, llama_sampler_init_min_p(0.05f, 1));
  llama_sampler_chain_add(chain, llama_sampler_init_temp(0.7f));
  llama_sampler_chain_add(chain, llama_sampler_init_dist(LLAMA_DEFAULT_SEED));
  sampler_ = chain;

  memory_bytes_ = static_cast<size_t>(llama_model_size(m)) + llama_state_get_size(c);
  loaded_ = true;

  // 可选多模态：mtmd.dll + mmproj（失败不影响纯文本）
  std::string mmproj = mmproj_path_override_;
  if (mmproj.empty()) mmproj = resolve_mmproj_path(model_path_);
  if (!mmproj.empty() && mtmd_runtime_available()) {
    mtmd_.init(m, mmproj, n_threads_);
  }
  return true;
#endif
}

size_t GgufModel::memory_bytes() const {
  return memory_bytes_;
}

nlohmann::json GgufModel::status() const {
  auto j = ILocalModel::status();
  j["n_ctx"] = n_ctx_;
  j["n_ctx_train"] = n_ctx_train_;
  j["n_ctx_requested"] = n_ctx_requested_;
  j["n_ctx_auto"] = n_ctx_requested_ <= 0;
  j["mtmd_runtime"] = mtmd_runtime_available();
  j["vision"] = mtmd_.ready();
  if (!mtmd_.mmproj_path().empty()) {
    j["mmproj"] = mtmd_.mmproj_path();
  } else {
    const std::string cand = resolve_mmproj_path(model_path_);
    if (!cand.empty()) {
      j["mmproj_candidate"] = cand;
      j["vision_capable"] = mtmd_runtime_available();
    } else {
      j["vision_capable"] = false;
    }
  }
  if (mtmd_.ready()) j["vision_capable"] = true;
  return j;
}

std::string GgufModel::infer_turns(const std::vector<ChatTurn>& turns,
                                   int max_tokens) {
#ifndef THIN_AGENT_WITH_LLAMA_CPP
  (void)turns; (void)max_tokens;
  return "";
#else
  if (!loaded_ && !load()) return "";
  if (turns.empty()) return "";

  auto* c = static_cast<llama_context*>(ctx_);
  auto* m = static_cast<llama_model*>(model_);
  auto* v = static_cast<const llama_vocab*>(vocab_);
  auto* smpl = static_cast<llama_sampler*>(sampler_);

  std::vector<llama_token> prompt_tokens;
  if (!prepare_prompt_tokens(m, c, v, smpl, turns, &prompt_tokens, &mtmd_))
    return "";

  // max_tokens 只是循环上限，不另开内存；真正显存/内存在 load 时按 n_ctx 定
  // 生成不能超过「窗口 − 已用 prompt」，否则 decode 会失败/截断
  // vision eval 后 token 计数不可靠：用窗口一半作保守 room
  const int prompt_n = static_cast<int>(prompt_tokens.size());
  const bool vision_path = mtmd_.ready() && prompt_n <= 1;
  const int room = n_ctx_ > 0
                       ? (vision_path ? std::max(64, n_ctx_ / 2)
                                      : std::max(64, n_ctx_ - prompt_n - 4))
                       : std::max(max_tokens, 64);
  const int limit = std::min(std::max(max_tokens, 64), room);

  std::string output;
  output.reserve(static_cast<size_t>(limit) * 4);
  int generated = 0;

  while (generated < limit) {
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

  return utf8_sanitize(output);
#endif
}

void GgufModel::infer_stream_turns(const std::vector<ChatTurn>& turns,
                                   TokenCallback on_token,
                                   int max_tokens) {
#ifndef THIN_AGENT_WITH_LLAMA_CPP
  (void)turns; (void)max_tokens;
  if (on_token) on_token("", true);
  return;
#else
  if (!on_token) return;
  if (!loaded_ && !load()) { on_token("", true); return; }
  if (turns.empty()) { on_token("", true); return; }

  auto* c = static_cast<llama_context*>(ctx_);
  auto* m = static_cast<llama_model*>(model_);
  auto* v = static_cast<const llama_vocab*>(vocab_);
  auto* smpl = static_cast<llama_sampler*>(sampler_);

  std::vector<llama_token> prompt_tokens;
  if (!prepare_prompt_tokens(m, c, v, smpl, turns, &prompt_tokens, &mtmd_)) {
    on_token("", true);
    return;
  }

  std::string pending;
  int generated = 0;
  const int prompt_n = static_cast<int>(prompt_tokens.size());
  const bool vision_path = mtmd_.ready() && prompt_n <= 1;
  const int room = n_ctx_ > 0
                       ? (vision_path ? std::max(64, n_ctx_ / 2)
                                      : std::max(64, n_ctx_ - prompt_n - 4))
                       : std::max(max_tokens, 64);
  const int limit = std::min(std::max(max_tokens, 64), room);

  while (generated < limit) {
    llama_token new_token = llama_sampler_sample(smpl, c, -1);
    if (llama_vocab_is_eog(v, new_token)) break;

    char buf[256];
    int n = llama_token_to_piece(v, new_token, buf, sizeof(buf), 0, true);
    if (n > 0) {
      pending.append(buf, n);
      std::string piece = take_complete_utf8(pending);
      if (!piece.empty()) on_token(piece, false);
    }

    auto batch = llama_batch_get_one(&new_token, 1);
    if (llama_decode(c, batch)) break;
    llama_sampler_accept(smpl, new_token);
    ++generated;
  }

  if (!pending.empty()) {
    std::string rest = utf8_sanitize(pending);
    if (!rest.empty()) on_token(rest, false);
  }
  on_token("", true);
#endif
}

std::string GgufModel::infer(const std::string& input,
                              int max_tokens,
                              const std::string& /*capability*/) {
  if (input.empty()) return "";
  return infer_turns({ChatTurn{"user", input, {}}}, max_tokens);
}

void GgufModel::infer_stream(const std::string& input,
                              TokenCallback on_token,
                              int max_tokens,
                              const std::string& /*capability*/) {
  if (!on_token) return;
  if (input.empty()) { on_token("", true); return; }
  infer_stream_turns({ChatTurn{"user", input, {}}}, std::move(on_token),
                     max_tokens);
}

}  // namespace local
}  // namespace thin_agent
