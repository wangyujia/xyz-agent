#include "thin_agent/core/CommandValidator.h"

#include <algorithm>
#include <cctype>
#include <sstream>
#include <string>
#include <vector>

namespace thin_agent {

namespace {

// 将命令标准化为小写并去除前导空白
std::string normalize(const std::string& cmd) {
  std::string out = cmd;
  // trim left
  while (!out.empty() && std::isspace(static_cast<unsigned char>(out.front())))
    out.erase(out.begin());
  std::transform(out.begin(), out.end(), out.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return out;
}

// 检查命令中是否包含指定子串（大小写不敏感）
bool contains_token(const std::string& cmd, const std::string& token) {
  return cmd.find(token) != std::string::npos;
}

// 检查是否包含正则模式的子串（简单实现：搜索独立单词出现）
bool contains_word(const std::string& cmd, const std::string& word) {
  size_t pos = 0;
  while (true) {
    pos = cmd.find(word, pos);
    if (pos == std::string::npos) return false;
    // 检查前后是否是单词边界
    bool left_ok = (pos == 0 || !std::isalnum(static_cast<unsigned char>(cmd[pos - 1])));
    size_t end = pos + word.size();
    bool right_ok = (end >= cmd.size() || !std::isalnum(static_cast<unsigned char>(cmd[end])));
    if (left_ok && right_ok) return true;
    ++pos;
  }
}

// ── 灾难性模式：无条件阻止，不允许任何例外 ──
// 注意：所有模式必须全小写（匹配前命令会被 normalize 转小写）
static const char* kCatastrophicPatterns[] = {
  // Fork Bomb
  ":(){ :|:& };:",
  ":(){ :|: & };:",
  // 递归删除根文件系统
  "rm -rf /",
  "rm -r /",
  "rm -rf /*",
  "rm -rf ~",
  "rm -rf $home",
  "rm -rf /home",
  "rm -rf /etc",
  "rm -rf /usr",
  "rm -rf /bin",
  "rm -rf /sbin",
  "rm -rf /lib",
  "rm -rf /boot",
  "rm -rf /var",
  // 格式化磁盘
  "mkfs.",
  "mke2fs /",
  // dd 覆写磁盘
  "dd if=",
  // chmod 777 危险路径
  "chmod 777 /",
  "chmod -r 777 /",
  // chown 危险路径
  "chown -r /",
  // 禁用安全机制
  "setenforce 0",
  "sysctl -w kernel.randomize_va_space=0",
  // iptables 清空
  "iptables -f",
  "iptables -p input accept",
  "iptables -p output accept",
  // 删除内核模块
  "modprobe -r",
  "rmmod",
  // 强制卸载
  "umount -f /",
  // 环境变量注入危险命令
  "eval ",
};
constexpr int kCatastrophicCount = sizeof(kCatastrophicPatterns) / sizeof(kCatastrophicPatterns[0]);

// ── 管道执行检测：curl/wget + 管道到 shell ──
bool has_pipe_to_shell(const std::string& cmd) {
  auto pipe_pos = cmd.find('|');
  if (pipe_pos == std::string::npos) return false;

  std::string left = cmd.substr(0, pipe_pos);
  std::string right = cmd.substr(pipe_pos + 1);

  bool left_is_fetch = (contains_token(left, "curl") || contains_token(left, "wget"));
  bool right_is_shell = (contains_token(right, "sh") || contains_token(right, "bash") ||
                          contains_token(right, "zsh") || contains_token(right, "perl") ||
                          contains_token(right, "python") || contains_token(right, "ruby"));

  return left_is_fetch && right_is_shell;
}

// ── 危险项（组合检测）──
bool has_output_redirect_to_device(const std::string& cmd) {
  if (cmd.find(">") == std::string::npos) return false;
  return (cmd.find("/dev/sd") != std::string::npos ||
          cmd.find("/dev/nvme") != std::string::npos ||
          cmd.find("/dev/xvd") != std::string::npos ||
          cmd.find("/dev/hd") != std::string::npos ||
          cmd.find("/dev/mmcblk") != std::string::npos ||
          cmd.find("/dev/dm-") != std::string::npos ||
          cmd.find("/dev/loop") != std::string::npos);
}

}  // namespace

bool CommandValidator::is_dangerous(const std::string& cmd) {
  std::string norm = normalize(cmd);

  // ── 1. 管道到 shell（curl | sh 等）──
  if (has_pipe_to_shell(norm)) return true;

  // ── 2. 输出重定向到块设备 ──
  if (has_output_redirect_to_device(norm)) return true;

  return false;
}

bool CommandValidator::is_catastrophic(const std::string& cmd) {
  std::string norm = normalize(cmd);

  for (int i = 0; i < kCatastrophicCount; ++i) {
    if (norm.find(kCatastrophicPatterns[i]) != std::string::npos) {
      return true;
    }
  }
  return false;
}

bool CommandValidator::is_safe(const std::string& command, std::string* reason) {
  std::string norm = normalize(command);

  // 灾难性模式（最高优先级，立即拒绝）
  for (int i = 0; i < kCatastrophicCount; ++i) {
    if (norm.find(kCatastrophicPatterns[i]) != std::string::npos) {
      if (reason) *reason = std::string("catastrophic_pattern: ") + kCatastrophicPatterns[i];
      return false;
    }
  }

  // 管道到 shell
  if (has_pipe_to_shell(norm)) {
    if (reason) *reason = "pipe_to_shell: curl/wget piped to shell interpreter";
    return false;
  }

  // 输出重定向到块设备
  if (has_output_redirect_to_device(norm)) {
    if (reason) *reason = "redirect_to_device: output redirect to block device";
    return false;
  }

  return true;
}

}  // namespace thin_agent
