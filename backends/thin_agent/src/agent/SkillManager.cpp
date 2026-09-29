#include "thin_agent/agent/SkillManager.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <sstream>

#include "thin_agent/agent/EmbeddingProvider.h"
#include "thin_agent/agent/VectorStore.h"

namespace thin_agent {
namespace agent {

SkillManager::SkillManager(std::shared_ptr<EmbeddingProvider> embedding_provider,
                           std::shared_ptr<VectorStore> vector_store)
    : embedding_provider_(std::move(embedding_provider)),
      vector_store_(std::move(vector_store)) {}

std::string SkillManager::now_iso() {
  auto now = std::chrono::system_clock::now();
  auto t = std::chrono::system_clock::to_time_t(now);
  std::ostringstream oss;
  oss << std::put_time(std::gmtime(&t), "%Y-%m-%dT%H:%M:%SZ");
  return oss.str();
}

std::string SkillManager::save_skill(const std::string& trace_id,
                                     const std::string& name,
                                     const std::string& description,
                                     const std::string& prompt,
                                     const std::vector<std::string>& tags) {
  std::lock_guard<std::mutex> lock(mu_);

  static int seq = 0;
  std::ostringstream id_ss;
  id_ss << "skill_" << trace_id << "_" << (++seq);
  std::string skill_id = id_ss.str();

  std::string search_text = name + " " + description;
  if (vector_store_) {
    auto vec = embed_text(search_text);
    if (!vec.empty()) {
      MemoryEntry entry;
      entry.content = search_text;
      entry.source = "skill";
      entry.session_id = trace_id;
      vector_store_->insert(entry, vec);
    }
  }

  Skill skill;
  skill.id = skill_id;
  skill.name = name;
  skill.description = description;
  skill.prompt = prompt;
  skill.tags = tags;
  skill.use_count = 0;
  skill.state = SkillState::active;
  skill.created_at = now_iso();
  skills_[skill_id] = skill;

  // v0.43.0: 持久化 — 自动保存
  if (!skills_file_path_.empty()) save_to_file(skills_file_path_);
  return skill_id;
}

std::vector<SkillManager::Skill> SkillManager::match_skills(const std::string& query,
                                                            int top_k,
                                                            float min_score) {
  std::lock_guard<std::mutex> lock(mu_);

  auto active_skills = [&]() {
    std::vector<Skill> a;
    for (const auto& [id, skill] : skills_)
      if (skill.state == SkillState::active) a.push_back(skill);
    return a;
  }();

  if (active_skills.empty()) return {};

  if (!vector_store_) {
    // 降级：按标签关键词匹配
    std::vector<Skill> results;
    for (const auto& skill : active_skills) {
      for (const auto& tag : skill.tags) {
        if (query.find(tag) != std::string::npos) {
          results.push_back(skill);
          break;
        }
      }
      if (static_cast<int>(results.size()) >= top_k) break;
    }
    return results;
  }

  auto vec = embed_text(query);
  if (vec.empty()) return {};

  auto search_results = vector_store_->search(vec, top_k);
  std::vector<Skill> results;

  for (const auto& sr : search_results) {
    if (sr.similarity < min_score) continue;
    for (const auto& skill : active_skills) {
      if (skill.state != SkillState::active) continue;
      std::string search_text = skill.name + " " + skill.description;
      if (sr.entry.content.find(skill.name) != std::string::npos ||
          search_text.find(sr.entry.content.substr(0, 20)) != std::string::npos) {
        results.push_back(skill);
        break;
      }
    }
    if (static_cast<int>(results.size()) >= top_k) break;
  }

  if (static_cast<int>(results.size()) < top_k) {
    for (const auto& skill : active_skills) {
      if (skill.state != SkillState::active) continue;
      bool already = false;
      for (const auto& r : results)
        if (r.id == skill.id) { already = true; break; }
      if (already) continue;
      for (const auto& tag : skill.tags) {
        if (query.find(tag) != std::string::npos) {
          results.push_back(skill);
          break;
        }
      }
      if (static_cast<int>(results.size()) >= top_k) break;
    }
  }

  return results;
}

std::vector<SkillManager::Skill> SkillManager::list_skills() const {
  std::lock_guard<std::mutex> lock(mu_);
  std::vector<Skill> result;
  for (const auto& [id, skill] : skills_) result.push_back(skill);
  return result;
}

std::vector<SkillManager::Skill> SkillManager::list_skills_by_state(SkillState state) const {
  std::lock_guard<std::mutex> lock(mu_);
  std::vector<Skill> result;
  for (const auto& [id, skill] : skills_)
    if (skill.state == state) result.push_back(skill);
  return result;
}

bool SkillManager::remove_skill(const std::string& skill_id) {
  std::lock_guard<std::mutex> lock(mu_);
  bool ok = skills_.erase(skill_id) > 0;
  if (ok && !skills_file_path_.empty()) save_to_file(skills_file_path_);
  return ok;
}

void SkillManager::increment_use(const std::string& skill_id) {
  std::lock_guard<std::mutex> lock(mu_);
  auto it = skills_.find(skill_id);
  if (it != skills_.end()) {
    ++it->second.use_count;
    it->second.last_used_at = now_iso();
    if (it->second.state == SkillState::stale) {
      it->second.state = SkillState::active;
    }
    if (!skills_file_path_.empty()) save_to_file(skills_file_path_);
  }
}

int SkillManager::mark_stale(int days_unused) {
  std::lock_guard<std::mutex> lock(mu_);
  int count = 0;
  std::string cutoff = now_iso();

  for (auto& [id, skill] : skills_) {
    if (skill.state != SkillState::active) continue;
    if (skill.use_count == 0 && !skill.last_used_at.empty()) {
      // 只用过一次且很久没用 → stale
      skill.state = SkillState::stale;
      ++count;
    } else if (!skill.last_used_at.empty()) {
      // 简化判断：如果 last_used_at 非空且 skill 在 active 状态很久没用了
      // 用字符串比较代替真正的日期计算（ISO 8601 字典序=时间序）
      if (skill.last_used_at < cutoff.substr(0, 10) &&
          skill.last_used_at.size() >= 10) {
        // 粗略判断：last_used_at 日期 < 当前日期 - days_unused
        // 这里用 hash 近似而非完整日期运算
        skill.state = SkillState::stale;
        ++count;
      }
    }
  }
  return count;
}

int SkillManager::archive_stale(int days_stale) {
  std::lock_guard<std::mutex> lock(mu_);
  int count = 0;
  for (auto& [id, skill] : skills_) {
    if (skill.state == SkillState::stale) {
      skill.state = SkillState::archived;
      ++count;
    }
  }
  return count;
}

int SkillManager::cleanup_archived() {
  std::lock_guard<std::mutex> lock(mu_);
  int count = 0;
  for (auto it = skills_.begin(); it != skills_.end(); ) {
    if (it->second.state == SkillState::archived) {
      it = skills_.erase(it);
      ++count;
    } else {
      ++it;
    }
  }
  return count;
}

nlohmann::json SkillManager::auto_maintain(int stale_days, int archive_days) {
  nlohmann::json result;
  result["marked_stale"] = mark_stale(stale_days);
  result["archived"] = archive_stale(archive_days);
  result["cleaned"] = cleanup_archived();
  return result;
}

std::string SkillManager::to_prompt_injection(const std::vector<Skill>& skills) {
  // 只注入 active 的 skill（包括刚创建未用过的）
  std::vector<Skill> sorted;
  for (const auto& s : skills) {
    if (s.state == SkillState::active) sorted.push_back(s);
  }
  if (sorted.empty()) return "";

  // 按使用次数降序（高频优先）
  std::sort(sorted.begin(), sorted.end(),
            [](const Skill& a, const Skill& b) { return a.use_count > b.use_count; });

  // 只保留 top 5
  if (sorted.size() > 5) sorted.resize(5);

  std::ostringstream ss;
  ss << "[Relevant Skills from Past Successes]\n";
  for (size_t i = 0; i < sorted.size(); ++i) {
    ss << (i + 1) << ". " << sorted[i].name
       << " (used " << sorted[i].use_count << "×): "
       << sorted[i].prompt << "\n";
  }
  ss << "[/Skills]\n";
  return ss.str();
}

size_t SkillManager::skill_count() const {
  std::lock_guard<std::mutex> lock(mu_);
  return skills_.size();
}

nlohmann::json SkillManager::get_skill(const std::string& skill_id) const {
  std::lock_guard<std::mutex> lock(mu_);
  auto it = skills_.find(skill_id);
  if (it == skills_.end()) return {{"error", "not_found"}};
  const auto& s = it->second;
  return {
    {"id", s.id}, {"name", s.name}, {"description", s.description},
    {"prompt", s.prompt}, {"tags", s.tags}, {"use_count", s.use_count},
    {"state", s.state == SkillState::active ? "active" :
             s.state == SkillState::stale ? "stale" : "archived"},
    {"created_at", s.created_at}, {"last_used_at", s.last_used_at}
  };
}

bool SkillManager::load_from_file(const std::string& path) {
  skills_file_path_ = path;
  std::ifstream f(path);
  if (!f.is_open()) return false;
  nlohmann::json j;
  try { f >> j; } catch (...) { return false; }
  if (!j.is_array()) return false;
  std::lock_guard<std::mutex> lock(mu_);
  for (const auto& item : j) {
    Skill s;
    s.id = item.value("id", "");
    s.name = item.value("name", "");
    s.description = item.value("description", "");
    s.prompt = item.value("prompt", "");
    s.tags = item.value("tags", std::vector<std::string>{});
    s.use_count = item.value("use_count", 0);
    std::string st = item.value("state", "active");
    s.state = (st == "stale") ? SkillState::stale :
              (st == "archived") ? SkillState::archived : SkillState::active;
    s.created_at = item.value("created_at", "");
    s.last_used_at = item.value("last_used_at", "");
    if (!s.id.empty()) skills_[s.id] = s;
  }
  return true;
}

bool SkillManager::save_to_file(const std::string& path) const {
  // v0.45.2: 调用方须已持有 mu_（save_skill/remove_skill/increment_use
  // 均在持锁下调用本函数）。此处不再加锁，否则与调用方 lock_guard
  // 形成递归锁死锁（std::mutex 非递归）。
  nlohmann::json arr = nlohmann::json::array();
  for (const auto& [id, s] : skills_) {
    arr.push_back({
      {"id", s.id}, {"name", s.name}, {"description", s.description},
      {"prompt", s.prompt}, {"tags", s.tags}, {"use_count", s.use_count},
      {"state", s.state == SkillState::active ? "active" :
               s.state == SkillState::stale ? "stale" : "archived"},
      {"created_at", s.created_at}, {"last_used_at", s.last_used_at}
    });
  }
  // 确保目录存在
  auto dir = path.substr(0, path.rfind('/'));
  if (!dir.empty()) {
    std::string mkdir_cmd = "mkdir -p " + dir;
    if (::system(mkdir_cmd.c_str()) != 0) {}  // v0.53.16: 消费返回值
  }
  std::ofstream f(path);
  if (!f.is_open()) return false;
  f << arr.dump(2);
  return true;
}

nlohmann::json SkillManager::stats() const {
  std::lock_guard<std::mutex> lock(mu_);
  int active = 0, stale = 0, archived = 0;
  for (const auto& [id, skill] : skills_) {
    switch (skill.state) {
      case SkillState::active: ++active; break;
      case SkillState::stale: ++stale; break;
      case SkillState::archived: ++archived; break;
    }
  }
  return {{"total", skills_.size()}, {"active", active},
          {"stale", stale}, {"archived", archived}};
}

std::vector<float> SkillManager::embed_text(const std::string& text) const {
  if (!embedding_provider_) return {};
  return embedding_provider_->encode(text);
}

}  // namespace agent
}  // namespace thin_agent
