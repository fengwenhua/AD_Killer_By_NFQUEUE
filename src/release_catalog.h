#pragma once
#include <cstddef>
#include <functional>
#include <string>

namespace release_catalog {
constexpr size_t RELEASES_PER_PAGE = 5, MAX_PAGES = 4, PAGE_BYTES = 512u * 1024u;
constexpr size_t TOTAL_BYTES = 2u * 1024u * 1024u, NOTES_BYTES = 16000;
constexpr unsigned TIME_MS = 25000;
struct Page {
    bool valid = false;
    size_t records = 0, candidates = 0;
    std::string json, error;
};
// Discard API metadata and check-only bodies before crossing the WebUI bridge.
Page project(const std::string &body, const std::string &repo, bool notes = false, bool single = false);
struct Reply { bool ok = false; long http_status = 0; std::string json, error; size_t pages = 0; };
using Fetch = std::function<std::string(const std::string &, std::string &, size_t, long &)>;
// Read fixed page URLs only, never server-provided next links. Stop at the
// first page with valid candidates, end of listing, error or shared budget.
Reply scan(const std::string &api, const std::string &repo, const Fetch &fetch);
} // namespace release_catalog
