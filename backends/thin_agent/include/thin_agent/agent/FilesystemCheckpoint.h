#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace thin_agent {
namespace agent {

/// 文件系统级检查点 —— 内容寻址存储，支持文件变更回滚
///
/// 存储布局：
///   ~/.thin_agent/checkpoints/
///     <checkpoint_id>/manifest.json    — {"workdir","label","timestamp","files":[{path,sha256,size,mtime}]}
///     objects/<sha256[:2]>/<sha256>    — 内容寻址 blob（多快照自动去重）
///
/// 默认排除：.git/ build/ node_modules/ __pycache__/ .venv/ *.so *.o *.a .env *.log 等
class FilesystemCheckpoint {
 public:
  /// 文件条目
  struct FileEntry {
    std::string path;       // 相对 working directory 的路径
    std::string sha256;     // 内容 SHA-256（64 位十六进制）
    uint64_t    size = 0;
    int64_t     mtime = 0;  // 文件时钟原始刻度（file_time_type::duration::count()，仅供"未变"比较）
    /// v0.54.23: 同一 mtime 换算到 system_clock 毫秒（跨纪元换算，见 .cpp）。
    /// 用途：pre-FC 去重的**竞态守卫** —— 仅当文件 mtime 明显早于上一次快照保存时刻才复用其 sha256。
    int64_t     mtime_sys_ms = 0;
  };

  /// 快照元数据
  struct Snapshot {
    std::string             checkpoint_id;
    std::string             workdir;
    std::string             label;
    int64_t                 timestamp = 0;   // epoch 毫秒
    std::vector<FileEntry>  files;
  };

  /// 回滚结果
  struct RollbackResult {
    bool ok = false;
    std::string error;
    std::vector<std::string> restored;   // 已恢复（内容变化）
    std::vector<std::string> skipped;    // 未变化（跳过）
    std::vector<std::string> added;      // 新文件（快照中不存在）
    std::vector<std::string> removed;    // 已删除（快照中存在，当前不存在）
  };

  /// @param base_dir  存储根目录（默认 ~/.thin_agent/checkpoints/）
  explicit FilesystemCheckpoint(const std::string& base_dir = "");

  // ── 核心操作 ──

  /// 保存快照 —— 扫描 workdir，存储新 blob，写入 manifest
  /// @return checkpoint_id，失败返回空字符串
  /// v0.54.23: 上一次 save() 中"因未变而复用 sha256"的文件数（诊断/测试用；去重的唯一可观测出口）
  int last_reused_count() const;

  std::string save(const std::string& workdir,
                   const std::vector<std::string>& custom_excludes = {},
                   const std::string& label = "manual",
                   int max_checkpoints = 20);

  /// 回滚文件 —— 按快照恢复 workdir 中的文件
  RollbackResult rollback(const std::string& checkpoint_id,
                          bool dry_run = false);

  /// 差异 —— 列出与快照相比有变化的文件
  /// @return {"path": "<path>", "status": "modified|added|removed", ...}
  nlohmann::json diff(const std::string& checkpoint_id);

  // ── 管理操作 ──

  /// 列出所有快照（按时间倒序）
  std::vector<Snapshot> list() const;

  /// 删除指定快照（不删除仍被其他快照引用的 blob）
  bool remove(const std::string& checkpoint_id);

  /// 修剪旧快照，保留最近 keep_count 个
  int prune(int keep_count = 20);

  /// 统计信息
  nlohmann::json stats() const;

  // ── 自动保存（turn 级速率限制）──
  // 在一次 FC turn 中多次写文件前只保存一次

  /// 标记当前 turn 有文件变更（调用后 auto_save 在下一次调用时生效）
  void mark_dirty() { turn_dirty_ = true; }

  /// 自动保存（同 turn 多次调用只执行一次）。需要先 mark_dirty()。
  /// @return checkpoint_id，无变更或已保存返回空字符串
  std::string auto_save(const std::string& workdir,
                        const std::string& label = "auto_fc");

  /// 重置 turn 状态（每个新的用户请求开始时调用）
  void reset_turn() { turn_dirty_ = false; turn_saved_ = false; }

  // ── 默认排除列表 ──
  static const std::vector<std::string>& default_excludes();

 private:
  std::string base_dir_;
  bool turn_dirty_ = false;
  bool turn_saved_ = false;
  int last_reused_ = 0;   // v0.54.23: 上次 save() 的复用量（诊断/测试出口）
  mutable std::mutex mu_;

  // 路径 helper
  std::string objects_dir() const;
  std::string manifest_path(const std::string& ckpt_id) const;
  std::string blob_path(const std::string& sha256) const;
  static std::string checkpoint_dir(const std::string& base, const std::string& ckpt_id);
  std::vector<Snapshot> list_nolock() const;
  RollbackResult rollback_nolock(const std::string& checkpoint_id, bool dry_run);

  // 文件操作
  static std::string compute_sha256(const std::string& filepath);
  bool store_blob(const std::string& src_path, const std::string& sha256);
  bool restore_blob(const std::string& sha256, const std::string& dst_path);
  static std::vector<FileEntry> scan_workdir(const std::string& workdir,
                                              const std::vector<std::string>& excludes);
  static bool is_excluded(const std::string& rel_path,
                          const std::vector<std::string>& excludes);
};

/// 默认检查点根目录
inline std::string default_checkpoints_dir() {
  // Reuse THIN_AGENT_HOME / HOME from RuntimePaths convention
  const char* env_home = std::getenv("THIN_AGENT_HOME");
  if (!env_home || !*env_home) env_home = std::getenv("HOME");
  std::string home = (env_home && *env_home) ? env_home : ".";
  return home + "/.thin_agent/checkpoints";
}

}  // namespace agent
}  // namespace thin_agent
