#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct llama_model;
struct llama_context;

namespace thin_agent {
namespace local {

/// 运行时动态加载 mtmd.dll，对接 llama.cpp 多模态（mmproj）。
/// 预编译包常有 DLL 无 import lib，故不静态链接。
class MtmdSession {
 public:
  MtmdSession() = default;
  ~MtmdSession();

  MtmdSession(const MtmdSession&) = delete;
  MtmdSession& operator=(const MtmdSession&) = delete;

  /// 加载动态库 + mmproj。text_model 须已加载。
  bool init(llama_model* text_model, const std::string& mmproj_path,
            int n_threads = 4);

  void reset();

  bool ready() const { return ctx_ != nullptr && vision_; }
  bool vision() const { return vision_; }
  const std::string& mmproj_path() const { return mmproj_path_; }
  /// 插入到 prompt 中的媒体占位符（默认 <__media__>）
  const char* media_marker() const;

  /// 将带 marker 的 prompt + 图像字节（jpeg/png…）编码进 llama_context。
  /// images 数量须等于 prompt 中 marker 出现次数。
  bool eval_prompt(llama_context* lctx, const std::string& prompt,
                   const std::vector<std::vector<uint8_t>>& images);

 private:
  void* ctx_ = nullptr;  ///< mtmd_context*
  bool vision_ = false;
  std::string mmproj_path_;
};

/// 动态库是否可加载（不要求已 init session）。
bool mtmd_runtime_available();

/// 根据主 GGUF 路径 / 环境变量解析 mmproj。
/// 顺序：THIN_AGENT_MMPROJ → 同目录 *mmproj*.gguf → <stem>.mmproj*.gguf
std::string resolve_mmproj_path(const std::string& model_path);

}  // namespace local
}  // namespace thin_agent
