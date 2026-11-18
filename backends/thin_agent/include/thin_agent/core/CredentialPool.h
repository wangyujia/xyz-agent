#pragma once

#include <string>

namespace thin_agent {

/// 按 provider 存放 API key。文件缺失或字段为空时由 resolve_api_key 回退到环境变量。
class CredentialPool {
 public:
  void load(const std::string& path);
  std::string get(const std::string& provider, const std::string& field) const;

 private:
  std::string raw_;
};

}  // namespace thin_agent
