#pragma once
/// v0.52.26: FC 运行日志（journal）——断点续跑 #2 的持久化层。
///
/// 背景：FC 循环全部状态在栈上（messages/迭代数/pending 审批在内存），
/// 服务重启 = 进行中的多步任务全丢。本类把"可恢复运行"落 SQLite：
///   - fc_runs       每个 FC run 一行，工具边界追加 messages 快照（增量）
///   - fc_approvals  pending 审批持久化（重启后重新推送审批事件）
///
/// 语义约定：
///   - 落盘点=每个工具边界之后（不是每轮全量），写放大受控
///   - 重启扫描把 status=running 的标记 interrupted（一次性）
///   - resume 由 AgentService 驱动：读快照→重建 messages→续跑 FC

#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace thin_agent {

/// 单条 FC 运行记录。
struct FcRun {
  std::string run_id;       ///< ckpt_<ms>_<seq>
  std::string session_id;   ///< 所属会话
  std::string user_text;    ///< 触发本次 run 的原始用户输入
  std::string status;       ///< running | done | failed | interrupted
  int iter{0};              ///< 最后落盘时已消耗迭代数
  nlohmann::json messages;  ///< ChatMessage 数组快照（增量 JSON）
  int64_t updated_at_ms{0};
};

/// 持久化的待审批（与内存版 PendingApproval 对应）。
struct FcPendingApproval {
  std::string run_id;
  std::string session_id;
  std::string tool_name;
  std::string tool_args;    ///< JSON 字符串
  int fc_iterations_used{0};
  std::string user_text;
  int64_t created_at_ms{0};
};

class FcRunStore {
public:
  FcRunStore();
  ~FcRunStore();

  /// 打开（不存在则创建）+ 建表。失败返回 false。
  bool open(const std::string& db_path);

  /// —— run 生命周期 ——
  /// 创建新 run（status=running）。
  bool create_run(const FcRun& run);
  /// 工具边界追加消息（增量：只存新增的 ChatMessage JSON 数组）+ iter。
  bool append_messages(const std::string& run_id, int iter,
                       const nlohmann::json& new_messages);
  /// 终态更新：done / failed / interrupted。
  bool finish_run(const std::string& run_id, const std::string& status);
  /// 重启清扫：所有 running → interrupted。返回受影响行数。
  int mark_interrupted_on_boot();
  /// 取一个可恢复 run（interrupted，最新优先）。无则返回空 run_id。
  FcRun load_resumable(const std::string& session_id);
  /// 列出指定会话全部 run（调试/审计用）。
  std::vector<FcRun> list_runs(const std::string& session_id, int limit = 20);

  /// —— 审批持久化 ——
  bool save_approval(const FcPendingApproval& pa);
  /// 取走（并删除）指定会话的待审批。无则返回空 run_id。
  FcPendingApproval take_approval(const std::string& session_id);
  /// 审批过期清理（TTL 秒）。返回清除数。
  int purge_expired_approvals(int ttl_sec);

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace thin_agent
