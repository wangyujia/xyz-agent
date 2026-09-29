#pragma once
/// v0.53.0: PluginContext —— 插件与核心之间的服务接口（v0.53 核心瘦身地基）。
///
/// 背景：38 个场景 handler 此前焊在 AgentService（10,811 行）。按
/// "轻量核心+插件+配置"定位拆为独立 .so 插件后，插件需要少量核心
/// 服务（触发 chat/广播/会话快照/数据目录），但不能反向依赖
/// AgentService 全身。PluginContext 是注入给插件的窄接口：
///   - 插件只见抽象（头文件依赖，无链接依赖——符号运行时解析）
///   - 核心实现它，插件的 init 签名升级为 (registry, context)
///   - 旧插件（仅 registry 签名）继续兼容加载
///
/// 版本协商：context_v2 传入前插件可用 ctx.version 检查能力。

#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace thin_agent {

/// 注入给 v2 插件的核心服务面。
class PluginContext {
public:
  virtual ~PluginContext();
  PluginContext();

  /// 能力版本（协商用）。v2 = chat/broadcast/snapshot/data_dir。
  virtual int version() const { return 2; }

  /// 触发一次 chat 管线（同步，返回最终 chat_result）。
  /// cron/patrol/monitor 等"定时→LLM 自主响应"场景用。
  /// session 为空则用内部会话。禁止在 chat 回调内重入（核心侧防重入）。
  virtual nlohmann::json chat(const std::string& session,
                              const std::string& text,
                              const nlohmann::json& meta = {}) {
    (void)session; (void)text; (void)meta;
    return {{"type", "error"}, {"message", "chat not available"}};
  }

  /// 广播一帧给所有在线连接（cron 交付/监控告警等）。
  virtual void broadcast(const nlohmann::json& frame) { (void)frame; }

  /// 会话快照（checkpoint 用）：返回该会话当前消息数组。
  virtual nlohmann::json session_snapshot(const std::string& session) {
    (void)session;
    return nlohmann::json::array();
  }

  /// 数据目录（插件自有状态存放处，已含 profile 前缀）。
  /// 约定子目录：data/<plugin_name>/…
  virtual std::string data_dir(const std::string& plugin_name) const {
    (void)plugin_name;
    return ".";
  }

  /// 配置面：chat_policy.json 的 plugins.<name> 节（无则 null）。
  virtual nlohmann::json config(const std::string& plugin_name) const {
    (void)plugin_name;
    return nlohmann::json();
  }

  /// 日志（核心统一格式）。
  /// v0.53.4: 服务实例存活探测——插件后台线程（cron ticker 等）回调前
  /// 必查。实现=进程级注册表（ctx 构造登记/析构除名），不依赖虚表与
  /// 成员（ctx 对象亡后虚调用本身即 UB——注册表方案规避之）。
  virtual bool alive() const;

  /// v0.53.47: 静态存活探测——插件后台线程(ticker/monitor)持裸指针
  /// 时【禁止虚调用 alive()】(悬垂时虚表读取即 UB,实测 SegFault),
  /// 必须用本静态版:注册表键查询,悬垂指针只做比较不解引用。
  static bool is_alive(const PluginContext* ctx);
  /// 进程级 ctx 存活注册表（源在 PluginContext.cpp）
  static bool probe(const void* ctx);

  /// 日志（核心统一格式）。
  virtual void log(const std::string& plugin_name, const std::string& level,
                   const std::string& event, const nlohmann::json& fields = {}) {
    (void)plugin_name; (void)level; (void)event; (void)fields;
  }

 private:
  void register_self() const;
  void unregister_self() const;
};

}  // namespace thin_agent
