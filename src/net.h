// AD_Killer_By_NFQUEUE - 安全下载器:libcurl dlopen → 无 shell 的 curl/wget execv 兜底
// [libcurl] 系列日志文案
#pragma once

#include <cstddef>
#include <string>

// 下载 HTTP(S) url 到 path(原子临时文件 + rename,单源最大 64 MiB)
// 返回空串成功,否则错误信息
// A WorkBudget scope adds shared time/body limits. Single-threaded CLI library
// transfers run in an owned child, supervised even during DNS/library init;
// unknown thread/library state selects exec before issuing the request.
std::string download_file(const std::string &url, const std::string &path);

// In-memory prefix only: caller-specified cap <= 32768 bytes (default 4096),
// at most 20 seconds per
// backend, same HTTP(S)/TLS/redirect restrictions. Never writes formal rules.
std::string download_prefix(const std::string &url, std::string &prefix,
                            bool &limited, size_t byte_limit = 4096);
// Same bounded operation, additionally returning the final HTTP status (0 when
// no response was received). Does not expose credentials or response headers.
std::string download_prefix_status(const std::string &url, std::string &prefix,
                                   bool &limited, size_t byte_limit, long &http_status);

// libcurl 是否可用(dlopen 成功)
bool net_curl_available();

// 初始化 libcurl(尝试系统多个路径 dlopen)
bool net_curl_init();

// 释放 libcurl
void net_curl_shutdown();
