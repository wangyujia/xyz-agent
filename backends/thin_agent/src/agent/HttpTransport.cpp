#include "thin_agent/agent/HttpTransport.h"

#include <curl/curl.h>
#include <iostream>
#include "thin_agent/log/LogEvent.h"

namespace thin_agent {
namespace agent {

namespace {

// libcurl write callback
size_t write_cb(void* ptr, size_t size, size_t nmemb, void* userdata) {
  auto* buf = static_cast<std::string*>(userdata);
  size_t total = size * nmemb;
  buf->append(static_cast<const char*>(ptr), total);
  return total;
}

}  // namespace

bool HttpTransport::connect(const std::string& endpoint) {
  endpoint_ = endpoint;
  connected_ = true;
  log_event("mcp-http", LogLevel::Info, "endpoint",
            {{"endpoint", endpoint_}});
  return true;  // HTTP is stateless — "connect" just records the URL
}

std::string HttpTransport::send(const std::string& request) {
  if (endpoint_.empty()) {
    return R"({"jsonrpc":"2.0","error":{"code":-32000,"message":"not connected"}})";
  }

  CURL* curl = curl_easy_init();
  if (!curl) {
    return R"({"jsonrpc":"2.0","error":{"code":-32000,"message":"curl init failed"}})";
  }

  std::string response;
  struct curl_slist* headers = nullptr;
  headers = curl_slist_append(headers, "Content-Type: application/json");

  curl_easy_setopt(curl, CURLOPT_URL, endpoint_.c_str());
  curl_easy_setopt(curl, CURLOPT_POSTFIELDS, request.c_str());
  curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)request.size());
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);

  CURLcode res = curl_easy_perform(curl);

  long http_code = 0;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

  curl_slist_free_all(headers);
  curl_easy_cleanup(curl);

  if (res != CURLE_OK) {
    log_event("mcp-http", LogLevel::Error, "curl error",
              {{"error", curl_easy_strerror(res)}});
    return R"({"jsonrpc":"2.0","error":{"code":-32000,"message":"HTTP request failed"}})";
  }

  if (http_code != 200) {
    log_event("mcp-http", LogLevel::Warn, "HTTP status",
              {{"code", http_code}});
    return R"({"jsonrpc":"2.0","error":{"code":-32000,"message":"HTTP )"
           + std::to_string(http_code) + R"("}})";
  }

  return response;
}

void HttpTransport::disconnect() {
  connected_ = false;
  endpoint_.clear();
}

}  // namespace agent
}  // namespace thin_agent
