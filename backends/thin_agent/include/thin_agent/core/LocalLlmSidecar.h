#pragma once

#include <atomic>
#include <mutex>
#include <string>

#include "thin_agent/llm/DemoConfigCompat.h"

namespace thin_agent {

/// 托管本地 OpenAI 兼容推理端点（llama-server）的启停与健康检查。
/// 供 provider=lmstudio 的 profile（如 local_tools_mode）使用：进程内 GGUF
/// 走文本 tool_call，sidecar 走 native tools/tool_calls。
class LocalLlmSidecar {
 public:
  LocalLlmSidecar() = default;
  ~LocalLlmSidecar();

  LocalLlmSidecar(const LocalLlmSidecar&) = delete;
  LocalLlmSidecar& operator=(const LocalLlmSidecar&) = delete;

  /// 若端点已健康则复用；否则按 cfg.sidecar_* 拉起 llama-server 并轮询就绪。
  /// @return true 表示 /v1/models 可达；失败时 *err 写原因。
  bool ensure_running(const DemoConfigCompat& cfg, std::string* err = nullptr);

  /// 仅当我们自己拉起的进程才终止；外部已有的端点不杀。
  /// 会打断进行中的 wait_healthy，便于析构/切换时尽快退出。
  void stop();

  bool is_running() const;
  bool owned() const { return owned_.load(); }
  std::string api_base() const;

  /// 探测 api_base（默认 http://127.0.0.1:1234/v1）的 /models。
  static bool probe(const std::string& api_base, int timeout_ms = 2000);

  /// 由 DemoConfigCompat.sidecar_* 拼出实际 api_base。
  static std::string make_api_base(const DemoConfigCompat& cfg);

 private:
  bool spawn(const DemoConfigCompat& cfg, std::string* err);
  bool wait_healthy(const std::string& api_base, int timeout_ms, std::string* err);

  mutable std::mutex mu_;
  std::mutex ensure_mu_;  ///< 串行化 ensure_running（与 stop 可交错：stop 靠 stopping_ 打断等待）
  std::atomic<bool> owned_{false};
  std::atomic<bool> stopping_{false};
  std::string api_base_;
  int port_{0};

#ifdef _WIN32
  void* process_handle_{nullptr};  // HANDLE
  void* job_handle_{nullptr};      // HANDLE
  unsigned long pid_{0};
#else
  int pid_{-1};
#endif
};

}  // namespace thin_agent
