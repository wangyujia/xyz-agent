#include "thin_agent/local/ModelPool.h"

#include <algorithm>
#include <chrono>
#include <future>
#include <stdexcept>

#include "thin_agent/local/GgufModel.h"

namespace thin_agent {
namespace local {

ModelPool& ModelPool::instance() {
  static ModelPool pool;
  return pool;
}

void ModelPool::add(std::unique_ptr<ILocalModel> model) {
  std::lock_guard<std::mutex> lk(mu_);
  models_.push_back(std::move(model));
}

void ModelPool::remove(const std::string& name) {
  std::lock_guard<std::mutex> lk(mu_);
  models_.erase(
      std::remove_if(models_.begin(), models_.end(),
                     [&](const auto& m) { return m->name() == name; }),
      models_.end());
}

bool ModelPool::reload(const std::string& name, const std::string& new_gguf_path) {
  std::lock_guard<std::mutex> lk(mu_);
  for (auto& m : models_) {
    if (m->name() == name) {
      // 卸载旧模型释放内存
      m->unload();

      // 创建新 GgufModel 替换；n_ctx 沿用「自动跟模型」策略（<=0）
      auto caps = m->capabilities();
      auto* gguf = dynamic_cast<GgufModel*>(m.get());
      int n_ctx = gguf ? gguf->requested_n_ctx() : 0;
      int n_threads = 4;

      auto new_model = std::make_unique<GgufModel>(new_gguf_path, caps, n_ctx, n_threads);
      if (!new_model->load()) return false;

      m = std::move(new_model);
      if (preferred_chat_ == name) preferred_chat_ = m->name();
      return true;
    }
  }
  return false;
}

void ModelPool::order_chat_candidates_locked(std::vector<ILocalModel*>& cands) const {
  if (preferred_chat_.empty()) return;
  // 已选本地模型时：优先该 GGUF（贴近 llama-server 直聊），模板仅作失败回落
  std::vector<ILocalModel*> pref, rest;
  for (auto* m : cands) {
    if (m->name() == preferred_chat_) pref.push_back(m);
    else rest.push_back(m);
  }
  if (pref.empty()) return;
  cands.clear();
  cands.insert(cands.end(), pref.begin(), pref.end());
  cands.insert(cands.end(), rest.begin(), rest.end());
}

ILocalModel* ModelPool::find_by_name(const std::string& name) {
  std::lock_guard<std::mutex> lk(mu_);
  for (auto& m : models_) {
    if (m->name() == name) return m.get();
  }
  return nullptr;
}

std::string ModelPool::preferred_chat() const {
  std::lock_guard<std::mutex> lk(mu_);
  return preferred_chat_;
}

bool ModelPool::set_preferred_chat(const std::string& name, bool unload_others,
                                   bool load_now) {
  ILocalModel* target = nullptr;
  {
    std::lock_guard<std::mutex> lk(mu_);
    for (auto& m : models_) {
      if (m->name() != name) continue;
      bool has_chat = false;
      for (const auto& c : m->capabilities()) {
        if (c.name == "chat") { has_chat = true; break; }
      }
      if (!has_chat) return false;
      if (m->backend() == "template") return false;
      target = m.get();
      break;
    }
    if (!target) return false;
    preferred_chat_ = name;
    if (unload_others) {
      for (auto& m : models_) {
        if (m.get() == target) continue;
        if (m->backend() == "gguf" && m->is_loaded()) m->unload();
      }
    }
  }
  if (!load_now) return true;
  // 锁外加载，避免 load 长时间占池锁
  if (!target->is_loaded() && !target->load()) return false;
  return true;
}

std::vector<ILocalModel*> ModelPool::match(const std::string& capability) {
  std::lock_guard<std::mutex> lk(mu_);
  std::vector<ILocalModel*> result;
  for (auto& m : models_) {
    if (!m->is_loaded()) continue;
    for (const auto& cap : m->capabilities()) {
      if (cap.name == capability) {
        result.push_back(m.get());
        break;
      }
    }
  }
  std::sort(result.begin(), result.end(),
            [&](ILocalModel* a, ILocalModel* b) {
              auto pa = 999, pb = 999;
              for (const auto& c : a->capabilities())
                if (c.name == capability) { pa = c.priority; break; }
              for (const auto& c : b->capabilities())
                if (c.name == capability) { pb = c.priority; break; }
              return pa < pb;
            });
  return result;
}

ILocalModel* ModelPool::best(const std::string& capability,
                              int64_t max_latency_us) {
  auto candidates = match(capability);
  if (candidates.empty()) return nullptr;

  // filter by max latency
  if (max_latency_us > 0) {
    std::vector<ILocalModel*> filtered;
    for (auto* m : candidates) {
      if (m->typical_latency_us() <= max_latency_us)
        filtered.push_back(m);
    }
    if (!filtered.empty()) return filtered.front();
  }
  return candidates.front();
}

CascadeResult ModelPool::cascade(const std::string& capability,
                                  const std::string& input,
                                  int max_tokens) {
  CascadeResult result;
  auto start = std::chrono::steady_clock::now();

  // 收集所有匹配能力的模型（含未加载的），按 priority 排序
  std::vector<ILocalModel*> all_candidates;
  {
    std::lock_guard<std::mutex> lk(mu_);
    for (auto& m : models_) {
      for (const auto& cap : m->capabilities()) {
        if (cap.name == capability) {
          all_candidates.push_back(m.get());
          break;
        }
      }
    }
    std::sort(all_candidates.begin(), all_candidates.end(),
              [&](ILocalModel* a, ILocalModel* b) {
                int pa = 999, pb = 999;
                for (const auto& c : a->capabilities())
                  if (c.name == capability) { pa = c.priority; break; }
                for (const auto& c : b->capabilities())
                  if (c.name == capability) { pb = c.priority; break; }
                return pa < pb;
              });
    if (capability == "chat") order_chat_candidates_locked(all_candidates);
  }

  if (all_candidates.empty()) {
    auto end = std::chrono::steady_clock::now();
    result.latency_us = std::chrono::duration_cast<std::chrono::microseconds>(
                            end - start).count();
    return result;
  }

  int tier = 0;
  for (auto* model : all_candidates) {
    result.tried.push_back(model->name());

    // 懒加载：未加载的模型尝试加载（首次加载有开销但后续命中缓存）
    if (!model->is_loaded()) {
      if (!model->load()) {
        ++tier;
        continue;  // 加载失败，尝试下一个
      }
    }

    auto output = model->infer(input, max_tokens, capability);
    if (!output.empty()) {
      result.ok = true;
      result.output = output;
      result.model_used = model->name();
      result.tier = tier;
      auto end = std::chrono::steady_clock::now();
      result.latency_us =
          std::chrono::duration_cast<std::chrono::microseconds>(end - start)
              .count();
      return result;
    }
    ++tier;
  }

  auto end = std::chrono::steady_clock::now();
  result.latency_us = std::chrono::duration_cast<std::chrono::microseconds>(
                          end - start).count();
  return result;
}

CascadeResult ModelPool::cascade_stream(const std::string& capability,
                                         const std::string& input,
                                         TokenCallback on_token,
                                         int max_tokens) {
  CascadeResult result;
  auto start = std::chrono::steady_clock::now();

  // 收集所有匹配能力的模型（含未加载的），按 priority 排序
  std::vector<ILocalModel*> all_candidates;
  {
    std::lock_guard<std::mutex> lk(mu_);
    for (auto& m : models_) {
      for (const auto& cap : m->capabilities()) {
        if (cap.name == capability) {
          all_candidates.push_back(m.get());
          break;
        }
      }
    }
    std::sort(all_candidates.begin(), all_candidates.end(),
              [&](ILocalModel* a, ILocalModel* b) {
                int pa = 999, pb = 999;
                for (const auto& c : a->capabilities())
                  if (c.name == capability) { pa = c.priority; break; }
                for (const auto& c : b->capabilities())
                  if (c.name == capability) { pb = c.priority; break; }
                return pa < pb;
              });
    if (capability == "chat") order_chat_candidates_locked(all_candidates);
  }

  if (all_candidates.empty() || !on_token) {
    auto end = std::chrono::steady_clock::now();
    result.latency_us = std::chrono::duration_cast<std::chrono::microseconds>(
                            end - start).count();
    return result;
  }

  int tier = 0;
  for (auto* model : all_candidates) {
    result.tried.push_back(model->name());

    if (!model->is_loaded()) {
      if (!model->load()) { ++tier; continue; }
    }

    // 流式回调包装：收集输出文本 + 自动补 done
    std::string collected;
    auto wrapper = [&](const std::string& token, bool done) {
      if (!token.empty()) {
        collected += token;
        on_token(token, done);
      }
      if (done && token.empty()) {
        on_token("", true);  // 确保 done 信号送达（仅当模型没发带内容的 done 时）
      }
    };

    model->infer_stream(input, wrapper, max_tokens, capability);

    if (!collected.empty()) {
      result.ok = true;
      result.output = collected;
      result.model_used = model->name();
      result.tier = tier;
      auto end = std::chrono::steady_clock::now();
      result.latency_us =
          std::chrono::duration_cast<std::chrono::microseconds>(end - start)
              .count();
      return result;
    }
    ++tier;
  }

  auto end = std::chrono::steady_clock::now();
  result.latency_us = std::chrono::duration_cast<std::chrono::microseconds>(
                          end - start).count();
  return result;
}

std::vector<std::pair<std::string, std::string>> ModelPool::parallel(
    const std::vector<std::string>& capabilities,
    const std::string& input,
    int max_tokens) {
  std::vector<std::pair<std::string, std::string>> results;
  std::vector<std::future<std::pair<std::string, std::string>>> futures;

  for (const auto& cap : capabilities) {
    auto* model = best(cap);
    if (!model) continue;
    futures.push_back(std::async(std::launch::async, [model, cap, input, max_tokens]() {
      return std::make_pair(model->name(), model->infer(input, max_tokens, cap));
    }));
  }

  for (auto& f : futures) {
    results.push_back(f.get());
  }
  return results;
}

size_t ModelPool::size() const {
  std::lock_guard<std::mutex> lk(mu_);
  return models_.size();
}

size_t ModelPool::total_memory_bytes() const {
  std::lock_guard<std::mutex> lk(mu_);
  size_t total = 0;
  for (const auto& m : models_) total += m->memory_bytes();
  return total;
}

std::vector<nlohmann::json> ModelPool::status_all() const {
  std::lock_guard<std::mutex> lk(mu_);
  std::vector<nlohmann::json> out;
  for (const auto& m : models_) out.push_back(m->status());
  return out;
}

}  // namespace local
}  // namespace thin_agent
