#pragma once

#include <string>

namespace thin_agent {

/// 文件语法检查（轻量，按扩展名选择策略）
///
/// 纯 C++ 检查（零外部依赖）：
///   .json          → nlohmann::json::parse()
///
/// 通过 popen 调用外部工具（该语言未安装则静默跳过）：
///   .yaml / .yml   → python3 -c "import yaml; yaml.safe_load(open(...))"
///   .py            → python3 -c "compile(open(...).read(), ..., 'exec')"
///   .js            → node -c
///   .cpp / .h / .c → g++ -fsyntax-only（仅当 g++ 在 PATH 中）
///   其他扩展名      → 跳过，返回空字符串
///
/// @param path  文件路径
/// @param content 可选的文件内容（跳过文件读取，用于内存中的内容检查）
/// @return 空字符串 = 语法正确；非空 = "⚠️ Syntax error:\n<详情>"
std::string syntax_check(const std::string& path,
                         const std::string& content = "");

}  // namespace thin_agent
