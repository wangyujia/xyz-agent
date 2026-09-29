#pragma once

#include <memory>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace thin_agent {

/// 一条持久记忆（对标 Hermes MEMORY.md / USER.md 的 § 条目）。
struct Fact {
    int64_t id = 0;
    std::string path;          // system/prefs, user/style, role/developer/xxx, ref/xxx
    std::string content;       // ≤1000 chars
    std::string created_at;
    std::string updated_at;
};

/// 持久记忆存储：SQLite（主）+ MD 文件（备份）+ 系统 prompt 冷冻快照。
///
/// 两段式架构：
///   - 热路径（hot_snapshot）：system/* + user/* + role/<current_role>/*
///     会话开始时构建一次，全程不变（prefix-cache 友好）。
///   - 冷路径（search）：FTS5 全文搜索，LLM 通过 memory_find 工具按需检索。
///
/// MD 备份：export_to_md/import_from_md 提供人类可读备份与跨机器迁移能力。
class FactStore {
public:
    FactStore();
    ~FactStore();

    // ── 生命周期 ────────────────────────────────────────────
    /// 打开数据库（不存在则创建），自动建表 + FTS5 + 触发器。
    /// 如果表为空且存在默认 MD 文件 → 自动导入。
    bool open(const std::string& db_path);

    // ── CRUD ────────────────────────────────────────────────
    /// 保存一条记忆。path 已存在 → UPDATE，否则 → INSERT。
    /// 返回新插入的 rowid，失败返回 -1。
    int64_t save(const std::string& path, const std::string& content);

    /// 按 id 删除。返回是否成功。
    bool forget(int64_t id);

    // ── 热路径：构建冷冻快照 ───────────────────────────────
    /// 查询 system/* + user/* + role/<role_name>/* ，按 updated_at DESC 排序，
    /// 拼成紧凑文本，控制在 char_limit 字符以内。
    /// @param role_name 当前 Agent 角色名（空字符串 = 不注入 role/ 条目）。
    /// @param char_limit 快照字符上限（默认 4000）。
    std::string hot_snapshot(const std::string& role_name = "",
                             int char_limit = 4000);

    // ── 冷路径：FTS5 搜索 ──────────────────────────────────
    /// 全文搜索记忆内容。query 为空时返回全部（prefix 过滤 path）。
    std::vector<Fact> find(const std::string& query = "",
                           const std::string& prefix = "",
                           int limit = 20);

    // ── MD 备份 ─────────────────────────────────────────────
    /// 导出到 dir/MEMORY.md 和 dir/USER.md（Hermes 兼容 § 分隔格式）。
    bool export_to_md(const std::string& dir);

    /// 从 dir/MEMORY.md 和 dir/USER.md 导入（不清除现有数据）。
    bool import_from_md(const std::string& dir);

    // ── 安全钩子（留空实现，以后加固）──────────────────────
    using WriteValidator = std::function<bool(const std::string& content, std::string& reason)>;
    void set_write_validator(WriteValidator v) { write_validator_ = std::move(v); }

    // ── 统计 ────────────────────────────────────────────────
    nlohmann::json stats() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;

    WriteValidator write_validator_;
};

}  // namespace thin_agent
