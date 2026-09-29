#pragma once

#include <functional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace thin_agent {

/// 探针结果：一次健康检查的输出。
struct ProbeResult {
    std::string name;          // "disk", "memory", "process", "log_errors"
    bool warning = false;      // 是否触发告警阈值
    bool ok = true;            // 探针自身是否执行成功
    std::string summary;       // 人类可读单行摘要
    nlohmann::json data;       // 结构化数据
    std::string error;         // 探针执行失败时的错误信息
};

/// 巡检配置。
struct PatrolConfig {
    bool enabled = true;
    int cron_interval_min = 30;        // cron 触发间隔（分钟）
    bool quiet_mode = true;            // true: 仅 warning 时推送；false: 每次都推送

    struct DiskProbe {
        bool enabled = true;
        std::string path = "/";
        int warn_pct = 85;
    } disk;

    struct MemoryProbe {
        bool enabled = true;
        int warn_pct = 90;
    } memory;

    struct ProcessProbe {
        bool enabled = true;
        std::vector<std::string> names = {"thin_agent"};
    } process;

    struct LogErrorsProbe {
        bool enabled = true;
        std::string log_path = "";      // 空 → 自动取 ~/.thin_agent/logs/agent_svc.log
        int minutes = 60;
        int warn_count = 3;             // 最近 N 分钟内 ERROR 数量超过此值 → warning
    } log_errors;

    /// 从 chat_policy.json 的 "patrol" 段落加载。
    static PatrolConfig from_policy();
};

/// 系统健康巡检探针。
///
/// 4 个探针：
///   - check_disk(path, warn_pct): statvfs 磁盘使用率
///   - check_memory(warn_pct): /proc/meminfo 内存使用率
///   - check_process(names): /proc/<pid> 进程存活 + RSS/CPU
///   - check_log_errors(path, minutes, warn_count): grep ERROR 计数
///
/// 用法：
///   auto results = PatrolProbe::run_all(cfg);
///   for (auto& r : results) { if (r.warning) { ... } }
class PatrolProbe {
public:
    /// 运行所有启用的探针，返回结果列表。
    static std::vector<ProbeResult> run_all(const PatrolConfig& cfg);

    /// 构建 LLM prompt：将探测结果转为自然语言描述。
    /// 如果 quiet_mode=true 且所有探针正常，返回空字符串（无需推送）。
    static std::string build_prompt(const std::vector<ProbeResult>& results,
                                     const PatrolConfig& cfg);

    // ── 单个探针（也可单独调用）──────────────────────────

    static ProbeResult check_disk(const std::string& path, int warn_pct);
    static ProbeResult check_memory(int warn_pct);
    static ProbeResult check_process(const std::vector<std::string>& names);
    static ProbeResult check_log_errors(const std::string& log_path,
                                         int minutes, int warn_count);
};

}  // namespace thin_agent
