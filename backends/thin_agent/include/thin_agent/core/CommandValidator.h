#pragma once

#include <string>

namespace thin_agent {

/// CommandValidator — 危险命令硬阻断，不依赖配置，始终生效。
///
/// 检测以下模式的命令：
/// - 递归删除根目录 (rm -rf /, rm -rf /*, rm -rf ~)
/// - 格式化/覆写磁盘 (mkfs.*, dd if=)
/// - Fork Bomb (:(){ :|:& };:, 循环 fork)
/// - 权限/所有权变更 (chmod 777 /, chown -R /)
/// - 管道到危险命令 (curl ... | sh, wget ... | bash)
/// - 输出重定向到设备文件 (> /dev/sd*, > /dev/nvme*)
///
/// 使用：在 shell_exec handler 中，参数解析后执行前调用。
///   if (!CommandValidator::is_safe(command)) return error;
class CommandValidator {
 public:
  /// 检查命令是否安全执行。返回 false 表示命令被阻止。
  /// @param command  完整的命令字符串
  /// @param reason   输出参数：被阻止的原因（人类可读）
  static bool is_safe(const std::string& command, std::string* reason = nullptr);

 private:
  static bool is_dangerous(const std::string& cmd);
  static bool is_catastrophic(const std::string& cmd);
};

}  // namespace thin_agent
