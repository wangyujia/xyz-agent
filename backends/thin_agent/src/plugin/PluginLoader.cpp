#include "thin_agent/plugin/PluginLoader.h"
#include "thin_agent/plugin/PluginContext.h"  // v0.53.0
#include "thin_agent/plugin/PluginInterface.h"
#include "thin_agent/core/SkillRegistry.h"

#include <cstdlib>
#include <iostream>
#include <string>

// v0.54.17: 动态库/目录扫描的**平台隔离**（Windows 无 dlfcn.h/glob.h；插件后缀 .dll 而非 .so）。
// 与 discovery.cpp 同款：差异只在本文件内隔离，不跨文件封装。
#ifdef _WIN32
#include <windows.h>
#include <algorithm>
#include <filesystem>
#define TA_DLOPEN(p)    reinterpret_cast<void*>(LoadLibraryA(p))
#define TA_DLSYM(h, s)  reinterpret_cast<void*>(GetProcAddress(static_cast<HMODULE>(h), s))
#define TA_DLCLOSE(h)   FreeLibrary(static_cast<HMODULE>(h))
#define TA_DLSUFFIX     ".dll"
static std::string ta_dlerror() {
  return "Win32 error " + std::to_string(GetLastError());
}
#else
#include <dlfcn.h>
#include <glob.h>
#define TA_DLOPEN(p)    ::dlopen(p, RTLD_NOW | RTLD_LOCAL)
#define TA_DLSYM(h, s)  ::dlsym(h, s)
#define TA_DLCLOSE(h)   ::dlclose(h)
#define TA_DLSUFFIX     ".so"
static std::string ta_dlerror() { return ::dlerror(); }
#endif
#include "thin_agent/log/LogEvent.h"

namespace thin_agent {
namespace plugin {

/// 展开路径中的 ~ 和 $HOME
static std::string expand_home(const std::string& path) {
  if (path.empty()) return path;
  std::string result = path;
  // ~ 展开
  if (result[0] == '~') {
    const char* home = std::getenv("HOME");
    if (!home) home = std::getenv("USERPROFILE");
    if (home) result.replace(0, 1, home);
  }
  return result;
}

/// glob 扫描目录下所有插件文件（POSIX: `*.so` / Windows: `*.dll`）
static std::vector<std::string> glob_so(const std::string& dir) {
#ifdef _WIN32
  // v0.54.17: Windows 无 `glob.h` ⇒ 用 std::filesystem 扫 `TA_DLSUFFIX`（.dll）。
  // 排序以给出**确定性加载顺序**（Linux 侧原用 GLOB_NOSORT，顺序不保证）。
  std::vector<std::string> result;
  std::error_code ec;
  for (const auto& e : std::filesystem::directory_iterator(dir, ec)) {
    if (e.is_regular_file() && e.path().extension() == TA_DLSUFFIX) {
      result.emplace_back(e.path().string());
    }
  }
  std::sort(result.begin(), result.end());
  return result;
#else
  std::vector<std::string> result;
  std::string pattern = dir + "/*.so";
  glob_t g{};
  int rc = ::glob(pattern.c_str(), GLOB_NOSORT | GLOB_TILDE, nullptr, &g);
  if (rc == 0) {
    for (size_t i = 0; i < g.gl_pathc; ++i) {
      if (g.gl_pathv[i]) result.emplace_back(g.gl_pathv[i]);
    }
  }
  globfree(&g);
  return result;
#endif
}

std::vector<PluginLoadResult> load_plugins(
    SkillRegistry& registry,
    const std::string& root_dir,
    const std::vector<std::string>& modes,
    PluginContext* ctx) {

  std::vector<PluginLoadResult> results;
  std::string root = expand_home(root_dir);

  for (const auto& mode : modes) {
    std::string dir = root + "/" + mode;
    auto so_files = glob_so(dir);

    if (so_files.empty()) {
      log_event("plugin", LogLevel::Warn, "no .so in dir", {{"dir", dir}});
      continue;
    }

    for (const auto& so_path : so_files) {
      PluginLoadResult r;
      r.name = so_path;

      void* handle = TA_DLOPEN(so_path.c_str());
      if (!handle) {
        r.ok = false;
        r.error = std::string("dlopen: ") + ta_dlerror();
        log_event("plugin", LogLevel::Error, "load error", {{"error", r.error}});
        results.push_back(r);
        continue;
      }

      // v0.53.0: v2 符号优先——init2(registry, context)，支持窄服务面注入；
      // 无 init2 则回退 init(registry)（旧插件兼容）。
      using PluginInit2Fn = const char* (*)(SkillRegistry&, PluginContext&);
      auto* init2_fn = reinterpret_cast<PluginInit2Fn>(
          TA_DLSYM(handle, "thin_agent_plugin_init2"));
      if (init2_fn) {
        if (ctx) {
          const char* plugin_name = init2_fn(registry, *ctx);
          if (!plugin_name) {
            r.ok = false;
            r.error = "init2 returned nullptr";
          } else {
            r.ok = true;
            r.name = plugin_name;
            std::cout << "[plugin] loaded: " << plugin_name
                      << " (mode=" << mode << ", v2)" << std::endl;
          }
          results.push_back(r);
          continue;
        }
        log_event("plugin", LogLevel::Warn, "init2 but no context; fallback",
                  {{"so", so_path}});
      }

      auto* init_fn = reinterpret_cast<PluginInitFn>(
          TA_DLSYM(handle, kPluginInitSymbol));
      if (!init_fn) {
        r.ok = false;
        r.error = std::string("dlsym: ") + ta_dlerror();
        log_event("plugin", LogLevel::Error, "load error", {{"error", r.error}});
        TA_DLCLOSE(handle);
        results.push_back(r);
        continue;
      }

      const char* plugin_name = init_fn(registry);
      if (!plugin_name) {
        r.ok = false;
        r.error = "init returned nullptr";
        log_event("plugin", LogLevel::Error, "load error", {{"error", r.error}});
      } else {
        r.ok = true;
        std::cout << "[plugin] loaded: " << plugin_name
                  << " (mode=" << mode << ")" << std::endl;
      }
      results.push_back(r);
    }
  }

  return results;
}

}  // namespace plugin
}  // namespace thin_agent
