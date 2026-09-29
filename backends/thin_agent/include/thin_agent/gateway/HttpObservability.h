#pragma once
// HttpObservability.h — 网关适配器 HTTP 发送的**统一**可观测底线（v0.53.83）
//
// 背景（fire-and-forget 可观测底线律，家族第 N 次复发）：
// R67/HHH2 给 FeishuAdapter 补了"发送失败可观测"，但**只改了 http_delete**
// （反应删除路径），真正发消息的 `http_post` 仍是裸 `curl_easy_perform(...)`
// ——返回值全吞、无状态码、无日志。同族还有 DiscordAdapter::http_request、
// TelegramAdapter::http_post_json/http_get、WechatAdapter::http_post。
// 逐个手补必然再漏（这家族已漏 8 次），故提为**一处判据、四适配器共用**。
//
// 语义边界（刻意保守）：
//   * 只观测，不改控制流：不重试、不抛异常、不阻塞——fire-and-forget 哲学不变
//     （丢消息可容忍，"丢了没人知道"不可容忍）。
//   * 失败判据 = 传输层错误(CURLcode != CURLE_OK) 或 HTTP 状态码 >= 400。
//   * 输出走 stderr 单行固定格式，便于 grep/日志看板/告警对接。
#include <curl/curl.h>

#include <cstdio>
#include <string>

namespace thin_agent {
namespace gateway {

/// 一次 HTTP 发送是否算失败（传输错误，或 HTTP >= 400）。
inline bool http_send_failed(CURLcode perf, long http_code) {
  if (perf != CURLE_OK) return true;
  return http_code >= 400;
}

/// 失败文案；成功返回空串。格式：[<platform>] 发送失败: transport=<...>
/// 或 http=<code> url=<...>
inline std::string http_send_failure_message(const char* platform, CURLcode perf,
                                             long http_code, const std::string& url) {
  if (!http_send_failed(perf, http_code)) return "";
  std::string msg = "[";
  msg += (platform != nullptr && *platform != '\0') ? platform : "gateway";
  msg += "] 发送失败: ";
  if (perf != CURLE_OK) {
    msg += "transport=";
    msg += curl_easy_strerror(perf);
  } else {
    msg += "http=";
    msg += std::to_string(http_code);
  }
  msg += " url=";
  msg += url;
  return msg;
}

/// 统一观测包装：执行请求 → 取响应码 → 失败打一行 stderr 并返回 perf。
/// 调用方**无需**检查返回值（fire-and-forget），但失败不再静默。
inline CURLcode curl_perform_observed(CURL* curl, const char* platform,
                                      const std::string& url) {
  const CURLcode perf = curl_easy_perform(curl);
  long http_code = 0;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
  const std::string msg =
      http_send_failure_message(platform, perf, http_code, url);
  if (!msg.empty()) {
    std::fprintf(stderr, "%s\n", msg.c_str());
    std::fflush(stderr);
  }
  return perf;
}

}  // namespace gateway
}  // namespace thin_agent
