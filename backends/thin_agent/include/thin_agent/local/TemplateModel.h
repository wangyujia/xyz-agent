#pragma once

#include <string>
#include <unordered_map>
#include <vector>

#include "thin_agent/local/ILocalModel.h"

namespace thin_agent {
namespace local {

/// 基于 chat_policy.json 模板匹配的零依赖模型。
///
/// 注册时自动从 chat_policy 加载 offline_general / offline_chat 模板集。
/// 推理策略：关键词匹配 → 返回对应模板；无匹配 → 返回空字符串。
class TemplateModel : public ILocalModel {
 public:
  TemplateModel();

  std::string name() const override { return "template"; }
  std::string backend() const override { return "template"; }

  std::vector<ModelCapability> capabilities() const override {
    return {{"chat", 0, 2048}};
  }

  std::string infer(const std::string& input,
                    int max_tokens = 256,
                    const std::string& capability = "chat") override;

  bool load() override;
  bool is_loaded() const override { return loaded_; }
  size_t memory_bytes() const override { return 0; }  ///< 纯配置，零内存

 private:
  /// 从 chat_policy.json 加载 offline_* 模板。
  void load_templates();

  /// 简单关键词匹配：在 input 中查找 keywords 的任一命中。
  std::string match_keyword(const std::string& input_lower) const;

  /// 模板表：keyword → reply_text。
  struct TemplateRule {
    std::vector<std::string> keywords;  ///< 命中关键词
    std::string reply;                  ///< 对应回复
  };
  std::vector<TemplateRule> rules_;
  std::string fallback_reply_;  ///< 无命中时的兜底回复
  bool loaded_ = false;
};

}  // namespace local
}  // namespace thin_agent
