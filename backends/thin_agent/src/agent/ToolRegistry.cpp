#include "thin_agent/agent/ToolRegistry.h"
#include "thin_agent/agent/ToolCallContext.h"

#include <algorithm>
#include <chrono>
#include <future>
#include <sstream>
#include <stdexcept>

namespace thin_agent {
namespace agent {

// ── ToolSchema 实现 ──────────────────────────────────────────────

nlohmann::json ToolSchema::to_openai_function() const {
  nlohmann::json func;
  func["name"] = name;
  func["description"] = description;

  nlohmann::json props = nlohmann::json::object();
  nlohmann::json required_arr = nlohmann::json::array();

  for (const auto& p : parameters) {
    nlohmann::json prop;
    prop["type"] = p.type;
    prop["description"] = p.description;
    if (!p.enum_values.is_null()) {
      prop["enum"] = p.enum_values;
    }
    props[p.name] = prop;
    if (p.required) {
      required_arr.push_back(p.name);
    }
  }

  nlohmann::json params;
  params["type"] = "object";
  params["properties"] = props;
  if (!required_arr.empty()) {
    params["required"] = required_arr;
  }

  func["parameters"] = params;
  return func;
}

std::string ToolSchema::to_text_description() const {
  std::ostringstream os;
  os << "- " << name << ": " << description << "\n";
  if (!parameters.empty()) {
    os << "  参数:\n";
    for (const auto& p : parameters) {
      os << "    " << p.name << " (" << p.type
         << (p.required ? ", 必填" : ", 可选")
         << "): " << p.description << "\n";
    }
  }
  return os.str();
}

std::vector<std::string> ToolSchema::validate_params(
    const nlohmann::json& params) const {
  std::vector<std::string> errors;

  for (const auto& p : parameters) {
    if (p.required && (!params.contains(p.name) || params[p.name].is_null())) {
      errors.push_back("missing required parameter: " + p.name);
      continue;
    }
    if (!params.contains(p.name)) continue;

    const auto& val = params[p.name];
    // 简单类型检查
    if (p.type == "string" && !val.is_string()) {
      errors.push_back(p.name + " should be string, got " +
                       std::string(val.type_name()));
    } else if (p.type == "integer" && !val.is_number_integer()) {
      errors.push_back(p.name + " should be integer, got " +
                       std::string(val.type_name()));
    } else if (p.type == "boolean" && !val.is_boolean()) {
      errors.push_back(p.name + " should be boolean, got " +
                       std::string(val.type_name()));
    } else if (p.type == "object" && !val.is_object()) {
      errors.push_back(p.name + " should be object, got " +
                       std::string(val.type_name()));
    } else if (p.type == "array" && !val.is_array()) {
      errors.push_back(p.name + " should be array, got " +
                       std::string(val.type_name()));
    }

    // 枚举值检查
    if (p.enum_values.is_array() && val.is_string()) {
      bool found = false;
      for (const auto& ev : p.enum_values) {
        if (ev.is_string() && ev.get<std::string>() == val.get<std::string>()) {
          found = true;
          break;
        }
      }
      if (!found) {
        errors.push_back(p.name + " has invalid value: " +
                         val.get<std::string>());
      }
    }
  }

  return errors;
}

// ── ToolRegistry 实现 ────────────────────────────────────────────

ToolRegistry& ToolRegistry::instance() {
  static ToolRegistry reg;
  return reg;
}

std::vector<std::string> ToolRegistry::register_tool(ToolSchema tool,
                                                      const std::string& on_conflict,
                                                      const std::string& prefix) {
  std::lock_guard<std::mutex> lk(mu_);
  std::vector<std::string> conflicts;

  auto it = tools_.find(tool.name);
  if (it != tools_.end()) {
    conflicts.push_back(tool.name);
    if (on_conflict == "reject") return conflicts;
    if (on_conflict == "prefix") {
      tool.name = prefix + tool.name;
    }
    // "replace": 直接覆盖
  }

  tools_[tool.name] = std::move(tool);
  return conflicts;
}

const ToolSchema* ToolRegistry::find(const std::string& name) const {
  std::lock_guard<std::mutex> lk(mu_);
  auto it = tools_.find(name);
  if (it != tools_.end()) return &it->second;
  return nullptr;
}

std::vector<ToolSchema> ToolRegistry::all_tools() const {
  std::lock_guard<std::mutex> lk(mu_);
  std::vector<ToolSchema> out;
  out.reserve(tools_.size());
  for (const auto& [_, t] : tools_) out.push_back(t);
  // 按名称排序保证确定性
  std::sort(out.begin(), out.end(),
            [](const ToolSchema& a, const ToolSchema& b) {
              return a.name < b.name;
            });
  return out;
}

nlohmann::json ToolRegistry::to_openai_tools() const {
  nlohmann::json arr = nlohmann::json::array();
  for (const auto& tool : all_tools()) {
    nlohmann::json entry;
    entry["type"] = "function";
    entry["function"] = tool.to_openai_function();
    arr.push_back(entry);
  }
  return arr;
}

std::string ToolRegistry::build_tools_prompt() const {
  auto tools = all_tools();
  if (tools.empty()) return "";

  std::ostringstream os;
  os << "你可以使用以下工具来完成任务。需要调用工具时，请严格按以下 JSON 格式回复：\n\n"
     << "```json\n{\"tool_calls\": [{\"name\": \"<工具名>\", \"params\": {...}}]}\n```\n\n"
     << "如果需要直接回复用户（不再调用工具），请使用：\n\n"
     << "```json\n{\"tool_calls\": [], \"final_answer\": \"<回复内容>\"}\n```\n\n"
     << "可用工具:\n\n";
  for (const auto& t : tools) {
    os << t.to_text_description() << "\n";
  }
  return os.str();
}

ToolCallResult ToolRegistry::call(const std::string& name,
                                   const nlohmann::json& params,
                                   int timeout_ms) {
  ToolCallResult result;
  result.tool_name = name;

  const ToolSchema* schema = find(name);
  if (!schema) {
    result.error = "tool not found: " + name;
    return result;
  }

  // 参数校验
  auto errors = schema->validate_params(params);
  if (!errors.empty()) {
    std::ostringstream oss;
    for (size_t i = 0; i < errors.size(); ++i) {
      if (i) oss << "; ";
      oss << errors[i];
    }
    result.error = oss.str();
    return result;
  }

  // 执行（带超时保护）
  if (!schema->execute) {
    result.error = "tool has no execute callback: " + name;
    return result;
  }

  if (timeout_ms > 0) {
    // v0.52.11: 超时语义修复——std::async 的 future 析构会阻塞 join
    // 工具线程，wait_for 超时后的 return 路径照样卡死（真 e2e 实测：
    // 波次反复 60s 超时→future 析构堆积 join→线程 72→269 雪崩→
    // 服务冻结）。改 detached 线程+promise：超时放弃等待立即返回，
    // 工具线程自然退出后 promise 析构（未设置值时丢弃）——零泄漏。
    // v0.52.11c: promise 共享堆上（detached 线程可能晚于调用方栈帧
    // 存活——栈上 promise 被超时方析构后 set_value 即崩，实测
    // std::future_error: No associated state）。
    auto prom = std::make_shared<std::promise<nlohmann::json>>();
    auto future = prom->get_future();
    // v0.52.23: 协作式取消——调用方超时放弃后置位，长跑工具在
    // 循环点检查 current_tool_ctx() 提前退出（方案 A）。
    auto ctx = std::make_shared<agent::ToolCallContext>();
    // v0.52.11b: 生命周期加固——detached 线程拷贝 execute 回调（值捕获）
    // 而非持有 schema 裸指针：注册表 clear/覆盖注册可能销毁 schema
    //（agent_core 单测实测 use-after-free SIGSEGV）。回调本身是
    // std::function 拷贝（持有的捕获状态与注册表共存亡）。
    auto execute_fn = schema->execute;  // std::function 拷贝
    std::thread([prom, execute_fn, params, ctx]() {
      nlohmann::json out;
      agent::g_current_tool_ctx = ctx.get();
      try {
        out = execute_fn(params);
      } catch (...) {
        out = {{"ok", false}, {"error", "tool execution exception"}};
      }
      // 只 set 一次（future 已超时放弃时 set_value 抛
      // promise_already_satisfied 之外的 future_error——共享状态
      // 在双方都释放后由最后一个释放者销毁，安全）
      try {
        prom->set_value(std::move(out));
      } catch (...) { /* 已超时销毁或已满足——丢弃 */ }
    }).detach();
    auto status = future.wait_for(std::chrono::milliseconds(timeout_ms));
    if (status == std::future_status::timeout) {
      ctx->cancel_now();  // v0.52.23: 通知协作工具提前退出
      result.error = "tool execution timeout: " + name;
      return result;
    }
    try {
      auto exec_result = future.get();
      result.ok = exec_result.value("ok", false);
      result.result = exec_result.value("result", nlohmann::json::object());
      result.error = exec_result.value("error", "");
    } catch (const std::exception& e) {
      result.error = std::string("tool execution exception: ") + e.what();
    }
  } else {
    try {
      auto exec_result = schema->execute(params);
      result.ok = exec_result.value("ok", false);
      result.result = exec_result.value("result", nlohmann::json::object());
      result.error = exec_result.value("error", "");
    } catch (const std::exception& e) {
      result.error = std::string("tool execution exception: ") + e.what();
    }
  }

  return result;
}

size_t ToolRegistry::size() const {
  std::lock_guard<std::mutex> lk(mu_);
  return tools_.size();
}

void ToolRegistry::clear() {
  std::lock_guard<std::mutex> lk(mu_);
  tools_.clear();
}

}  // namespace agent
}  // namespace thin_agent
