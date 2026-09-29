// unit_patrol_probe：PatrolProbe 系统巡检探针测试

#include <iostream>
#include <string>

#include "thin_agent/core/PatrolProbe.h"

using thin_agent::PatrolConfig;
using thin_agent::PatrolProbe;

#define ASSERT(cond, msg) do { \
    if (!(cond)) { std::cerr << "FAIL: " << msg << std::endl; return 1; } \
} while (0)

int test_check_disk() {
    auto r = PatrolProbe::check_disk("/", 85);
    ASSERT(r.ok, "disk ok");
    ASSERT(r.name == "disk", "disk name");
    ASSERT(r.data.contains("pct"), "disk pct");
    int pct = r.data["pct"].get<int>();
    ASSERT(pct >= 0 && pct <= 100, "disk pct range");
    ASSERT(!r.summary.empty(), "disk summary");
    return 0;
}

int test_check_memory() {
    auto r = PatrolProbe::check_memory(90);
    ASSERT(r.ok, "memory ok");
    ASSERT(r.name == "memory", "memory name");
    ASSERT(r.data.contains("pct"), "memory pct");
    int pct = r.data["pct"].get<int>();
    ASSERT(pct >= 0 && pct <= 100, "memory pct range");
    return 0;
}

int test_check_process() {
    // 搜索 bash（WSL 下一定有）
    auto r = PatrolProbe::check_process({"bash"});
    ASSERT(r.ok, "process ok");
    ASSERT(r.name == "process", "process name");
    ASSERT(r.data.contains("processes"), "process data");
    return 0;
}

int test_check_log_errors() {
    // 用不存在的日志 → 应返回 !ok
    auto r = PatrolProbe::check_log_errors("/nonexistent/path.log", 60, 3);
    ASSERT(!r.ok, "log missing → !ok");
    ASSERT(!r.error.empty(), "log error message");
    return 0;
}

int test_run_all() {
    PatrolConfig cfg;
    cfg.disk.enabled = true;
    cfg.disk.warn_pct = 99;  // 高阈值 → 不太可能 warning
    cfg.memory.enabled = true;
    cfg.memory.warn_pct = 99;
    cfg.process.enabled = true;
    cfg.process.names = {"bash"};
    cfg.log_errors.enabled = false;  // 跳过日志检查（无真实日志文件）

    auto results = PatrolProbe::run_all(cfg);
    ASSERT(results.size() >= 3, "run_all count");
    ASSERT(results[0].ok, "first probe ok");
    return 0;
}

int test_build_prompt() {
    PatrolConfig cfg;
    cfg.quiet_mode = false;  // 强制输出

    std::vector<thin_agent::ProbeResult> results;
    thin_agent::ProbeResult r;
    r.name = "disk";
    r.ok = true;
    r.summary = "磁盘 /：使用率 45%";
    results.push_back(r);

    r.name = "process";
    r.ok = true;
    r.warning = true;
    r.summary = "thin_agent ❌ 未运行";
    results.push_back(r);

    auto prompt = PatrolProbe::build_prompt(results, cfg);
    ASSERT(!prompt.empty(), "build_prompt not empty");
    ASSERT(prompt.find("thin_agent") != std::string::npos, "prompt mentions thin_agent");
    ASSERT(prompt.find("⚠️") != std::string::npos, "prompt has warning marker");
    return 0;
}

int test_quiet_mode() {
    PatrolConfig cfg;
    cfg.quiet_mode = true;

    std::vector<thin_agent::ProbeResult> results;
    thin_agent::ProbeResult r;
    r.name = "disk";
    r.ok = true;
    r.warning = false;
    r.summary = "all good";
    results.push_back(r);

    auto prompt = PatrolProbe::build_prompt(results, cfg);
    ASSERT(prompt.empty(), "quiet mode → empty prompt");
    return 0;
}

int test_config_defaults() {
    PatrolConfig cfg;
    ASSERT(cfg.enabled, "default enabled");
    ASSERT(cfg.cron_interval_min == 30, "default interval");
    ASSERT(cfg.quiet_mode, "default quiet");
    ASSERT(cfg.disk.warn_pct == 85, "default disk warn");
    ASSERT(cfg.memory.warn_pct == 90, "default memory warn");
    return 0;
}

int main() {
    int failures = 0;
    auto run = [&](const char* name, int (*fn)()) {
        int rc = fn();
        if (rc == 0) std::cout << "  PASS: " << name << std::endl;
        else { std::cout << "  FAIL: " << name << std::endl; failures++; }
    };

    run("check_disk", test_check_disk);
    run("check_memory", test_check_memory);
    run("check_process", test_check_process);
    run("check_log_errors", test_check_log_errors);
    run("run_all", test_run_all);
    run("build_prompt", test_build_prompt);
    run("quiet_mode", test_quiet_mode);
    run("config_defaults", test_config_defaults);

    if (failures == 0) {
        std::cout << "unit:test_patrol_probe PASS" << std::endl;
        return 0;
    }
    std::cerr << "unit:test_patrol_probe FAIL (" << failures << " failures)" << std::endl;
    return 1;
}
