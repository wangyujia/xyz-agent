#pragma once

#include <string_view>

namespace thin_agent {

/// thin_agent 发布版本号，与 CHANGELOG / 握手 hello 保持一致。
inline constexpr std::string_view kThinAgentVersion = "v0.54.34";

}  // namespace thin_agent
