/// test_discord_unit.cpp — DiscordAdapter 单元测试
///
/// 通过 PlatformAdapter 公共接口测试 Discord 适配器核心逻辑：
///   工厂/标识、启用条件、连接状态、回调、生命周期安全

#include <cassert>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>

#include "thin_agent/api/PlatformAdapter.h"

// 工厂函数声明（定义在 src/gateway/DiscordAdapter.cpp）
extern "C" thin_agent::PlatformAdapter* create_discord_adapter();

// ── helpers ──

static void setenv_safe(const char* key, const char* val) {
#ifdef _WIN32
  _putenv_s(key, val ? val : "");
#else
  if (val) ::setenv(key, val, 1); else ::unsetenv(key);
#endif
}

static std::string getenv_safe(const char* key) {
  const char* v = std::getenv(key);
  return v ? std::string(v) : "";
}

// 保存/恢复环境变量
struct EnvGuard {
  std::string key, saved;
  EnvGuard(const char* k) : key(k), saved(getenv_safe(k)) {}
  ~EnvGuard() { setenv_safe(key.c_str(), saved.empty() ? nullptr : saved.c_str()); }
};

// ── 测试 ──

static void test_factory_creates_valid_pointer() {
  auto adapter = std::unique_ptr<thin_agent::PlatformAdapter>(create_discord_adapter());
  assert(adapter != nullptr);
  std::cout << "PASS: test_factory_creates_valid_pointer\n";
}

static void test_name_is_discord() {
  auto adapter = std::unique_ptr<thin_agent::PlatformAdapter>(create_discord_adapter());
  assert(adapter->name() == "discord");
  std::cout << "PASS: test_name_is_discord\n";
}

static void test_not_enabled_without_token() {
  EnvGuard g("DISCORD_BOT_TOKEN");
  ::unsetenv("DISCORD_BOT_TOKEN");
  auto adapter = std::unique_ptr<thin_agent::PlatformAdapter>(create_discord_adapter());
  assert(!adapter->is_enabled());
  std::cout << "PASS: test_not_enabled_without_token\n";
}

static void test_enabled_with_token() {
  EnvGuard g("DISCORD_BOT_TOKEN");
  ::setenv("DISCORD_BOT_TOKEN", "fake-token-12345", 1);
  auto adapter = std::unique_ptr<thin_agent::PlatformAdapter>(create_discord_adapter());
  assert(adapter->is_enabled());
  std::cout << "PASS: test_enabled_with_token\n";
}

static void test_not_enabled_with_empty_token() {
  EnvGuard g("DISCORD_BOT_TOKEN");
  ::setenv("DISCORD_BOT_TOKEN", "", 1);
  auto adapter = std::unique_ptr<thin_agent::PlatformAdapter>(create_discord_adapter());
  assert(!adapter->is_enabled());
  std::cout << "PASS: test_not_enabled_with_empty_token\n";
}

static void test_not_connected_initially() {
  auto adapter = std::unique_ptr<thin_agent::PlatformAdapter>(create_discord_adapter());
  assert(!adapter->is_connected());
  std::cout << "PASS: test_not_connected_initially\n";
}

static void test_connect_without_token_safe() {
  EnvGuard g("DISCORD_BOT_TOKEN");
  ::unsetenv("DISCORD_BOT_TOKEN");
  auto adapter = std::unique_ptr<thin_agent::PlatformAdapter>(create_discord_adapter());
  // 不应崩溃 — connect 应检测并安全返回
  adapter->connect(nullptr);
  assert(!adapter->is_connected());
  std::cout << "PASS: test_connect_without_token_safe\n";
}

static void test_connect_null_mgr_safe() {
  EnvGuard g("DISCORD_BOT_TOKEN");
  ::setenv("DISCORD_BOT_TOKEN", "fake-token", 1);
  auto adapter = std::unique_ptr<thin_agent::PlatformAdapter>(create_discord_adapter());
  // 有 token 但 mgr 为 null — 不应崩溃
  adapter->connect(nullptr);
  assert(!adapter->is_connected());
  std::cout << "PASS: test_connect_null_mgr_safe\n";
}

static void test_disconnect_without_connect_safe() {
  auto adapter = std::unique_ptr<thin_agent::PlatformAdapter>(create_discord_adapter());
  // 不应崩溃
  adapter->disconnect();
  assert(!adapter->is_connected());
  std::cout << "PASS: test_disconnect_without_connect_safe\n";
}

static void test_message_callback_registered() {
  auto adapter = std::unique_ptr<thin_agent::PlatformAdapter>(create_discord_adapter());
  bool called = false;
  adapter->set_message_callback(
      [&](const std::string&, const std::string&, const std::string&,
          const std::string&, thin_agent::PlatformAdapter*) {
        called = true;
      });
  // set 完成后回调不应被立即触发
  assert(!called);
  std::cout << "PASS: test_message_callback_registered\n";
}

static void test_multiple_instances_independent() {
  auto a1 = std::unique_ptr<thin_agent::PlatformAdapter>(create_discord_adapter());
  auto a2 = std::unique_ptr<thin_agent::PlatformAdapter>(create_discord_adapter());

  assert(a1 != nullptr);
  assert(a2 != nullptr);
  assert(a1.get() != a2.get());
  assert(a1->name() == "discord");
  assert(a2->name() == "discord");
  assert(!a1->is_connected());
  assert(!a2->is_connected());

  std::cout << "PASS: test_multiple_instances_independent\n";
}

static void test_on_timer_safe() {
  auto adapter = std::unique_ptr<thin_agent::PlatformAdapter>(create_discord_adapter());
  // on_timer 不应崩溃（即使未连接，mgr_ 为 null）
  adapter->on_timer();
  std::cout << "PASS: test_on_timer_safe\n";
}

// ── main ──

int main() {
  std::cout << "=== DiscordAdapter Tests ===\n";
  test_factory_creates_valid_pointer();
  test_name_is_discord();
  test_not_enabled_without_token();
  test_enabled_with_token();
  test_not_enabled_with_empty_token();
  test_not_connected_initially();
  test_connect_without_token_safe();
  test_connect_null_mgr_safe();
  test_disconnect_without_connect_safe();
  test_message_callback_registered();
  test_multiple_instances_independent();
  test_on_timer_safe();
  std::cout << "\nALL TESTS PASSED\n";
  return 0;
}
