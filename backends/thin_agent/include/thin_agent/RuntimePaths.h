#pragma once

#include <string>
#include <cstdlib>

namespace thin_agent {

/// v0.43.0: 激活的 profile 名称。为空时使用默认 home 目录。
/// 设置方式：thin_agent_set_profile("prod")
inline std::string& active_profile() {
  static std::string p;
  return p;
}
inline void thin_agent_set_profile(const std::string& name) { active_profile() = name; }

#if defined(THIN_AGENT_EMBEDDED_CAM) && THIN_AGENT_EMBEDDED_CAM
inline std::string default_data_dir() { return "/data/thin_agent/data"; }
inline std::string default_config_dir() { return "/data/thin_agent/config"; }
inline std::string default_models_dir() { return "/data/thin_agent/models"; }
#else
/// 返回用户主目录下的 .thin_agent 根目录
/// $THIN_AGENT_HOME 优先于 $HOME（防止 Hermes/容器环境覆盖 $HOME）
/// v0.43.0: profile 非空时使用 ~/.thin_agent/profiles/{profile}/
inline std::string default_home_dir() {
  const char* env_home = std::getenv("THIN_AGENT_HOME");
  if (env_home && *env_home) return std::string(env_home);
#ifdef _WIN32
  const char* home = std::getenv("USERPROFILE");
#else
  const char* home = std::getenv("HOME");
#endif
  if (!home || !*home) home = ".";
  std::string base = std::string(home) + "/.thin_agent";
  auto& prof = active_profile();
  if (!prof.empty()) base += "/profiles/" + prof;
  return base;
}
inline std::string default_data_dir()   { return default_home_dir() + "/data"; }
inline std::string default_config_dir() { return default_home_dir() + "/config"; }
inline std::string default_models_dir() { return default_home_dir() + "/models"; }
#endif

inline std::string default_task_db_path() { return default_data_dir() + "/agent_tasks.db"; }
inline std::string default_memory_jsonl_path() { return default_data_dir() + "/agent_memory.jsonl"; }
inline std::string default_decision_audit_path() { return default_data_dir() + "/decision_audit.jsonl"; }
inline std::string default_chat_policy_path() { return default_config_dir() + "/chat_policy.json"; }
inline std::string default_demo_model_config_path() {
    // Prefer the root-level config (used by run_agent.sh), fall back to config/ subdirectory
    std::string root_cfg = default_home_dir() + "/demo.model.yaml";
    // Quick check: if root-level exists, use it; otherwise fall back to config/ subdir
    // (We can't stat here without <sys/stat.h>, so always return root-level;
    //  the config loader will report a clear error if neither path exists.)
    return root_cfg;
}
inline std::string default_intent_stub_onnx_path() { return default_models_dir() + "/intent/intent_stub.onnx"; }
inline std::string default_traces_db_path() { return default_data_dir() + "/agent_traces.db"; }
inline std::string default_logs_dir() { return default_home_dir() + "/logs"; }
inline std::string default_agent_svc_log_path() { return default_logs_dir() + "/agent_svc.log"; }
inline std::string default_agent_gw_log_path() { return default_logs_dir() + "/agent_gw.log"; }

// ── v0.27.0: 知识库路径 ──
inline std::string default_kb_dir() { return default_home_dir() + "/kb"; }
inline std::string default_kb_db_path() { return default_kb_dir() + "/codebase.db"; }
inline std::string default_kb_index_path() { return default_kb_dir() + "/kb_index.json"; }

}  // namespace thin_agent
