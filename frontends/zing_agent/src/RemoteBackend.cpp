// zing_agent P3: 远程后端实现（ssh CLI 编排）
#include "RemoteBackend.h"
#ifdef _WIN32
#include <process.h>
#endif

#include <cstdlib>
#include <cstring>
#include <ctime>
#ifndef _WIN32
#include <sys/wait.h>
#else
#include <windows.h>
#define WEXITSTATUS(x) (x)
#endif

namespace zing {

// v17: 单引号串内转义（start.sh 的 bash -c '...' 内嵌 token 安全）
static std::string escape_for_sq(const std::string& s) {
  std::string out;
  for (char c : s) {
    if (c == '\'') out += "'\\''";
    else out += c;
  }
  return out;
}

static std::string escape_sh(const std::string& s) {
#ifdef _WIN32
  // v17: Windows run_cmd 走 _popen→cmd.exe——单引号不是引号,POSIX 风格
  // 引号串会碎。cmd.exe 语义：双引号包裹+"内部双引号翻倍"转义。
  std::string out = "\"";
  for (char c : s) {
    if (c == '"') out += "\"\"";
    else out += c;
  }
  return out + "\"";
#else
  std::string out = "'";
  for (char c : s) {
    if (c == '\'') out += "'\\''";
    else out += c;
  }
  return out + "'";
#endif
}

std::string ssh_cmd(const HostProfile& h, const std::string& remote_cmd) {
  std::string key;
  if (!h.key_path.empty()) key = " -i " + escape_sh(h.key_path);
  return "ssh -p " + std::to_string(h.ssh_port) + key +
         " -o BatchMode=yes -o ConnectTimeout=8"
         " -o StrictHostKeyChecking=accept-new " +
         escape_sh(h.user + "@" + h.host) + " " + escape_sh(remote_cmd);
}

std::string ssh_f_cmd(const HostProfile& h, const std::string& remote_cmd) {
  std::string key;
  if (!h.key_path.empty()) key = " -i " + escape_sh(h.key_path);
  return "ssh -f -p " + std::to_string(h.ssh_port) + key +
         " -o BatchMode=yes -o ConnectTimeout=8"
         " -o StrictHostKeyChecking=accept-new " +
         escape_sh(h.user + "@" + h.host) + " " + escape_sh(remote_cmd);
}

std::string scp_to_cmd(const HostProfile& h, const std::string& local,
                       const std::string& remote) {
  std::string key;
  if (!h.key_path.empty()) key = " -i " + escape_sh(h.key_path);
  return "scp -P " + std::to_string(h.ssh_port) + key +
         " -o BatchMode=yes -o ConnectTimeout=8"
         " -o StrictHostKeyChecking=accept-new " +
         escape_sh(local) + " " +
         escape_sh(h.user + "@" + h.host + ":" + remote);
}

#ifdef _WIN32
// v4: Windows 实现——_popen + cmd /c；超时经 Job 对象（子进程超时整树击杀，
// _popen 本身无超时机制）。stderr 合并进 stdout（2>&1）简化。
ExecResult run_cmd(const std::string& cmd, int timeout_sec) {
  ExecResult r;
  std::string full = cmd + " 2>&1";
  FILE* fp = ::_popen(full.c_str(), "r");
  if (!fp) { r.rc = -1; r.err = "popen failed"; return r; }
  char buf[4096];
  while (::fgets(buf, sizeof(buf), fp)) r.out += buf;
  const int rc = ::_pclose(fp);
  r.rc = (rc == -1) ? -1 : rc;
  // 注：超时由调用侧 ConnectTimeout+ssh 自身保障（ssh -o ConnectTimeout 已
  // 覆盖连接期；命令期超时 Windows 侧暂无看门狗——远程命令均为短命令）
  return r;
}
#else
ExecResult run_cmd(const std::string& cmd, int timeout_sec) {
  ExecResult r;
  const std::string full = "timeout " + std::to_string(timeout_sec) +
                           " bash -c " + escape_sh(cmd) +
                           " 2>/tmp/zing_p3_err";
  FILE* fp = ::popen(full.c_str(), "r");
  if (!fp) { r.rc = -1; r.err = "popen failed"; return r; }
  char buf[4096];
  while (::fgets(buf, sizeof(buf), fp)) r.out += buf;
  const int rc = ::pclose(fp);
  r.rc = (rc == -1) ? -1 : (rc == 124 ? 124 : WEXITSTATUS(rc));
  // stderr 尾部
  FILE* ef = ::fopen("/tmp/zing_p3_err", "r");
  if (ef) {
    char ebuf[2048];
    size_t n;
    while ((n = ::fread(ebuf, 1, sizeof(ebuf) - 1, ef)) > 0) {
      r.err.append(ebuf, n);
      if (r.err.size() > 4096) break;
    }
    ::fclose(ef);
  }
  return r;
}
#endif

bool RemoteBackend::reachable(int timeout_sec) {
  const auto r = run_cmd(ssh_cmd(p_, "echo zing-ok"), timeout_sec);
  return r.rc == 0 && r.out.find("zing-ok") != std::string::npos;
}

bool RemoteBackend::agent_healthy(int timeout_sec) {
  // v17: /stats → /health——v0.53.32 鉴权后 /stats 无 token 返回 401,
  // probe 恒 false → start() 假失败;/health 是鉴权豁免的存活探针
  const std::string probe =
      "curl -s -m 5 -o /dev/null -w '%{http_code}' http://127.0.0.1:" +
      std::to_string(p_.agent_port) + "/health 2>/dev/null || "
      "wget -q -O /dev/null http://127.0.0.1:" + std::to_string(p_.agent_port) +
      "/health 2>/dev/null && echo 200 || echo 000";
  const auto r = run_cmd(ssh_cmd(p_, probe), timeout_sec);
  return r.rc == 0 && r.out.find("200") != std::string::npos;
}

int RemoteBackend::install(const std::string& local_bin, const std::string& local_env,
                           const std::string& local_config, std::string* err) {
  // v0.53.32: 远程 0.0.0.0 必须鉴权——token 空时自动生成（32 hex 随机）
  if (p_.auth_token.empty()) {
    unsigned char rnd[16];
#ifdef _WIN32
    // v17: BCryptGenRandom 替代 rand()——rand() 种子可预测,token 可伪造
    {
      typedef BOOLEAN (WINAPI *RtlGenRandomFn)(void*, ULONG);
      HMODULE adv = ::GetModuleHandleA("advapi32.dll");
      auto rtl = adv ? reinterpret_cast<RtlGenRandomFn>(
                           ::GetProcAddress(adv, "SystemFunction036"))
                     : nullptr;
      if (rtl && rtl(rnd, sizeof(rnd))) {
        // ok
      } else {
        for (auto& b : rnd) b = static_cast<unsigned char>(::rand());  // 兜底
      }
    }
#else
    FILE* ur = ::fopen("/dev/urandom", "rb");
    if (ur) { size_t got = ::fread(rnd, 1, 16, ur); ::fclose(ur);
              (void)got; }
    else { for (auto& b : rnd) b = static_cast<unsigned char>(::rand()); }
#endif
    static const char* hex = "0123456789abcdef";
    for (auto b : rnd) { p_.auth_token += hex[b >> 4]; p_.auth_token += hex[b & 15]; }
  }
  // 1) 远程目录
  auto r = run_cmd(ssh_cmd(p_, "mkdir -p " + p_.remote_dir + "/bin " +
                                    p_.remote_dir + "/config"), 15);
  if (r.rc != 0) {
    if (err) *err = "mkdir 失败: " + r.err;
    return 1;
  }
  // 2) 投放（幂等：同名跳过——force 可后续加）
  r = run_cmd(ssh_cmd(p_, "test -x " + p_.remote_dir + "/bin/thin_agent && "
                                "echo exists || echo need"), 10);
  const bool need_bin = r.rc == 0 && r.out.find("need") != std::string::npos;
  if (need_bin) {
    // v4: 64MB 二进制+慢链路（跨因特网 ssh 实测 <2MB/s）→60s 必炸；180s
    const auto sr = run_cmd(scp_to_cmd(p_, local_bin, p_.remote_dir + "/bin/thin_agent"), 180);
    if (sr.rc != 0) {
      if (err) *err = "scp bin 失败: " + sr.err;
      return 2;
    }
    run_cmd(ssh_cmd(p_, "chmod +x " + p_.remote_dir + "/bin/thin_agent"), 10);
  }
  if (!local_env.empty()) {
    const auto sr = run_cmd(scp_to_cmd(p_, local_env, p_.remote_dir + "/config/agent.env"), 20);
    if (sr.rc != 0 && err) *err += "scp env 警告: " + sr.err;
  }
  if (!local_config.empty()) {
    const auto sr = run_cmd(scp_to_cmd(p_, local_config, p_.remote_dir + "/config/model.yaml"), 20);
    if (sr.rc != 0 && err) *err += "scp config 警告: " + sr.err;
  }
  // v5: 投放 start.sh（start 的远端逻辑脚本化——嵌套引号经 ssh+bash -c
  // 两层转义后变形/挂起，gdb 实锤 fd 卡死；脚本文件=单层引号稳定）
  {
    const std::string sh =
        "#!/bin/bash\n"
        "cd '" + escape_for_sq(p_.remote_dir) + "' || exit 9\n"
        "setsid nohup bash -c 'set -a; [ -f config/agent.env ] && . config/agent.env; "
        "set +a; exec bin/thin_agent --host 0.0.0.0 --port " + std::to_string(p_.agent_port) +
        " --config config/model.yaml" +
        (p_.auth_token.empty() ? "" :
         " --auth-token '" + escape_for_sq(p_.auth_token) + "'") +
        "' </dev/null >>service.log 2>&1 &\n"
        "sleep 1\n"
        "pgrep -f 'thin_agent.*--port " + std::to_string(p_.agent_port) +
        "' | head -1 > svc.pid\n";
#ifdef _WIN32
    const std::string tmp = std::string(std::getenv("TEMP") ? std::getenv("TEMP") : ".")
                          + "\\zing_start_" + std::to_string(_getpid()) + ".sh";
#else
    const std::string tmp = "/tmp/zing_start_" + std::to_string(::getpid()) + ".sh";
#endif
    { FILE* f = ::fopen(tmp.c_str(), "w");
      if (!f) { if (err) *err += "start.sh 创建失败: " + tmp; return {}; }
      ::fputs(sh.c_str(), f); ::fclose(f); }
    const auto sr = run_cmd(scp_to_cmd(p_, tmp, p_.remote_dir + "/start.sh"), 15);
    ::remove(tmp.c_str());
    if (sr.rc == 0)
      run_cmd(ssh_cmd(p_, "chmod +x " + p_.remote_dir + "/start.sh"), 8);
    else if (err) *err += "scp start.sh 警告: " + sr.err;
  }
  return 0;
}

int RemoteBackend::start(std::string* err) {
  if (agent_healthy()) return 0;  // 已在跑
  // v3: ssh -f 即刻返回（v2 实测：无 -f 时 sshd 等待远端会话内全部进程
  // 退出——服务起了但 ssh 挂 15s 超时报假失败）
  // v5: start.sh 脚本化（install 投放；嵌套引号直传经 bash -c 转义变形——
  // 实测 B_DONE 但服务没起/20s 卡两种症状）。脚本=单层引号零转义。
  const auto r = run_cmd(ssh_cmd(p_, "bash " + p_.remote_dir + "/start.sh"), 25);
  if (r.rc != 0 && !agent_healthy()) {
    if (err) *err = "start 失败: " + r.err;
    return 1;
  }
  // 等健康（v5：先探 svc.pid 进程死活——服务秒退（EADDRINUSE/坏配置）时
  // 8s×16 次=135s 白等，远程测试实测超时假卡死；死进程+2 次确认即判失败）
  for (int i = 0; i < 16; ++i) {
    if (agent_healthy(4)) return 0;
    if (i >= 1) {  // 第 2 轮起：进程都没了→失败（首轮 sleep 1 可能还没写 pid）
      const auto pd = run_cmd(ssh_cmd(p_,
          "test -s " + p_.remote_dir + "/svc.pid && kill -0 $(cat " +
          p_.remote_dir + "/svc.pid) 2>/dev/null && echo alive; cat " +
          p_.remote_dir + "/svc.pid"), 8);
      const std::string& o = pd.out;
      if (o.find("alive") == std::string::npos) {
        // 看尾部日志辅助定位（EADDRINUSE/配置错误）
        const auto lg = run_cmd(ssh_cmd(p_,
            "tail -2 " + p_.remote_dir + "/service.log 2>/dev/null"), 8);
        if (err) *err = "服务进程未存活: " + lg.out.substr(0, 200);
        return 3;
      }
    }
#ifdef _WIN32
    Sleep(500);
#else
    struct timespec ts{0, 500 * 1000 * 1000};
    nanosleep(&ts, nullptr);
#endif
  }
  if (err) *err = "启动后 8s 未健康（查远端 " + p_.remote_dir + "/service.log）";
  return 2;
}

int RemoteBackend::stop(std::string* err) {
  // v2: svc.pid kill + pkill -f 双保险（pid 可能是 bash 包装层）
  const std::string kill_cmd =
      "cd " + p_.remote_dir + " 2>/dev/null; "
      "[ -f svc.pid ] && kill $(cat svc.pid) 2>/dev/null; "
      "pkill -f 'thin_agent.*--port " + std::to_string(p_.agent_port) + "' 2>/dev/null; "
      "rm -f svc.pid; echo done";
  const auto r = run_cmd(ssh_cmd(p_, kill_cmd), 10);
  if (r.rc != 0) {
    if (err) *err = "stop 失败: " + r.err;
    return 1;
  }
  return 0;
}

}  // namespace zing
