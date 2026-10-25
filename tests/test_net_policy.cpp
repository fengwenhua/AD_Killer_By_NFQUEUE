#include "net_policy.h"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace {

int failures = 0;

void expect(bool condition, const char *message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << "\n";
        failures++;
    }
}

}  // namespace

int main() {
    std::string error;
    expect(validate_download_url("https://example.com/rules.txt", &error),
           "HTTPS URL should be accepted");
    expect(validate_download_url("HTTP://[2001:db8::1]:8080/a", &error),
           "HTTP URL with bracketed IPv6 should be accepted");
    expect(error.empty(), "successful URL validation should clear the error");

    expect(!validate_download_url("file:///data/local/tmp/rules", &error),
           "file URL must be rejected");
    expect(!validate_download_url("ftp://example.com/rules", &error),
           "FTP URL must be rejected");
    expect(!validate_download_url("https://user:pass@example.com/rules", &error),
           "URL credentials must be rejected");
    expect(!validate_download_url("https:///rules", &error),
           "URL without a host must be rejected");
    expect(!validate_download_url("https://[2001:db8::1/rules", &error),
           "malformed bracketed IPv6 must be rejected");
    expect(!validate_download_url("https://example.com/a\n--output=/tmp/x", &error),
           "control characters must be rejected");
    expect(!validate_download_url("https:\\example.com\\rules", &error),
           "backslash URL ambiguity must be rejected");

    const std::string metachar_url =
        "https://example.com/rules;touch${IFS}/data/local/tmp/pwn";
    expect(validate_download_url(metachar_url, &error),
           "shell metacharacters are safe when preserved as one exec argument");
    std::vector<std::string> curl = build_curl_download_argv(
        "/system/bin/curl", metachar_url, "/data/local/tmp/out");
    expect(curl.front() == "/system/bin/curl" && curl.back() == metachar_url,
           "curl argv must preserve URL as one final argument");
    expect(curl[curl.size() - 2] == "--",
           "curl argv must terminate option parsing before URL");
    expect(std::find(curl.begin(), curl.end(), "=http,https") != curl.end(),
           "curl must restrict initial and redirect protocols");
    expect(*(std::find(curl.begin(), curl.end(), "--proto-redir")+1) == "=https",
           "HTTPS downloads must reject redirects to HTTP");
    expect(curl.size() > 2 && curl[1] == "--disable" && curl[2] == "--globoff",
           "curl must ignore curlrc and not expand URL glob expressions");

    std::vector<std::string> wget = build_wget_download_argv(
        "/system/bin/wget", metachar_url,
        "/data/local/tmp/out");
    expect(wget.back() == metachar_url && wget[wget.size() - 2] == "--",
           "GNU wget must preserve URL as one argument after option termination");
    expect(wget[1] == "--no-config" && wget[2] == "--https-only",
           "wget fallback must require documented GNU safety options");
    expect(std::find(wget.begin(), wget.end(), "--max-redirect=0") != wget.end(),
           "wget fallback must disable redirects it cannot safely constrain");
    expect(std::find(wget.begin(), wget.end(), "--tries=1") != wget.end(),
           "wget must not silently retry a failed source download");
    expect(MAX_RULE_DOWNLOAD_BYTES == 64u * 1024u * 1024u,
           "download size policy should remain 64 MiB");

    if (failures != 0) {
        std::cerr << failures << " network policy test(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "network policy tests passed\n";
    return EXIT_SUCCESS;
}
