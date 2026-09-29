#include "thin_agent/local/HybridRouter.h"

#include <algorithm>
#include <chrono>

#include "thin_agent/local/ModelPool.h"

namespace thin_agent {
namespace local {

// ── 复杂度分类启发式 ──────────────────────────────────────────

TaskComplexity HybridRouter::classify_complexity(const std::string& input) {
  if (input.empty()) return TaskComplexity::simple;

  // 长度判断
  if (input.size() < 30) return TaskComplexity::simple;
  if (input.size() > 300) return TaskComplexity::complex;

  // 代码/技术关键词
  static constexpr const char* complex_hints[] = {
    "code", "function", "class", "debug", "fix", "implement",
    "algorithm", "optimize", "refactor", "compile", "build",
    "error", "exception", "crash", "memory", "performance",
    "async", "thread", "concurrent", "lock", "deadlock",
    "recursive", "algorithm", "data structure", "binary",
    "explain", "analyze", "compare", "design", "architecture"
  };
  for (const auto* hint : complex_hints) {
    if (input.find(hint) != std::string::npos) {
      return TaskComplexity::complex;
    }
  }

  // 中等长度 + 问句 → moderate
  if (input.find('?') != std::string::npos ||
      input.find("\xEF\xBC\x9F" /* ？ */) != std::string::npos) {
    return TaskComplexity::moderate;
  }

  // 多行/多句 → moderate+
  if (std::count(input.begin(), input.end(), '\n') >= 2) {
    return TaskComplexity::complex;
  }

  return TaskComplexity::moderate;
}

// ── 构造 & 基础路由 ──────────────────────────────────────────

HybridRouter::HybridRouter() {}

HybridRouter::HybridRouter(Config cfg) : cfg_(std::move(cfg)) {}

CascadeResult HybridRouter::route(const std::string& capability,
                                   const std::string& input,
                                   int max_tokens) {
  if (cfg_.enable_adaptive) {
    return adaptive_route(capability, input, max_tokens);
  }

  // 旧行为：级联或 best
  if (cfg_.enable_cascade) {
    auto result = ModelPool::instance().cascade(capability, input, max_tokens);
    if (result.ok) return result;
  } else {
    auto* best = ModelPool::instance().best(capability);
    if (best) {
      CascadeResult result;
      auto start = std::chrono::steady_clock::now();
      result.output = best->infer(input, max_tokens, capability);
      if (!result.output.empty()) {
        result.ok = true;
        result.model_used = best->name();
        result.tier = 0;
        auto end = std::chrono::steady_clock::now();
        result.latency_us =
            std::chrono::duration_cast<std::chrono::microseconds>(end - start)
                .count();
        return result;
      }
    }
  }

  CascadeResult fallback;
  fallback.ok = true;
  fallback.output = "我当前处于离线模式，暂时无法回答这个问题。请尝试：状态查询、天气、记忆、帮助。";
  fallback.model_used = "fallback";
  return fallback;
}

// ── 自适应路由 ────────────────────────────────────────────────

CascadeResult HybridRouter::adaptive_route(const std::string& capability,
                                            const std::string& input,
                                            int max_tokens,
                                            TokenCallback on_token) {
  auto complexity = classify_complexity(input);

  switch (complexity) {
    case TaskComplexity::simple: {
      // 简单任务：只用 TemplateModel（0ms 延迟）
      auto start = std::chrono::steady_clock::now();
      int tokens = max_tokens > 0 ? std::min(max_tokens, cfg_.simple_max_tokens)
                                  : cfg_.simple_max_tokens;

      if (on_token) {
        auto result = ModelPool::instance().cascade_stream(
            capability, input, on_token, tokens);
        auto end = std::chrono::steady_clock::now();
        result.latency_us =
            std::chrono::duration_cast<std::chrono::microseconds>(end - start)
                .count();
        result.model_used = std::string("simple:") + result.model_used;
        return result;
      }

      auto result = ModelPool::instance().cascade(capability, input, tokens);
      auto end = std::chrono::steady_clock::now();
      result.latency_us =
          std::chrono::duration_cast<std::chrono::microseconds>(end - start)
              .count();
      result.model_used = std::string("simple:") + result.model_used;
      return result;
    }

    case TaskComplexity::moderate: {
      // 中等任务：用最佳匹配模型
      auto start = std::chrono::steady_clock::now();
      int tokens = max_tokens > 0 ? std::min(max_tokens, cfg_.moderate_max_tokens)
                                  : cfg_.moderate_max_tokens;

      if (on_token) {
        auto result = ModelPool::instance().cascade_stream(
            capability, input, on_token, tokens);
        auto end = std::chrono::steady_clock::now();
        result.latency_us =
            std::chrono::duration_cast<std::chrono::microseconds>(end - start)
                .count();
        result.model_used = std::string("moderate:") + result.model_used;
        return result;
      }

      auto result = ModelPool::instance().cascade(capability, input, tokens);
      if (result.ok) {
        auto end = std::chrono::steady_clock::now();
        result.latency_us =
            std::chrono::duration_cast<std::chrono::microseconds>(end - start)
                .count();
        result.model_used = std::string("moderate:") + result.model_used;
        return result;
      }
      break;
    }

    case TaskComplexity::complex: {
      // 复杂任务：完整级联（所有模型 + 更长 tokens）
      auto start = std::chrono::steady_clock::now();
      int tokens = max_tokens > 0 ? std::min(max_tokens, cfg_.complex_max_tokens)
                                  : cfg_.complex_max_tokens;

      if (on_token) {
        auto result = ModelPool::instance().cascade_stream(
            capability, input, on_token, tokens);
        auto end = std::chrono::steady_clock::now();
        result.latency_us =
            std::chrono::duration_cast<std::chrono::microseconds>(end - start)
                .count();
        result.model_used = std::string("complex:") + result.model_used;
        return result;
      }

      auto result = ModelPool::instance().cascade(capability, input, tokens);
      auto end = std::chrono::steady_clock::now();
      result.latency_us =
          std::chrono::duration_cast<std::chrono::microseconds>(end - start)
              .count();
      if (result.ok) {
        result.model_used = std::string("complex:") + result.model_used;
      }
      return result;
    }
  }

  // 兜底
  CascadeResult fallback;
  fallback.ok = true;
  fallback.output = "我当前处于离线模式，暂时无法回答这个问题。";
  fallback.model_used = "fallback";
  return fallback;
}

// ── 便捷方法 ──────────────────────────────────────────────────

std::string HybridRouter::chat(const std::string& input, int max_tokens) {
  auto result = route("chat", input, max_tokens);
  return result.output;
}

// ── 流式路由 ──────────────────────────────────────────────────

CascadeResult HybridRouter::route_stream(const std::string& capability,
                                          const std::string& input,
                                          TokenCallback on_token,
                                          int max_tokens) {
  if (cfg_.enable_adaptive) {
    return adaptive_route(capability, input, max_tokens, on_token);
  }

  if (cfg_.enable_cascade) {
    return ModelPool::instance().cascade_stream(capability, input, on_token, max_tokens);
  }

  auto* best = ModelPool::instance().best(capability);
  if (best && on_token) {
    CascadeResult result;
    auto start = std::chrono::steady_clock::now();
    best->infer_stream(input, on_token, max_tokens, capability);
    result.ok = true;
    result.model_used = best->name();
    result.tier = 0;
    auto end = std::chrono::steady_clock::now();
    result.latency_us =
        std::chrono::duration_cast<std::chrono::microseconds>(end - start)
            .count();
    return result;
  }

  CascadeResult fallback;
  fallback.ok = true;
  fallback.output = "我当前处于离线模式，暂时无法回答这个问题。";
  fallback.model_used = "fallback";
  if (on_token) on_token(fallback.output, true);
  return fallback;
}

}  // namespace local
}  // namespace thin_agent
