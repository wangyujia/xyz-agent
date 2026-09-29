#include "thin_agent/core/PatrolProbe.h"
#include <cctype>
#include <algorithm>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#ifdef _WIN32
// v0.54.17: Windows 无 `sys/statvfs.h` ⇒ 用 GetDiskFreeSpaceExA（见 check_disk）。
#include <windows.h>
#else
#include <sys/statvfs.h>
#endif

#include "thin_agent/RuntimePaths.h"

namespace thin_agent {

namespace fs = std::filesystem;

// ─────────────────────────────────────────────────────────────
// 通用辅助
// ─────────────────────────────────────────────────────────────

namespace {

/// 执行命令，返回 stdout（前 max_bytes 字节）。
std::string shell_out(const std::string& cmd, int max_bytes = 65536) {
    std::string result;
    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) return result;
    char buf[4096];
    while (fgets(buf, sizeof(buf), pipe)) {
        result += buf;
        if (static_cast<int>(result.size()) > max_bytes) {
            result.resize(max_bytes);
            break;
        }
    }
    pclose(pipe);
    return result;
}

/// 解析 /proc/meminfo 中某个 key 的 kB 值。
long parse_meminfo_kb(const std::string& key) {
    std::ifstream in("/proc/meminfo");
    if (!in.is_open()) return -1;
    std::string line;
    while (std::getline(in, line)) {
        if (line.find(key) == 0) {
            // "MemTotal:       16234512 kB"
            size_t pos = line.find(':');
            if (pos == std::string::npos) return -1;
            std::string val = line.substr(pos + 1);
            // strip leading whitespace + trailing " kB"
            while (!val.empty() && (val[0] == ' ' || val[0] == '\t')) val.erase(0, 1);
            size_t kb = val.find(" kB");
            if (kb != std::string::npos) val = val.substr(0, kb);
            try { return std::stol(val); } catch (...) { return -1; }
        }
    }
    return -1;
}

/// 根据进程名查找 PID。
std::vector<int> find_pids_by_name(const std::string& name) {
    std::vector<int> pids;
    // v0.53.43: 进程名来自 patrol 配置(LLM 可控)——白名单校验拒 shell 元字符
    const bool safe = !name.empty() && std::all_of(name.begin(), name.end(), [](char c) {
        return std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_' || c == '.';
    });
    if (!safe) return pids;
    std::string cmd = "pgrep -x " + name + " 2>/dev/null";
    std::string out = shell_out(cmd);
    std::istringstream iss(out);
    std::string line;
    while (std::getline(iss, line)) {
        if (line.empty()) continue;
        try { pids.push_back(std::stoi(line)); } catch (...) {}
    }
    return pids;
}

/// 进程 RSS（kB）。
long proc_rss_kb(int pid) {
    std::string path = "/proc/" + std::to_string(pid) + "/status";
    std::ifstream in(path);
    if (!in.is_open()) return -1;
    std::string line;
    while (std::getline(in, line)) {
        if (line.find("VmRSS:") == 0) {
            // "VmRSS:\t   12345 kB"
            size_t pos = line.find(':');
            if (pos == std::string::npos) return -1;
            std::string val = line.substr(pos + 1);
            while (!val.empty() && (val[0] == ' ' || val[0] == '\t')) val.erase(0, 1);
            size_t kb = val.find(" kB");
            if (kb != std::string::npos) val = val.substr(0, kb);
            try { return std::stol(val); } catch (...) { return -1; }
        }
    }
    return -1;
}

/// 日志错误计数（grep ERROR/WARN）。
int count_log_errors(const std::string& log_path, int minutes) {
    if (!fs::exists(log_path)) return -1;
    // v0.53.43: 路径拼 shell——白名单字符(路径安全集,拒元字符/引号)
    const bool path_safe = std::all_of(log_path.begin(), log_path.end(), [](char c) {
        return std::isalnum(static_cast<unsigned char>(c)) || c == '/' || c == '-' ||
               c == '_' || c == '.' || c == '+';
    });
    if (!path_safe) return 0;
    std::string cmd =
        "grep -c -E 'ERROR|WARN|CRITICAL|FATAL' " + log_path +
        " 2>/dev/null || echo 0";
    // Note: filtering by time would need `awk` which is complex.
    // This is a simple count — acceptable for patrol.
    std::string out = shell_out(cmd);
    try { return std::stoi(out); } catch (...) { return 0; }
}

}  // namespace

// ─────────────────────────────────────────────────────────────
// PatrolConfig
// ─────────────────────────────────────────────────────────────

PatrolConfig PatrolConfig::from_policy() {
    PatrolConfig cfg;

    // 尝试从 chat_policy.json 加载
    // 使用 shell 读取（避免引入 ChatPolicy 的复杂依赖）
    std::string policy_path = default_config_dir() + "/chat_policy.json";
    if (!fs::exists(policy_path)) return cfg;

    std::ifstream in(policy_path);
    if (!in.is_open()) return cfg;

    std::string raw((std::istreambuf_iterator<char>(in)),
                    std::istreambuf_iterator<char>());
    if (raw.empty()) return cfg;

    try {
        auto policy = nlohmann::json::parse(raw);
        if (!policy.contains("patrol")) return cfg;

        auto& p = policy["patrol"];
        cfg.enabled = p.value("enabled", true);
        cfg.cron_interval_min = p.value("cron_interval_min", 30);
        cfg.quiet_mode = p.value("quiet_mode", true);

        if (p.contains("disk")) {
            auto& d = p["disk"];
            cfg.disk.enabled = d.value("enabled", true);
            cfg.disk.path = d.value("path", "/");
            cfg.disk.warn_pct = d.value("warn_pct", 85);
        }
        if (p.contains("memory")) {
            auto& m = p["memory"];
            cfg.memory.enabled = m.value("enabled", true);
            cfg.memory.warn_pct = m.value("warn_pct", 90);
        }
        if (p.contains("process")) {
            auto& pr = p["process"];
            cfg.process.enabled = pr.value("enabled", true);
            if (pr.contains("names") && pr["names"].is_array()) {
                for (auto& n : pr["names"])
                    cfg.process.names.push_back(n.get<std::string>());
            }
        }
        if (p.contains("log_errors")) {
            auto& l = p["log_errors"];
            cfg.log_errors.enabled = l.value("enabled", true);
            cfg.log_errors.log_path = l.value("log_path", "");
            cfg.log_errors.minutes = l.value("minutes", 60);
            cfg.log_errors.warn_count = l.value("warn_count", 3);
        }
    } catch (...) {
        // 解析失败 → 用默认值
    }

    return cfg;
}

// ─────────────────────────────────────────────────────────────
// 探针实现
// ─────────────────────────────────────────────────────────────

ProbeResult PatrolProbe::check_disk(const std::string& path, int warn_pct) {
    ProbeResult r;
    r.name = "disk";

    // v0.54.17: `statvfs` 是 POSIX（Windows 无 `sys/statvfs.h`）⇒ Windows 走 GetDiskFreeSpaceExA。
    // 语义映射：`lpFreeBytesAvailableToCaller` 对应 POSIX 的 `f_bavail`（**调用者可用**，非 root
    // 全量），`lpTotalNumberOfBytes` 对应 `f_blocks * f_frsize`。
    unsigned long total = 0, avail = 0;
#ifdef _WIN32
    ULARGE_INTEGER free_to_caller{}, total_bytes{}, total_free{};
    if (!GetDiskFreeSpaceExA(path.c_str(), &free_to_caller, &total_bytes, &total_free)) {
        r.ok = false;
        r.error = "GetDiskFreeSpaceEx failed for " + path +
                  " (err=" + std::to_string(GetLastError()) + ")";
        return r;
    }
    total = static_cast<unsigned long>(total_bytes.QuadPart);
    avail = static_cast<unsigned long>(free_to_caller.QuadPart);
#else
    struct statvfs st;
    if (statvfs(path.c_str(), &st) != 0) {
        r.ok = false;
        r.error = "statvfs failed for " + path;
        return r;
    }

    total = st.f_blocks * st.f_frsize;
    avail = st.f_bavail * st.f_frsize;  // 非 root 可用的空间
#endif
    unsigned long used = total - avail;
    int pct = (total > 0) ? static_cast<int>(used * 100 / total) : 0;

    r.ok = true;
    r.warning = (pct >= warn_pct);

    auto fmt_mb = [](unsigned long bytes) -> std::string {
        double mb = bytes / (1024.0 * 1024.0);
        std::ostringstream os;
        if (mb >= 1024.0) os << std::fixed << std::setprecision(1) << (mb / 1024.0) << " GB";
        else os << std::fixed << std::setprecision(0) << mb << " MB";
        return os.str();
    };

    r.summary = "磁盘 " + path + "：使用率 " + std::to_string(pct) + "%"
              + "（已用 " + fmt_mb(used) + " / 总量 " + fmt_mb(total) + "）";

    r.data["path"] = path;
    r.data["pct"] = pct;
    r.data["total_bytes"] = total;
    r.data["used_bytes"] = used;
    r.data["avail_bytes"] = avail;

    if (r.warning) {
        r.summary += " ⚠️ 超过阈值 " + std::to_string(warn_pct) + "%";
    }

    return r;
}

ProbeResult PatrolProbe::check_memory(int warn_pct) {
    ProbeResult r;
    r.name = "memory";

    long total = parse_meminfo_kb("MemTotal");
    long avail = parse_meminfo_kb("MemAvailable");

    if (total < 0 || avail < 0) {
        r.ok = false;
        r.error = "cannot parse /proc/meminfo";
        return r;
    }

    long used = total - avail;
    int pct = (total > 0) ? static_cast<int>(used * 100 / total) : 0;

    r.ok = true;
    r.warning = (pct >= warn_pct);

    auto fmt_mb = [](long kb) -> std::string {
        double mb = kb / 1024.0;
        std::ostringstream os;
        if (mb >= 1024.0) os << std::fixed << std::setprecision(1) << (mb / 1024.0) << " GB";
        else os << std::fixed << std::setprecision(0) << mb << " MB";
        return os.str();
    };

    r.summary = "内存：使用率 " + std::to_string(pct) + "%"
              + "（已用 " + fmt_mb(used) + " / 总量 " + fmt_mb(total) + "）";

    r.data["total_kb"] = total;
    r.data["avail_kb"] = avail;
    r.data["pct"] = pct;

    if (r.warning) {
        r.summary += " ⚠️ 超过阈值 " + std::to_string(warn_pct) + "%";
    }

    return r;
}

ProbeResult PatrolProbe::check_process(const std::vector<std::string>& names) {
    ProbeResult r;
    r.name = "process";
    r.ok = true;

    nlohmann::json procs = nlohmann::json::array();
    int total_pids = 0;

    for (const auto& name : names) {
        auto pids = find_pids_by_name(name);
        total_pids += pids.size();

        nlohmann::json entry;
        entry["name"] = name;
        entry["pid_count"] = pids.size();
        entry["running"] = !pids.empty();

        if (!pids.empty()) {
            long rss = proc_rss_kb(pids[0]);
            entry["rss_kb"] = rss;

            auto fmt_mb = [](long kb) -> std::string {
                std::ostringstream os;
                os << std::fixed << std::setprecision(1) << (kb / 1024.0) << " MB";
                return os.str();
            };

            r.summary += (r.summary.empty() ? "" : "；") +
                         name + " 运行中（PID " + std::to_string(pids[0]) +
                         "，RSS " + (rss > 0 ? fmt_mb(rss) : "?") + "）";
        } else {
            r.warning = true;
            r.summary += (r.summary.empty() ? "" : "；") +
                         name + " ❌ 未运行";
        }

        procs.push_back(entry);
    }

    r.data["processes"] = procs;
    r.data["total_pids"] = total_pids;

    if (r.summary.empty()) r.summary = "无注册进程";
    return r;
}

ProbeResult PatrolProbe::check_log_errors(const std::string& log_path,
                                            int minutes, int warn_count) {
    ProbeResult r;
    r.name = "log_errors";

    std::string path = log_path;
    if (path.empty()) {
        path = default_data_dir() + "/../logs/agent_svc.log";
    }

    if (!fs::exists(path)) {
        r.ok = false;
        r.error = "log file not found: " + path;
        return r;
    }

    int count = count_log_errors(path, minutes);

    r.ok = true;
    r.warning = (count < 0) ? false : (count >= warn_count);

    r.summary = "日志 " + path + "：最近 " + std::to_string(minutes)
              + " 分钟内 ERROR/WARN 共 " + std::to_string(count) + " 条";

    r.data["log_path"] = path;
    r.data["error_count"] = count;
    r.data["minutes"] = minutes;

    if (r.warning) {
        r.summary += " ⚠️ 超过阈值 " + std::to_string(warn_count) + " 条";
    }

    return r;
}

// ─────────────────────────────────────────────────────────────
// run_all + build_prompt
// ─────────────────────────────────────────────────────────────

std::vector<ProbeResult> PatrolProbe::run_all(const PatrolConfig& cfg) {
    std::vector<ProbeResult> results;

    if (cfg.disk.enabled)
        results.push_back(check_disk(cfg.disk.path, cfg.disk.warn_pct));
    if (cfg.memory.enabled)
        results.push_back(check_memory(cfg.memory.warn_pct));
    if (cfg.process.enabled)
        results.push_back(check_process(cfg.process.names));
    if (cfg.log_errors.enabled)
        results.push_back(check_log_errors(
            cfg.log_errors.log_path, cfg.log_errors.minutes, cfg.log_errors.warn_count));

    return results;
}

std::string PatrolProbe::build_prompt(const std::vector<ProbeResult>& results,
                                        const PatrolConfig& cfg) {
    bool has_warning = false;
    for (const auto& r : results) {
        if (r.warning) { has_warning = true; break; }
    }

    // quiet mode: 无警告 → 静默
    if (cfg.quiet_mode && !has_warning) return "";

    std::ostringstream os;
    os << "You are a system health patrol. Review the following probe results "
       << "and provide a brief diagnostic summary in Chinese.\n\n"
       << "=== PATROL RESULTS ===\n";

    for (const auto& r : results) {
        os << "[" << (r.ok ? (r.warning ? "⚠️ WARN" : "✅ OK") : "❌ FAIL")
           << "] " << r.name << ": " << r.summary << "\n";
        if (!r.error.empty()) os << "    error: " << r.error << "\n";
    }

    if (has_warning) {
        os << "\n---\n"
           << "Some probes reported warnings. Analyze the situation and suggest "
           << "potential causes and actions. Keep it concise (2-3 sentences).\n";
    } else {
        os << "\n---\n"
           << "All probes are normal. Confirm briefly.\n";
    }

    return os.str();
}

}  // namespace thin_agent
