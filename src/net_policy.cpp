#include "net_policy.h"

#include <cctype>
#include <algorithm>

namespace {

void set_error(std::string *error, const char *message) {
    if (error) *error = message;
}

}  // namespace

bool https_download(const std::string &url) {
    std::string prefix = url.substr(0,8);
    for (char &ch : prefix) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return prefix == "https://";
}

bool validate_download_url(const std::string &url, std::string *error) {
    if (url.empty() || url.size() > 4096) {
        set_error(error, "URL 长度非法");
        return false;
    }
    for (unsigned char ch : url) {
        if (ch <= 0x20 || ch == 0x7f || ch == '\\') {
            set_error(error, "URL 含空白、控制字符或反斜杠");
            return false;
        }
    }

    size_t scheme_end = url.find("://");
    if (scheme_end == std::string::npos) {
        set_error(error, "URL 缺少 scheme");
        return false;
    }
    std::string scheme = url.substr(0, scheme_end);
    for (char &ch : scheme) ch = static_cast<char>(std::tolower(
        static_cast<unsigned char>(ch)));
    if (scheme != "http" && scheme != "https") {
        set_error(error, "仅允许 HTTP(S) URL");
        return false;
    }

    size_t authority_begin = scheme_end + 3;
    size_t authority_end = url.find_first_of("/?#", authority_begin);
    if (authority_end == std::string::npos) authority_end = url.size();
    std::string authority = url.substr(authority_begin,
                                       authority_end - authority_begin);
    if (authority.empty() || authority.find('@') != std::string::npos) {
        set_error(error, "URL 主机为空或含用户凭据");
        return false;
    }

    std::string host = authority;
    if (authority[0] == '[') {
        size_t close = authority.find(']');
        if (close == std::string::npos || close == 1 ||
            (close + 1 < authority.size() && authority[close + 1] != ':')) {
            set_error(error, "IPv6 主机格式非法");
            return false;
        }
        host = authority.substr(1, close - 1);
    } else {
        size_t colon = authority.rfind(':');
        if (colon != std::string::npos) host = authority.substr(0, colon);
    }
    if (host.empty()) {
        set_error(error, "URL 主机为空");
        return false;
    }
    if (error) error->clear();
    return true;
}

std::vector<std::string> build_curl_download_argv(
        const std::string &executable, const std::string &url,
        const std::string &output_path, uint64_t timeout_ms, uint64_t max_bytes) {
    const auto seconds = [](uint64_t milliseconds) {
        if (milliseconds % 1000 == 0) return std::to_string(milliseconds / 1000);
        return std::to_string(milliseconds / 1000) + "." +
               std::to_string(1000 + milliseconds % 1000).substr(1);
    };
    return {
        executable,
        "--disable", "--globoff",
        "--fail", "--silent", "--show-error", "--location",
        "--max-redirs", std::to_string(MAX_DOWNLOAD_REDIRECTS),
        "--proto", "=http,https", "--proto-redir", https_download(url) ? "=https" : "=http,https",
        "--connect-timeout", seconds(std::min<uint64_t>(15000, timeout_ms)),
        "--max-time", seconds(timeout_ms),
        "--max-filesize", std::to_string(max_bytes),
        "--output", output_path, "--", url,
    };
}

std::vector<std::string> build_wget_download_argv(
        const std::string &executable, const std::string &url,
        const std::string &output_path) {
    return {executable, "--no-config", "--https-only", "--tries=1", "-q", "-T", "30",
            "--max-redirect=0", "-O", output_path, "--", url};
}
