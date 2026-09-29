#pragma once
// v0.53.21: 斜杠命令词表解析（纯函数）——/xxx 文本 → 规范命令名。
// 词表外置 chat_policy（slash_commands.aliases），默认内置兜底。
// 独立头文件：零依赖，单测直测（AgentService 侧会话态操作不在此层）。
#include <map>
#include <string>

namespace thin_agent {

/// 默认别名表（配置缺失/损坏时兜底；配置段存在时覆盖）
inline const std::map<std::string, std::string>& slash_alias_defaults() {
  static const std::map<std::string, std::string> kDefaultAlias = {
      {"/new", "/new"},   {"/reset", "/reset"}, {"/clear", "/clear"},
      {"/help", "/help"}, {"/status", "/status"},
      {"/cls", "/clear"}, {"/clean", "/clear"},
      {"/start", "/new"}, {"/fresh", "/new"},
      // v0.53.22 B 档
      {"/stop", "/stop"}, {"/halt", "/stop"},
      {"/model", "/model"}, {"/models", "/model"},
      {"/tools", "/tools"}, {"/tool", "/tools"},
      // v0.53.23 C 档
      {"/compact", "/compact"}, {"/retry", "/retry"},
      {"/sessions", "/sessions"},
  };
  return kDefaultAlias;
}

/// 解析：文本首词（空格分隔）→ 规范命令名；未命中返回空串
/// （"/new xxx" 带参数也认；非 / 开头返回空=穿透）
inline std::string slash_parse(const std::string& text,
                               const std::map<std::string, std::string>& alias) {
  if (text.empty() || text[0] != '/') return "";
  std::string cmd = text;
  const auto sp = cmd.find(' ');
  if (sp != std::string::npos) cmd.resize(sp);
  auto it = alias.find(cmd);
  return it == alias.end() ? "" : it->second;
}

}  // namespace thin_agent
