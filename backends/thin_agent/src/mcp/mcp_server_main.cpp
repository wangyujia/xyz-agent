/// thin_agent MCP Server — stdio-based JSON-RPC server exposing thin_agent tools.
///
/// Usage:
///   thin_agent_mcp_server [--config <path>]
///
/// Reads JSON-RPC 2.0 requests from stdin, writes responses to stdout.
/// Compatible with Claude Desktop, Cursor, and other MCP clients.
///
/// Environment:
///   THIN_AGENT_CONFIG_PATH — override config file path
///   THIN_AGENT_HOME         — override home directory (default: ~/.thin_agent)

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

#include "thin_agent/Version.h"
#include "thin_agent/RuntimePaths.h"
#include "thin_agent/core/SkillRegistry.h"
#include "thin_agent/core/CommandValidator.h"
#include "thin_agent/agent/McpServer.h"

int main(int argc, char* argv[]) {
  // ── 解析命令行参数 ──
  std::string config_path;
  for (int i = 1; i < argc; ++i) {
    std::string arg(argv[i]);
    if ((arg == "--config" || arg == "-c") && i + 1 < argc) {
      config_path = argv[++i];
    } else if (arg == "--version" || arg == "-v") {
      std::cout << "thin_agent MCP Server " << thin_agent::kThinAgentVersion << std::endl;
      return 0;
    } else if (arg == "--help" || arg == "-h") {
      std::cout << "thin_agent MCP Server " << thin_agent::kThinAgentVersion << "\n\n"
                << "Usage: thin_agent_mcp_server [options]\n\n"
                << "Options:\n"
                << "  -c, --config <path>   Config file path (default: ~/.thin_agent/chat_policy.json)\n"
                << "  -v, --version         Print version\n"
                << "  -h, --help            Show this help\n\n"
                << "Environment:\n"
                << "  THIN_AGENT_CONFIG_PATH  Config file path override\n"
                << "  THIN_AGENT_HOME         Home directory (default: ~/.thin_agent)\n";
      return 0;
    }
  }

  // ── 加载配置 ──
  if (config_path.empty()) {
    const char* env = std::getenv("THIN_AGENT_CONFIG_PATH");
    if (env && env[0]) config_path = env;
  }
  if (config_path.empty()) {
    config_path = thin_agent::default_chat_policy_path();
  }

  // 加载 chat_policy.json
  std::string policy_content;
  {
    std::ifstream ifs(config_path);
    if (!ifs.is_open()) {
      std::cerr << "thin_agent_mcp_server: cannot open config: " << config_path << std::endl;
      return 1;
    }
    std::ostringstream oss;
    oss << ifs.rdbuf();
    policy_content = oss.str();
  }

  nlohmann::json policy_json;
  try {
    policy_json = nlohmann::json::parse(policy_content);
  } catch (const std::exception& e) {
    std::cerr << "thin_agent_mcp_server: JSON parse error in " << config_path
              << ": " << e.what() << std::endl;
    return 1;
  }

  // ── 初始化 SkillRegistry ──
  thin_agent::SkillRegistry skill_registry;

  // 从 skills 段加载
  if (policy_json.contains("skills")) {
    nlohmann::json whitelist_json = policy_json.value("whitelist", nlohmann::json::object());
    skill_registry.load_from_policy(policy_json["skills"], whitelist_json);
  }

  // ── DemoConfigCompat（用于初始化需要 config 的组件，但不连接云）──
  // MCP Server 只做工具暴露，不需要云连接相关配置
  (void)policy_json;  // 策略已由 SkillRegistry 消费完毕

  // ── 注册 C++ handler（shell_exec 等需要 popen 的内置 handler）──
  // 这里只注册最常用的几个
  skill_registry.register_cpp_handler("shell_exec",
    [](const nlohmann::json& params) -> nlohmann::json {
      std::string command = params.value("command", params.value("cmd", ""));
      if (command.empty()) return {{"success", false}, {"error", "command_required"}};

      // v0.53.38: 复用主服务 CommandValidator——此前硬编码两条模式,
      // "rm -rf /home"/dd 烧盘/重定向裸设备等全放行
      {
        std::string why;
        if (!thin_agent::CommandValidator::is_safe(command, &why)) {
          return {{"success", false}, {"error", "dangerous command blocked: " + why}};
        }
      }

      FILE* pipe = popen(command.c_str(), "r");
      if (!pipe) return {{"success", false}, {"error", "popen_failed"}};
      std::string output;
      char buf[4096];
      while (fgets(buf, sizeof(buf), pipe)) output += buf;
      int rc = pclose(pipe);
      return {{"success", rc == 0}, {"output", output}, {"exit_code", rc}};
    });

  // ── 启动 MCP Server ──
  thin_agent::agent::McpServer server(skill_registry);
  server.set_server_info("thin_agent", std::string(thin_agent::kThinAgentVersion));

  // stdio 模式：从 stdin 读请求，写到 stdout
  server.run_stdio();

  return 0;
}
