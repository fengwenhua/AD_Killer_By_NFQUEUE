// AD_Killer_By_NFQUEUE - 安全下载器:libcurl dlopen → 无 shell 的 curl/wget execv 兜底
// [libcurl] 系列日志文案
#pragma once

#include <cstddef>
#include <string>

// 下载 HTTP(S) url 到 path(原子临时文件 + rename,单源最大 64 MiB)
// 返回空串成功,否则错误信息
std::string download_file(const std::string &url, const std::string &path);

// In-memory prefix only: caller-specified cap <= 32768 bytes (default 4096),
// at most 20 seconds per
// backend, same HTTP(S)/TLS/redirect restrictions. Never writes formal rules.
std::string download_prefix(const std::string &url, std::string &prefix,
                            bool &limited, size_t byte_limit = 4096);

// libcurl 是否可用(dlopen 成功)
bool net_curl_available();

// 初始化 libcurl(尝试系统多个路径 dlopen)
bool net_curl_init();

// 释放 libcurl
void net_curl_shutdown();
