#include "thin_agent/agent/MemoryManager.h"

#include <algorithm>
#include <regex>
#include <sstream>
#include <unordered_set>

namespace thin_agent {
namespace agent {

namespace {

/// 按分隔符拆分字符串。
std::vector<std::string> split(const std::string& s, const std::string& delim) {
  std::vector<std::string> out;
  std::regex re(delim);
  std::sregex_token_iterator it(s.begin(), s.end(), re, -1);
  std::sregex_token_iterator end;
  for (; it != end; ++it) {
    std::string token = it->str();
    if (!token.empty()) out.push_back(token);
  }
  return out;
}

/// 去除首尾空白。
std::string trim(const std::string& s) {
  auto start = s.begin();
  while (start != s.end() && std::isspace(static_cast<unsigned char>(*start)))
    ++start;
  auto end = s.end();
  do {
    --end;
  } while (end > start &&
           std::isspace(static_cast<unsigned char>(*end)));
  return std::string(start, end + 1);
}

}  // namespace

MemoryManager::MemoryManager(std::shared_ptr<EmbeddingProvider> embedding,
                             const std::string& db_path, bool auto_extract)
    : embedding_(std::move(embedding)), auto_extract_(auto_extract) {
  store_.init(db_path, embedding_->dimension());
}

std::vector<std::string> MemoryManager::extract_facts(
    const std::string& user_message, const std::string& assistant_response) {
  std::vector<std::string> facts;
  std::unordered_set<std::string> seen;

  std::string combined =
      "用户: " + user_message + " | 助手: " + assistant_response;

  // 按句号、问号、感叹号、换行拆分
  auto sentences = split(combined, "[。！？\\n]+");

  for (auto& s : sentences) {
    s = trim(s);
    // 过滤太短或太长的——v0.53.41 修:此前 && 恒假(一个 size 不可能
    // 同时 <4 且 >300),太短句("好的"类)全部入库,记忆噪声膨胀稀释 RAG
    if (s.size() < 4 || s.size() > 300) continue;
    // 过滤纯标点/空白
    bool has_content = false;
    for (char c : s) {
      if (std::isalnum(static_cast<unsigned char>(c)) || (c & 0x80)) {
        has_content = true;
        break;
      }
    }
    if (!has_content) continue;
    // 去重
    if (!seen.insert(s).second) continue;
    facts.push_back(s);
  }

  return facts;
}

void MemoryManager::ingest_conversation(
    const std::string& session_id, const std::string& user_message,
    const std::string& assistant_response) {
  if (!auto_extract_) return;

  auto facts = extract_facts(user_message, assistant_response);
  for (const auto& fact : facts) {
    remember(fact, "conversation", session_id, 0.5f);
  }
}

int64_t MemoryManager::remember(const std::string& content,
                                const std::string& source,
                                const std::string& session_id,
                                float importance) {
  if (content.empty()) return -1;

  MemoryEntry entry;
  entry.content = content;
  entry.source = source;
  entry.session_id = session_id;
  entry.importance = importance;

  auto emb = embedding_->encode(content);
  return store_.insert(entry, emb);
}

std::vector<SearchResult> MemoryManager::recall(const std::string& query,
                                                 int top_k) {
  if (query.empty()) return {};
  auto q_emb = embedding_->encode(query);
  return store_.search(q_emb, top_k, 0.3f);
}

std::string MemoryManager::build_context(const std::string& query, int top_k) {
  auto results = recall(query, top_k);
  if (results.empty()) return "";

  std::ostringstream os;
  os << "以下是与此问题相关的历史记忆：\n\n";
  for (size_t i = 0; i < results.size(); ++i) {
    const auto& r = results[i];
    os << "[" << (i + 1) << "] (" << r.entry.source
       << ", 相关度: " << static_cast<int>(r.similarity * 100) << "%)\n";
    os << "    " << r.entry.content << "\n";
    if (!r.entry.session_id.empty()) {
      os << "    (会话: " << r.entry.session_id << ")\n";
    }
    os << "\n";
  }
  return os.str();
}

std::vector<MemoryEntry> MemoryManager::recent(int limit) {
  return store_.recent(limit);
}

nlohmann::json MemoryManager::stats() const { return store_.stats(); }

int MemoryManager::clear_session(const std::string& session_id) {
  return store_.remove_by_session(session_id);
}

}  // namespace agent
}  // namespace thin_agent
