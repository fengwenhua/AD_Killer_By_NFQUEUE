#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

constexpr size_t MAX_RULE_DOWNLOAD_BYTES = 64u * 1024u * 1024u;
constexpr int MAX_DOWNLOAD_REDIRECTS = 5;

// 仅允许结构完整、无凭据/控制字符的 HTTP(S) URL。
bool validate_download_url(const std::string &url, std::string *error = nullptr);
bool https_download(const std::string &url);

// argv 构造保持 URL 为单独参数；调用方必须使用 execv,不得交给 shell。
std::vector<std::string> build_curl_download_argv(
    const std::string &executable, const std::string &url,
    const std::string &output_path, uint64_t timeout_ms = 300000,
    uint64_t max_bytes = MAX_RULE_DOWNLOAD_BYTES);
std::vector<std::string> build_wget_download_argv(
    const std::string &executable, const std::string &url,
    const std::string &output_path);
