// zing_agent: 用户数据持久化（v7——替代 webview localStorage）
// 背景：webview set_html=data:URL 为 opaque origin，WebKitGTK 的
// localStorage 是内存态不落盘（实测 ~/.local/share/zing_agent 无存储文件）
// ——主题/会话/消息/主机档案全部随进程退出丢失。
// 方案：C++ 层单 JSON 文件写穿，用户目录：
//   Linux:   $ZING_STATE_PATH 或 $HOME/.config/zing_agent/state.json
//   Windows: %ZING_STATE_PATH% 或 %APPDATA%\zing_agent\state.json
// JS 侧经 cfgGetAll/cfgSet/cfgDel 三桥访问（webview 内不碰 localStorage）。
#pragma once

#include <cstdio>
#include <map>
#include <mutex>
#include <string>

namespace zing {

class ConfigStore {
 public:
  // 可注入路径（测试用；空=default_path）
  explicit ConfigStore(std::string path = "");

  // 路径解析（env 覆盖优先——测试/多实例隔离）
  static std::string default_path();

  // 读文件到内存（损坏 JSON=清空重开不炸）
  bool load();

  std::string get(const std::string& key, const std::string& def = "") const;
  bool has(const std::string& key) const;

  // 写穿：内存+立即落盘（全量重写；数据量为 KB 级可接受）
  void set(const std::string& key, const std::string& value);
  void del(const std::string& key);

  // 全量快照（cfgGetAll 桥：JS 启动预载一次往返）
  std::string all_json() const;

  const std::string& path() const { return path_; }

 private:
  void save_locked();  // 持锁调用

  mutable std::mutex mu_;
  std::map<std::string, std::string> kv_;
  std::string path_;
};

}  // namespace zing
