// P3: RemoteBackend 单测（ssh 自环 127.0.0.1:2222）
#include "RemoteBackend.h"
#include <cstdio>
#include <iostream>

int main() {
  int failed = 0;
  auto check = [&](bool ok, const std::string& name) {
    std::cout << (ok ? "PASS: " : "FAIL: ") << name << "\n";
    if (!ok) failed++;
  };

  // sshd 自环探测：无 sshd 则跳过（exit 77）
  {
    const auto r = zing::run_cmd("echo probe-self", 5);
    if (r.rc != 0) { std::cout << "SKIP: run_cmd 不可用\n"; return 77; }
    // 端口 2222 是否有 sshd 监听（无则本环境无测试 sshd）
    const auto p = zing::run_cmd(
        "bash -c '</dev/tcp/127.0.0.1/2222' 2>/dev/null && echo yes", 5);
    if (p.out.find("yes") == std::string::npos) {
      std::cout << "SKIP: 无测试 sshd（2222 未监听；CI/开发机常态）\n";
      return 77;
    }
  }

  zing::HostProfile h;
  h.name = "loop";
  h.host = "127.0.0.1";
  h.ssh_port = 2222;
  h.user = "root";
  h.key_path = "/root/.ssh/id_ed25519";
  h.remote_dir = "/tmp/zing_remote_test";
  h.agent_port = 18765;
  zing::RemoteBackend rb(h);

  // 1) 命令构造（转义正确性）
  const std::string c = zing::ssh_cmd(h, "echo 'a b'");
  check(c.find("ssh -p 2222") != std::string::npos &&
        c.find("-o BatchMode=yes") != std::string::npos &&
        c.find("root@127.0.0.1") != std::string::npos, "ssh_cmd 构造");
  // 2) 可达
  check(rb.reachable(), "reachable（ssh 自环）");
  // 3) 未安装时 agent 不健康
  check(!rb.agent_healthy(), "agent_healthy 初始 false");
  // 4) 安装（真实 scp 投放本地 thin_agent 二进制）
  std::string err;
  const int irc = rb.install("/root/code/thin_agent/build/thin_agent",
                             "/root/.thin_agent/zai.env",
                             "/root/code/thin_agent/config/demo.model.yaml", &err);
  check(irc == 0, "install scp 投放 " + (irc ? err : ""));
  // 5) 远程文件落位
  {
    const auto r = zing::run_cmd(zing::ssh_cmd(h, "test -x /tmp/zing_remote_test/bin/thin_agent && echo OK"), 10);
    check(r.rc == 0 && r.out.find("OK") != std::string::npos, "远程二进制可执行");
  }
  // ── 全链：start → 绑定 0.0.0.0 → stop → 净环境 ──
  //（四轮审计 W10/⑤ 的 e2e 固化；GLM key 经 env 文件注入——zai.env 若无
  // 则服务起后健康检查退化为端口监听判定，不阻断）
  {
    zing::run_cmd(zing::ssh_cmd(h,
        "pkill -f 'thin_agent.*--port " + std::to_string(h.agent_port) + "' 2>/dev/null; sleep 1"), 10);
    std::string err2;
    const int src_ = rb.start(&err2);
    check(src_ == 0, "start 远程拉起（ssh -f + setsid）" + (err2.empty() ? "" : " err=" + err2));
    // 绑定面验证：必须 0.0.0.0（127.0.0.1 = W10 复发）
    {
      const auto r = zing::run_cmd(
          zing::ssh_cmd(h, "ss -tln | grep '0.0.0.0:" + std::to_string(h.agent_port) + "' | grep -cv '127.0.0.1'"),
          10);
      check(!r.out.empty() && r.out[0] != '0',
            "绑定 0.0.0.0:" + std::to_string(h.agent_port) + "（非 127.0.0.1）");
    }
    check(rb.stop() == 0, "stop（svc.pid + pkill .*--port 宽松锚定）");
    {
      const auto r = zing::run_cmd(
          zing::ssh_cmd(h, "ss -tln | grep -c ':" + std::to_string(h.agent_port) + "' || echo 0"), 10);
      check(r.out.find("0") == 0, "stop 后端口净（无监听）");
    }
  }
  // 6) 清场
  zing::run_cmd(zing::ssh_cmd(h, "rm -rf /tmp/zing_remote_test"), 10);
  std::cout << (failed ? "FAILED\n" : "ALL PASS\n");
  return failed ? 1 : 0;
}
