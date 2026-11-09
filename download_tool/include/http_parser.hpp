#pragma once

#include <string>
#include <unordered_map>

namespace download {

  struct HttpResponse {
    std::string version;//新增：协议版本
    int status_code;
    std::string reason;//新增：状态描述（存储状态码对应的文本描述）
    std::unordered_map<std::string, std::string> headers;
    // std::string body;
  };

  HttpResponse parse_http(std::string s);

} // namespace download
