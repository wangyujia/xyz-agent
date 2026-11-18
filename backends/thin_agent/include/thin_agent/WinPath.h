#pragma once

#include <string>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace thin_agent {

// ONNX Runtime 在 Windows 上收 wchar_t 路径。配置里的路径按 UTF-8 存，
// 不是合法 UTF-8 时再按系统 ANSI 代码页转。
inline std::wstring win_path(const std::string& s) {
  if (s.empty()) return std::wstring();
  const int len = static_cast<int>(s.size());
  int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), len, nullptr, 0);
  UINT cp = CP_UTF8;
  DWORD flags = MB_ERR_INVALID_CHARS;
  if (n <= 0) {
    cp = CP_ACP;
    flags = 0;
    n = MultiByteToWideChar(CP_ACP, 0, s.data(), len, nullptr, 0);
  }
  if (n <= 0) return std::wstring(s.begin(), s.end());
  std::wstring w(static_cast<size_t>(n), L'\0');
  MultiByteToWideChar(cp, flags, s.data(), len, w.data(), n);
  return w;
}

}  // namespace thin_agent
#endif
