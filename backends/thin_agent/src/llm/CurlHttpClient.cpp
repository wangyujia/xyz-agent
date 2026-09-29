#include "thin_agent/llm/CurlHttpClient.h"

#include <curl/curl.h>

namespace {

/// libcurl 写回调。
size_t write_cb(char* ptr, size_t size, size_t nmemb, void* userdata) {
  auto* out = static_cast<std::string*>(userdata);
  out->append(ptr, size * nmemb);
  return size * nmemb;
}

/// v0.53.33: 流式写回调——append 全量 + 增量喂 on_data。
struct StreamSink {
  std::string* body;
  std::function<void(const char*, size_t)>* on_data;
};
size_t stream_write_cb(char* ptr, size_t size, size_t nmemb, void* userdata) {
  auto* sink = static_cast<StreamSink*>(userdata);
  const size_t n = size * nmemb;
  sink->body->append(ptr, n);
  if (sink->on_data && *sink->on_data) (*sink->on_data)(ptr, n);
  return n;
}

}  // namespace

namespace thin_agent {

CurlHttpClient::CurlHttpClient(long timeout_ms) : timeout_ms_(timeout_ms) {}

HttpResponse CurlHttpClient::post(const std::string& url,
                                   const std::string& body_json,
                                   const std::string& auth_bearer,
                                   const std::string& api_mode) {
  return post_streaming(url, body_json, auth_bearer, api_mode, nullptr);
}

/// v0.53.33: 公共实现——on_data 非空走流式写回调。
HttpResponse CurlHttpClient::post_streaming(const std::string& url,
                              const std::string& body_json,
                              const std::string& auth_bearer,
                              const std::string& api_mode,
                              std::function<void(const char*, size_t)> on_data) {
  HttpResponse out;

  CURL* curl = curl_easy_init();
  if (!curl) {
    out.ok = false;
    out.error = "curl_init_failed";
    return out;
  }

  struct curl_slist* headers = nullptr;
  headers = curl_slist_append(headers, "Content-Type: application/json");
  if (!auth_bearer.empty()) {
    if (api_mode == "anthropic_messages") {
      // Anthropic Messages API: x-api-key header
      const std::string auth = std::string("x-api-key: ") + auth_bearer;
      headers = curl_slist_append(headers, auth.c_str());
      headers = curl_slist_append(headers, "anthropic-version: 2023-06-01");
    } else {
      // OpenAI-compatible: Authorization: Bearer
      const std::string auth = std::string("Authorization: Bearer ") + auth_bearer;
      headers = curl_slist_append(headers, auth.c_str());
    }
  }

  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
  curl_easy_setopt(curl, CURLOPT_POST, 1L);
  curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body_json.c_str());
  curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE,
                   static_cast<long>(body_json.size()));
  StreamSink sink{&out.body, &on_data};
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,
                   on_data ? stream_write_cb : write_cb);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA,
                   on_data ? static_cast<void*>(&sink)
                           : static_cast<void*>(&out.body));
  curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, timeout_ms_);

  const CURLcode rc = curl_easy_perform(curl);

  long code = 0;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
  out.status_code = static_cast<int>(code);

  curl_slist_free_all(headers);
  curl_easy_cleanup(curl);

  if (rc != CURLE_OK) {
    out.ok = false;
    out.error = std::string("curl_error:") + curl_easy_strerror(rc);
  } else {
    out.ok = true;
  }
  return out;
}

HttpResponse CurlHttpClient::get(const std::string& url,
                                  const std::string& auth_bearer) {
  HttpResponse out;

  CURL* curl = curl_easy_init();
  if (!curl) {
    out.ok = false;
    out.error = "curl_init_failed";
    return out;
  }

  struct curl_slist* headers = nullptr;
  if (!auth_bearer.empty()) {
    const std::string auth = "Authorization: Bearer " + auth_bearer;
    headers = curl_slist_append(headers, auth.c_str());
  }

  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  if (headers) curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
  curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &out.body);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, timeout_ms_);
  // v0.53.10: 浏览器 UA + 跟随重定向——网页类数据源（Bing 搜索等）必需；
  // 302 是常态（区域跳转），无 UA 会被反爬层拒。
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 3L);
  curl_easy_setopt(curl, CURLOPT_USERAGENT,
                   "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 "
                   "(KHTML, like Gecko) Chrome/124.0 Safari/537.36");

  const CURLcode rc = curl_easy_perform(curl);

  long code = 0;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
  out.status_code = static_cast<int>(code);

  if (headers) curl_slist_free_all(headers);
  curl_easy_cleanup(curl);

  if (rc != CURLE_OK) {
    out.ok = false;
    out.error = std::string("curl_error:") + curl_easy_strerror(rc);
  } else {
    out.ok = true;
  }
  return out;
}

}  // namespace thin_agent
