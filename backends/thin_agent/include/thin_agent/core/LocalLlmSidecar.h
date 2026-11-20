#pragma once

#include <atomic>
#include <cstdint>
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

  /// 若端点已健康且身份匹配则复用；否则按 cfg.sidecar_* 拉起并轮询就绪。
  /// @return true 表示目标端点可达；失败时 *err 写原因。
  bool ensure_running(const DemoConfigCompat& cfg, std::string* err = nullptr);

  /// 取消进行中的 ensure，并终止我们自己拉起的进程（外部端点不杀）。
  void stop();

  bool is_running() const;
  bool owned() const { return owned_.load(); }
  std::string api_base() const;

  /// 探测 api_base（默认 http://127.0.0.1:1234/v1）的 /models；
  /// expect_model 非空时要求响应体包含该 id（别名校验）。
  static bool probe(const std::string& api_base, int timeout_ms = 2000,
                    const std::string& expect_model = {});

  /// 从本地 OpenAI 兼容端点读取上下文窗口（/v1/models meta.n_ctx，回退 /props）。
  /// @return >0 为有效 n_ctx；失败返回 0。
  static int fetch_n_ctx(const std::string& api_base, int timeout_ms = 2500);

  /// 由 DemoConfigCompat 拼出实际 api_base（api_base 优先，否则 sidecar_host/port）。
  static std::string make_api_base(const DemoConfigCompat& cfg);

  /// 解析监听 host/port：优先 sidecar_*；若空则从 api_base URL 拆。
  static void resolve_listen(const DemoConfigCompat& cfg, std::string* host, int* port);

  /// 身份指纹：api_base + model_path + alias，变化则不可盲目 reuse。
  static std::string identity_of(const DemoConfigCompat& cfg);

 private:
  bool spawn(const DemoConfigCompat& cfg, std::string* err);
  bool wait_healthy(const std::string& api_base, int timeout_ms,
                    const std::string& expect_model, uint64_t gen, std::string* err);
  void kill_owned();  ///< 只杀进程，不 bump cancel_gen（供 ensure 内重建）

  mutable std::mutex mu_;
  std::mutex ensure_mu_;
  std::atomic<uint64_t> cancel_gen_{0};
  std::atomic<bool> owned_{false};
  std::string api_base_;
  std::string identity_;
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
