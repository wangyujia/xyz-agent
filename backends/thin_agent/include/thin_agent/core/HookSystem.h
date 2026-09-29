#pragma once
/// v0.52.29: Hook System —— 工具/会话生命周期钩子（差距表 #1，P1）。
///
/// 对标 Claude Code 8 事件钩子，按 thin_agent 实际执行链裁剪为 8 个事件：
///   会话生命周期：session_start / session_end
///   工具生命周期：tool_pre / tool_post / tool_error
///   FC 循环边界： fc_start / fc_end
///   消息入站：    message_in
///
/// 设计要点：
///   - HookSource 两类：C++ 回调（进程内，同步）+ shell 命令（子进程，
///     异步收集，超时保护）——shell 钩子覆盖"不改代码接现有脚本"场景
///   - tool_pre 可否决（返回 deny+reason → 工具不执行，理由喂回模型），
///     其余事件只观察（观察者不得阻塞执行链——shell 钩子限时 5s）
///   - 钩子匹配：event 全量分发 + tool 名单过滤（tool_* 事件可限定工具）
///   - 配置零侵入：不配=完全零开销（map 查找 miss 即直通）
///
/// 与既有子系统的乘法关系：
///   审批（HITL）：tool_pre deny 先于 approval_checker（静态规则挡在动态审批前）
///   沙箱：tool_post 可做命令审计落盘
///   审计：现有 log_event 不变，钩子是"用户可编程的旁路"，不替代审计

#include <nlohmann/json.hpp>
#include <functional>
#include <string>
#include <vector>

namespace thin_agent {

/// 钩子事件类型。
enum class HookEvent {
  SessionStart,  ///< 会话建立（on_session_open）
  SessionEnd,    ///< 会话关闭（on_session_close）
  FcStart,       ///< FC 循环启动（带 goal 摘要）
  FcEnd,         ///< FC 循环收敛/失败（带迭代数+结论摘要）
  ToolPre,       ///< 工具执行前（可 deny）
  ToolPost,      ///< 工具执行后（带输出摘要）
  ToolError,     ///< 工具执行异常/失败
  MessageIn,     ///< 用户消息入站（改写前）
};

/// HookEvent → 协议名。
inline const char* hook_event_name(HookEvent e) {
  switch (e) {
    case HookEvent::SessionStart: return "session_start";
    case HookEvent::SessionEnd:   return "session_end";
    case HookEvent::FcStart:      return "fc_start";
    case HookEvent::FcEnd:        return "fc_end";
    case HookEvent::ToolPre:      return "tool_pre";
    case HookEvent::ToolPost:     return "tool_post";
    case HookEvent::ToolError:    return "tool_error";
    case HookEvent::MessageIn:    return "message_in";
  }
  return "unknown";
}

/// 事件名 → HookEvent。未知返回 MessageIn（调用方须比对原始字符串甄别，
/// 见 ws_agent_main 加载逻辑）。提供对称解析便于配置层使用。
inline HookEvent hook_event_from_string(const std::string& name) {
  if (name == "session_start") return HookEvent::SessionStart;
  if (name == "session_end")   return HookEvent::SessionEnd;
  if (name == "fc_start")      return HookEvent::FcStart;
  if (name == "fc_end")        return HookEvent::FcEnd;
  if (name == "tool_pre")      return HookEvent::ToolPre;
  if (name == "tool_post")     return HookEvent::ToolPost;
  if (name == "tool_error")    return HookEvent::ToolError;
  return HookEvent::MessageIn;
}

/// tool_pre 钩子的裁决。
struct HookVerdict {
  bool deny{false};
  std::string reason;  ///< deny 时的理由（喂回模型）
};

/// 单个已注册钩子。
struct HookRegistration {
  std::string id;         ///< 注册 id（卸载/审计用）
  HookEvent event;
  std::vector<std::string> tool_filter;  ///< 空=不过滤（全部工具）
  /// C++ 回调（进程内）。返回 verdict（tool_pre 用，其余忽略）。
  std::function<HookVerdict(const nlohmann::json& payload)> callback;
  /// shell 命令（非空=shell 钩子）。payload 以 JSON stdin 传入；
  /// tool_pre 时 exit_code!=0 → deny（stderr 截 200 字符为 reason）。
  std::string shell_cmd;
  int shell_timeout_ms{5000};
};

class HookSystem {
public:
  static HookSystem& instance();

  /// 注册回调钩子。返回注册 id（空=失败）。
  std::string register_hook(HookRegistration reg);
  /// 按 id 卸载。
  bool unregister_hook(const std::string& id);
  /// 清空（测试用）。
  void clear();
  /// 枚举已注册钩子（脱敏：id/event/tool_filter/shell_cmd/timeout，
  /// 不含 callback 体——进程外不可序列化）。
  std::vector<nlohmann::json> enumerate() const;
  /// 已注册钩子数（测试/观测用）。
  size_t size() const;

  /// 事件分发。payload 结构由事件决定（tool_* 含 tool/args/session/iter）。
  /// tool_pre 事件返回合并裁决（任一 deny 即 deny）；其余事件返回默认。
  /// 无钩子注册时 O(1) 直通。
  HookVerdict dispatch(HookEvent event, const nlohmann::json& payload);

private:
  HookSystem() = default;
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace thin_agent
