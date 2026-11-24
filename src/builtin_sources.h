#pragma once
#include <string>
#include <vector>

inline constexpr const char *HAGEZI_SOURCE_ID = "builtin:hagezi-pro-minus-fake";
inline constexpr const char *HAGEZI_SOURCE_NAME = "HaGeZi Pro − Fake";
inline constexpr const char *HAGEZI_PRO_SOURCE_ID = "builtin:hagezi-pro";
inline constexpr const char *HAGEZI_PRO_SOURCE_NAME = "HaGeZi Pro";
#if defined(ADK_NFQ_TEST_HAGEZI_PRO_URL) && defined(ADK_NFQ_TEST_HAGEZI_FAKE_URL)
// Compile-time loopback fixtures only; production build.sh never defines these.
inline constexpr const char *HAGEZI_PRO_URL = ADK_NFQ_TEST_HAGEZI_PRO_URL;
inline constexpr const char *HAGEZI_FAKE_URL = ADK_NFQ_TEST_HAGEZI_FAKE_URL;
#else
inline constexpr const char *HAGEZI_PRO_URL =
    "https://raw.githubusercontent.com/hagezi/dns-blocklists/main/wildcard/pro.txt";
inline constexpr const char *HAGEZI_FAKE_URL =
    "https://raw.githubusercontent.com/hagezi/dns-blocklists/main/wildcard/fake.txt";
#endif
inline bool is_builtin_source(const std::string &id) { return id == HAGEZI_SOURCE_ID || id == HAGEZI_PRO_SOURCE_ID; }
inline const char *builtin_source_name(const std::string &id) {
    return id == HAGEZI_PRO_SOURCE_ID ? HAGEZI_PRO_SOURCE_NAME : HAGEZI_SOURCE_NAME;
}
inline const char *builtin_source_kind(const std::string &id) {
    return id == HAGEZI_PRO_SOURCE_ID ? "hagezi-pro" : "hagezi-pro-minus-fake";
}
inline std::vector<std::string> builtin_source_urls(const std::string &id) {
    return id == HAGEZI_PRO_SOURCE_ID ? std::vector<std::string>{HAGEZI_PRO_URL} :
                                      std::vector<std::string>{HAGEZI_PRO_URL, HAGEZI_FAKE_URL};
}
