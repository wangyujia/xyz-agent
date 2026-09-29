// zing_agent: ConfigStore 实现（单 JSON 文件写穿）
#include "ConfigStore.h"

#include <nlohmann/json.hpp>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sys/stat.h>
#ifdef _WIN32
#include <shlobj.h>
#include <direct.h>
#else
#include <unistd.h>
#endif

namespace zing {

static void ensure_dir(const std::string& dir) {
#ifdef _WIN32
  _mkdir(dir.c_str());
#else
  ::mkdir(dir.c_str(), 0755);
#endif
}

std::string ConfigStore::default_path() {
  if (const char* p = ::getenv("ZING_STATE_PATH")) return p;
  std::string base;
#ifdef _WIN32
  char buf[MAX_PATH] = {0};
  if (SUCCEEDED(SHGetFolderPathA(nullptr, CSIDL_APPDATA, nullptr, 0, buf)))
    base = std::string(buf) + "\\zing_agent";
  else
    base = "zing_agent";
#else
  const char* home = ::getenv("HOME");
  base = (home ? std::string(home) : ".") + "/.config/zing_agent";
#endif
  return base + "/state.json";
}

ConfigStore::ConfigStore(std::string path) : path_(path.empty() ? default_path() : std::move(path)) {
  // 确保父目录存在（首次运行）
  const auto pos = path_.find_last_of("/\\");
  if (pos != std::string::npos) ensure_dir(path_.substr(0, pos));
}

bool ConfigStore::load() {
  std::lock_guard<std::mutex> lk(mu_);
  std::ifstream f(path_);
  if (!f.good()) return false;
  try {
    nlohmann::json j;
    f >> j;
    kv_.clear();
    if (j.is_object())
      for (auto it = j.begin(); it != j.end(); ++it)
        kv_[it.key()] = it.value().is_string() ? it.value().get<std::string>()
                                               : it.value().dump();
  } catch (...) {
    kv_.clear();  // 损坏=清空重开（用户数据只有展示层，可接受）
    return false;
  }
  return true;
}

std::string ConfigStore::get(const std::string& key, const std::string& def) const {
  std::lock_guard<std::mutex> lk(mu_);
  auto it = kv_.find(key);
  return it == kv_.end() ? def : it->second;
}

bool ConfigStore::has(const std::string& key) const {
  std::lock_guard<std::mutex> lk(mu_);
  return kv_.count(key) > 0;
}

void ConfigStore::set(const std::string& key, const std::string& value) {
  std::lock_guard<std::mutex> lk(mu_);
  kv_[key] = value;
  save_locked();
}

void ConfigStore::del(const std::string& key) {
  std::lock_guard<std::mutex> lk(mu_);
  kv_.erase(key);
  save_locked();
}

std::string ConfigStore::all_json() const {
  std::lock_guard<std::mutex> lk(mu_);
  nlohmann::json j = nlohmann::json::object();
  for (const auto& [k, v] : kv_) j[k] = v;
  return j.dump();
}

void ConfigStore::save_locked() {
  // v8: 原子写——tmp 写全量+flush 后 rename 替换。此前 truncate 直写,
  // 进程在 truncate 后崩溃=state.json 清零（用户数据全丢）
  nlohmann::json j = nlohmann::json::object();
  for (const auto& [k, v] : kv_) j[k] = v;
  const std::string tmp = path_ + ".tmp";
  {
    std::ofstream f(tmp, std::ios::trunc);
    f << j.dump();
    f.flush();
    if (!f.good()) {
      fprintf(stderr, "[zing] ConfigStore 写失败: %s (errno=%d)\n", tmp.c_str(), errno);
      ::remove(tmp.c_str());
      return;
    }
    // v17: 显式 close 并检查——析构 close 失败会被静默丢弃,rename 不完整 tmp→数据损坏
    f.close();
    if (!f) {
      fprintf(stderr, "[zing] ConfigStore close 失败: %s (errno=%d)\n", tmp.c_str(), errno);
      ::remove(tmp.c_str());
      return;
    }
  }
  if (::rename(tmp.c_str(), path_.c_str()) != 0)
    fprintf(stderr, "[zing] ConfigStore rename 失败: %s -> %s (errno=%d)\n",
            tmp.c_str(), path_.c_str(), errno);
}

}  // namespace zing
