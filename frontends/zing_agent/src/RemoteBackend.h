// zing_agent P3: 远程后端（ssh CLI 编排——零依赖，Win10+/macOS/Linux 自带 ssh/scp）
// 主机档案 + 远程探活/安装/启停（thin_agent 上 scp 投放 + nohup 拉起）。
#pragma once

#include <cstdio>
#include <map>
#include <string>
#include <vector>

namespace zing {

struct HostProfile {
  std::string name;       // 显示名
  std::string host;       // ip/hostname
  int ssh_port = 22;
  std::string user;       // 默认 root
  std::string key_path;   // 私钥（空=默认 agent/默认密钥）
  std::string remote_dir = "~/zing";       // 远程部署目录
  int agent_port = 8765;  // 远程 thin_agent 端口
  std::string auth_token; // v0.53.32: 远程服务鉴权 token（空=不启用）
};

// ssh/scp 命令构造（供测试与实现共用）
std::string ssh_cmd(const HostProfile& h, const std::string& remote_cmd);
/// v3: 后台拉起专用（ssh -f 本地即刻返回，不等远端会话结束）
std::string ssh_f_cmd(const HostProfile& h, const std::string& remote_cmd);
std::string scp_to_cmd(const HostProfile& h, const std::string& local,
                       const std::string& remote);

// 执行命令（超时秒；返回 rc/stdout/stderr）
struct ExecResult { int rc = -1; std::string out; std::string err; };
ExecResult run_cmd(const std::string& cmd, int timeout_sec = 20);

class RemoteBackend {
 public:
  explicit RemoteBackend(HostProfile p) : p_(std::move(p)) {}

  // 连通性（ssh echo 探测）
  bool reachable(int timeout_sec = 10);

  // 远程 thin_agent 健康吗（curl /stats || wget ——远程侧工具自适应）
  bool agent_healthy(int timeout_sec = 10);

  // 安装：本地二进制+env+config scp 投放（幂等——文件存在跳过）
  // 返回 0=ok / 非 0=失败（err 有说明）
  int install(const std::string& local_bin, const std::string& local_env,
              const std::string& local_config, std::string* err = nullptr);

  // 远程拉起（nohup 后台）——已健康则跳过
  int start(std::string* err = nullptr);

  // 远程停止（pidfile kill）
  int stop(std::string* err = nullptr);

  // ws:// URL（客户端直连——远程 8765 需可达）
  std::string ws_url() const {
    // v17: token 百分号编码——手填 token 含 & = # 空格时 URL 解析异常/截断
    std::string tok;
    for (char c : p_.auth_token) {
      const bool safe = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                        (c >= '0' && c <= '9') || c == '-' || c == '_' ||
                        c == '.' || c == '~';
      if (safe) tok += c;
      else {
        char buf[4];
        snprintf(buf, sizeof(buf), "%%%02X",
                 static_cast<unsigned char>(c));
        tok += buf;
      }
    }
    return "ws://" + p_.host + ":" + std::to_string(p_.agent_port) + "/ws" +
           (tok.empty() ? "" : "?token=" + tok);
  }

  const HostProfile& profile() const { return p_; }

 private:
  HostProfile p_;
};

}  // namespace zing
