#pragma once
// v0.53.20: 沙箱路径视图重写——shell_exec 沙箱模式下入参 /tmp→/host_tmp
// 与输出反重写 /host_tmp→/tmp 的纯函数对（进出统一宿主 /tmp 视图，
// LLM 无需感知沙箱内部路径约定）。独立头文件：零依赖，单测直测。
#include <cctype>
#include <string>

namespace thin_agent {

/// 命令入参重写：/tmp 词边界 → /host_tmp（/tmp、/tmp/、/tmp/x 换；
/// /tmpxyz、/tmpfs 等前缀撞名不换；已是 /host_tmp 形式不二次换）
inline std::string rewrite_sandbox_tmp_paths(const std::string& in) {
  const std::string kHost = "/host_tmp";
  std::string command = in;
  size_t pos = 0;
  while ((pos = command.find("/tmp", pos)) != std::string::npos) {
    const bool boundary =
        pos + 4 == command.size() ||
        (!isalnum(static_cast<unsigned char>(command[pos + 4])) &&
         command[pos + 4] != '_');
    const bool already_host =
        pos >= 6 && command.compare(pos - 6, 6, "/host_") == 0;
    if (boundary && !already_host) {
      command.replace(pos, 4, kHost);
      pos += kHost.size();
    } else {
      pos += 4;
    }
  }
  return command;
}

/// 命令输出反重写：/host_tmp → /tmp（宿主视图）
inline std::string unrewrite_sandbox_tmp_paths(const std::string& in) {
  std::string out = in;
  size_t opos = 0;
  while ((opos = out.find("/host_tmp", opos)) != std::string::npos) {
    out.replace(opos, 9, "/tmp");
    opos += 4;
  }
  return out;
}

}  // namespace thin_agent
