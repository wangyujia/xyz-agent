#pragma once

#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

namespace thin_agent {
namespace agent {

class EmbeddingProvider;
class VectorStore;

/// Skill 状态枚举。
enum class SkillState { active, stale, archived };

/// Skill 管理器：从成功的 AgentLoop trace 中提取 skill，
/// 通过嵌入向量相似度匹配，注入到 system prompt 中复用。
/// v0.10.0: 增加完整生命周期管理（使用计数 + 陈旧检测 + 自动归档）。
class SkillManager {
 public:
  struct Skill {
    std::string id;
    std::string name;
    std::string description;
    std::string prompt;
    std::vector<std::string> tags;
    int use_count{0};
    SkillState state{SkillState::active};
    std::string created_at;    ///< ISO 8601
    std::string last_used_at;  ///< ISO 8601，从未使用则为空
  };

  SkillManager(std::shared_ptr<EmbeddingProvider> embedding_provider,
               std::shared_ptr<VectorStore> vector_store);

  /// 保存 skill，返回 skill_id。
  std::string save_skill(const std::string& trace_id,
                         const std::string& name,
                         const std::string& description,
                         const std::string& prompt,
                         const std::vector<std::string>& tags);

  /// 按用户输入匹配最相关的 skills（仅 active）。
  std::vector<Skill> match_skills(const std::string& query,
                                 int top_k = 3,
                                 float min_score = 0.5f);

  /// 列出所有 skills。
  std::vector<Skill> list_skills() const;

  /// 列出指定状态的 skills。
  std::vector<Skill> list_skills_by_state(SkillState state) const;

  /// 删除 skill。
  bool remove_skill(const std::string& skill_id);

  /// 增加使用计数并刷新 last_used_at。
  void increment_use(const std::string& skill_id);

  /// 将超过 N 天未使用的 active skill 标记为 stale。
  /// @return 标记数量。
  int mark_stale(int days_unused = 30);

  /// 将超过 N 天的 stale skill 归档。
  /// @return 归档数量。
  int archive_stale(int days_stale = 60);

  /// 彻底删除已归档 skill。
  int cleanup_archived();

  /// 自动维护：stale 检测 + 归档 + 清理。
  /// 适合 CronScheduler 定期调用。
  /// @return 各阶段操作计数 {"marked_stale": N, "archived": N, "cleaned": N}
  nlohmann::json auto_maintain(int stale_days = 30, int archive_days = 60);

  /// 将匹配到的 skills 拼接为 system prompt 注入片段。
  /// 只注入 active + use_count>0 的 skill（按使用次数降序）。
  static std::string to_prompt_injection(const std::vector<Skill>& skills);

  /// 已保存 skill 数量。
  size_t skill_count() const;

  /// 获取指定 skill 的完整信息。
  nlohmann::json get_skill(const std::string& skill_id) const;

  /// v0.43.0: 持久化 — 加载/保存 skills 到 JSON 文件。保存目录自动创建。
  bool load_from_file(const std::string& path);
  bool save_to_file(const std::string& path) const;

  /// 各状态数量统计。
  nlohmann::json stats() const;

 private:
  std::vector<float> embed_text(const std::string& text) const;
  static std::string now_iso();

  std::shared_ptr<EmbeddingProvider> embedding_provider_;
  std::shared_ptr<VectorStore> vector_store_;
  mutable std::mutex mu_;
  std::unordered_map<std::string, Skill> skills_;
  /// v0.43.0: 持久化文件路径，在 load_from_file 时设置
  std::string skills_file_path_;
};

}  // namespace agent
}  // namespace thin_agent
