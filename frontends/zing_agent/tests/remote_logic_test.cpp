// zing_agent P3: RemoteBackend 纯逻辑单测（无 sshd 也能跑——命令构造层）
// 覆盖：ssh_cmd/scp_to_cmd/ssh_f_cmd 构造（转义/端口/密钥/用户）/
// ws_url / install 幂等判断串 / start launch 串（--host 0.0.0.0 断言）/
// stop pkill 模式（.*--port 宽松锚定断言——W10/⑤回归锚）。
// sshd 自环集成路径在 remote_test（有 sshd 才跑）。
#include "RemoteBackend.h"

#include <cstdio>
#include <iostream>
#include <string>

static int g_failed = 0;
static void check(bool ok, const std::string& name) {
  std::cout << (ok ? "PASS: " : "FAIL: ") << name << "\n";
  if (!ok) g_failed++;
}

int main() {
  zing::HostProfile h;
  h.name = "unit";
  h.host = "192.168.1.50";
  h.ssh_port = 2222;
  h.user = "ubuntu";
  h.key_path = "/home/me/.ssh/id_ed25519";
  h.remote_dir = "~/zing";
  h.agent_port = 8765;

  // ── 命令构造 ──
  {
    const std::string c = zing::ssh_cmd(h, "echo 'a b'");
    check(c.find("ssh -p 2222") != std::string::npos, "ssh_cmd 端口");
    check(c.find("-i '/home/me/.ssh/id_ed25519'") != std::string::npos, "ssh_cmd 密钥（引号转义）");
    check(c.find("'ubuntu@192.168.1.50'") != std::string::npos, "ssh_cmd user@host");
    check(c.find("-o BatchMode=yes") != std::string::npos, "ssh_cmd BatchMode（防交互挂起）");
    check(c.find("-o StrictHostKeyChecking=accept-new") != std::string::npos, "ssh_cmd 首连不交互");
    // 远程命令单引号转义：echo 'a b' → 'echo '\''a b'\'''
    check(c.find(R"('echo '\''a b'\''' )") != std::string::npos ||
          c.find("echo '\\''a b'\\''") != std::string::npos, "远程命令内单引号转义");
  }
  {
    const std::string c = zing::scp_to_cmd(h, "/tmp/bin", "~/zing/bin/thin_agent");
    check(c.find("scp -P 2222") != std::string::npos, "scp 端口大写 -P");
    check(c.find("'ubuntu@192.168.1.50:~/zing/bin/thin_agent'") != std::string::npos, "scp 目标路径");
  }
  {
    const std::string c = zing::ssh_f_cmd(h, "sleep 100");
    check(c.find("ssh -f ") == 0, "ssh_f_cmd 以 ssh -f 开头（后台拉起不挂会话）");
  }
  {
    // 空密钥：不出现 -i
    zing::HostProfile h2 = h;
    h2.key_path = "";
    const std::string c = zing::ssh_cmd(h2, "true");
    check(c.find(" -i ") == std::string::npos, "空密钥不产 -i 参数");
  }

  // ── ws_url ──
  check(zing::RemoteBackend(h).ws_url() == "ws://192.168.1.50:8765/ws", "ws_url 组装");

  // ── launch/pkill 模式回归锚（W10/⑤：真实源码抽验）──
  {
    FILE* f = ::fopen(SRC_DIR "/src/RemoteBackend.cpp", "r");
    if (!f) { std::cout << "SKIP: 源码抽验（无源码文件）\n"; }
    else {
      std::string src;
      char buf[4096];
      size_t n;
      while ((n = ::fread(buf, 1, sizeof(buf), f)) > 0) src.append(buf, n);
      ::fclose(f);
      check(src.find("--host 0.0.0.0") != std::string::npos,
            "源码锚：start 串含 --host 0.0.0.0（远程可达）");
      check(src.find("thin_agent.*--port") != std::string::npos,
            "源码锚：pgrep/pkill 宽松端口锚定（--host 参数不破匹配）");
      check(src.find("180)") != std::string::npos,
            "源码锚：scp 180s 超时（64MB 慢链路）");
    }
  }

  std::cout << (g_failed ? "FAILED\n" : "ALL PASS\n");
  return g_failed ? 1 : 0;
}
