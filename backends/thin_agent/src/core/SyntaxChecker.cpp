#include "thin_agent/core/SyntaxChecker.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>

// nlohmann/json is already a core dependency
#include <nlohmann/json.hpp>

namespace thin_agent {

namespace {

/// 获取文件扩展名（小写，含点号），如 ".py"
std::string ext_lower(const std::string& path) {
    auto dot = path.find_last_of('.');
    if (dot == std::string::npos) return "";
    std::string ext = path.substr(dot);
    for (auto& c : ext) c = static_cast<char>(::tolower(static_cast<unsigned char>(c)));
    return ext;
}

/// 通过 popen 执行命令，返回 {exit_code, stdout+stderr}
struct CmdResult {
    int exit_code = -1;
    std::string output;
};

CmdResult popen_cmd(const std::string& cmd) {
    CmdResult r;
    // v0.53.38: 语法检查命令加超时——g++ -fsyntax-only 对大文件可跑 10s+,
    // 无看门狗会卡死 FC 循环线程(之前无超时)。Windows 分支保持原样
    // (timeout 命令不存在;检查命令均为短命令)。
#ifdef _WIN32
    std::string full_cmd = cmd + " 2>&1";
    // TODO(v0.53.x Windows 真机): Windows 分支无超时(GNU timeout 不可用)
    /// ——改走 SandboxExecutor Job Object 或 WaitOrTimer 模式
#else
    std::string full_cmd = "timeout 25 " + cmd + " 2>&1";
#endif
    FILE* pipe = ::popen(full_cmd.c_str(), "r");
    if (!pipe) return r;

    char buf[4096];
    while (::fgets(buf, sizeof(buf), pipe) != nullptr) {
        r.output += buf;
    }
    int status = ::pclose(pipe);
#ifdef _WIN32
    r.exit_code = status;
#else
    r.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
#endif
    return r;
}

/// shell 转义路径（简单实现：用单引号包裹，替换内部单引号）
std::string sh_escape(const std::string& s) {
    std::string r = "'";
    for (char c : s) {
        if (c == '\'') r += "'\\''";
        else r += c;
    }
    r += '\'';
    return r;
}

/// 读取文件内容
std::string read_content(const std::string& path) {
    std::ifstream in(path);
    if (!in.is_open()) return "";
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

}  // namespace

std::string syntax_check(const std::string& path, const std::string& content_hint) {
    auto ext = ext_lower(path);
    if (ext.empty()) return "";

    auto read = [&]() -> std::string {
        return content_hint.empty() ? read_content(path) : content_hint;
    };

    // ── JSON: 纯 C++ ──
    if (ext == ".json") {
        try {
            auto data = read();
            if (data.empty()) return "";
            auto parsed = nlohmann::json::parse(data);  // v0.53.16: 仅为校验抛异常与否
            (void)parsed.size();               // 消费返回值（-Wunused-result）
            return "";
        } catch (const std::exception& e) {
            return std::string("⚠️ JSON syntax error:\n") + e.what();
        }
    }

    // ── YAML: popen python3 yaml.safe_load ──
    if (ext == ".yaml" || ext == ".yml") {
        auto r = popen_cmd("python3 -c " + sh_escape(
            "import yaml; yaml.safe_load(open(" + sh_escape(path) + "))"));
        if (r.exit_code == 0) return "";
        return r.output.empty() ? "" : "⚠️ YAML syntax error:\n" + r.output;
    }

    // ── Python: popen python3 -c compile() ──
    if (ext == ".py") {
        auto r = popen_cmd("python3 -c " + sh_escape(
            "compile(open(" + sh_escape(path) + ").read(), " +
            sh_escape(path) + ", 'exec')"));
        if (r.exit_code == 0 && r.output.find("SyntaxError") == std::string::npos) {
            return "";
        }
        return r.output.empty() ? "" : "⚠️ Python syntax error:\n" + r.output;
    }

    // ── JavaScript: popen node -c ──
    if (ext == ".js") {
        auto r = popen_cmd("node -c " + sh_escape(path));
        if (r.exit_code == 0) return "";
        return r.output.empty() ? "" : "⚠️ JavaScript syntax error:\n" + r.output;
    }

    // ── C/C++: popen g++ -fsyntax-only（仅当 g++ 可用） ──
    if (ext == ".cpp" || ext == ".c" || ext == ".h" || ext == ".hpp" || ext == ".cc") {
        // 快速检查 g++ 是否可用
        auto which = popen_cmd("which g++");
        if (which.exit_code != 0) return "";  // 没装 g++，静默跳过

        auto r = popen_cmd("g++ -fsyntax-only -std=c++17 " + sh_escape(path));
        if (r.exit_code == 0) return "";
        // 只返回第一行错误（避免模板展开噪音）
        auto nl = r.output.find('\n');
        auto first_line = (nl != std::string::npos) ? r.output.substr(0, nl) : r.output;
        return "⚠️ C++ syntax error:\n" + first_line;
    }

    return "";  // 未知类型，跳过
}

}  // namespace thin_agent
