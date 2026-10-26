#include "release_catalog.h"
#include "bounded_json.h"
#include "module_version.h"
#include "source_config.h"
#include "work_budget.h"
#include <algorithm>

namespace release_catalog {
namespace {
using bounded_json::Value;
using bounded_json::Kind;
const std::string *text(const Value *value) { return value && value->kind == Kind::STRING ? &value->text : nullptr; }
bool boolean(const Value *value, bool expected) { return value && value->kind == Kind::BOOLEAN && value->boolean == expected; }
std::string bounded_text(const std::string &input, size_t maximum, bool &truncated) {
    size_t size = std::min(input.size(), maximum);
    while (size && size < input.size() && (static_cast<unsigned char>(input[size]) & 0xc0) == 0x80) --size;
    truncated = size < input.size();
    std::string result;
    for (size_t i = 0; i < size; ++i) {
        const auto ch = static_cast<unsigned char>(input[i]);
        if (ch >= 0x20 && ch != 0x7f) result += input[i];
        else if (ch == '\n' || ch == '\r' || ch == '\t') result += input[i];
    }
    return result;
}
std::string entry(const Value &value, const std::string &repo, bool notes) {
    const auto tag = text(value.get("tag_name")), url = text(value.get("html_url"));
    const auto preview = value.get("prerelease");
    if (!tag || tag->empty() || tag->front() != 'v' || !module_update::valid_version(tag->substr(1)) ||
        !url || *url != repo + "/releases/tag/" + *tag || !boolean(value.get("draft"), false) ||
        !preview || preview->kind != Kind::BOOLEAN) return "";
    std::string result = "{\"tag_name\":" + source_json_string(*tag) + ",\"draft\":false,\"prerelease\":" +
        (preview->boolean ? "true" : "false") + ",\"html_url\":" + source_json_string(*url);
    if (notes) {
        bool truncated = false, unused = false;
        const auto body = text(value.get("body")), name = text(value.get("name")), date = text(value.get("published_at"));
        result += ",\"body\":" + source_json_string(bounded_text(body ? *body : "", NOTES_BYTES, truncated)) +
            ",\"body_truncated\":" + (truncated ? "true" : "false") +
            ",\"name\":" + source_json_string(bounded_text(name ? *name : *tag, 200, unused)) +
            ",\"published_at\":" + source_json_string(bounded_text(date ? *date : "", 40, unused));
    } else {
        const auto assets = value.get("assets");
        if (!assets || assets->kind != Kind::ARRAY) return "";
        const auto package = "AD_Killer_By_NFQUEUE-" + *tag + ".zip";
        const auto download = repo + "/releases/download/" + *tag + "/" + package;
        uint64_t size = 0, checksum_size = 0;
        size_t zips = 0, checksums = 0;
        for (const auto &asset : assets->items) {
            const auto name = text(asset.get("name")), address = text(asset.get("browser_download_url"));
            if (!name) continue;
            const auto number = asset.get("size"); uint64_t bytes = 0;
            if (*name == package) {
                ++zips;
                if (!address || *address != download || !number || !number->integer(bytes) || bytes < 4 || bytes > 64u * 1024u * 1024u) return "";
                size = bytes;
            } else if (*name == package + ".sha256") {
                ++checksums;
                if (!address || *address != download + ".sha256" || !number || !number->integer(bytes) || !bytes || bytes > 512) return "";
                checksum_size = bytes;
            }
        }
        if (zips != 1 || checksums != 1) return "";
        result += ",\"assets\":[{\"name\":" + source_json_string(package) + ",\"size\":" + std::to_string(size) +
            ",\"browser_download_url\":" + source_json_string(download) + "},{\"name\":" + source_json_string(package + ".sha256") +
            ",\"size\":" + std::to_string(checksum_size) + ",\"browser_download_url\":" + source_json_string(download + ".sha256") + "}]";
    }
    return result + "}";
}
}
Page project(const std::string &body, const std::string &repo, bool notes, bool single) {
    Page result;
    Value parsed;
    if (!bounded_json::parse(body, parsed) || (single ? parsed.kind != Kind::OBJECT : parsed.kind != Kind::ARRAY)) {
        result.error = "GitHub 发布信息格式无效或超过解析限制"; return result;
    }
    result.records = single ? 1 : parsed.items.size();
    if (result.records > (notes || single ? 1 : RELEASES_PER_PAGE)) { result.error = "GitHub 发布信息超过本页记录上限"; return result; }
    result.json = "[";
    const auto append = [&](const Value &value) {
        if (single && !notes && !boolean(value.get("prerelease"), false)) return;
        const auto json = entry(value, repo, notes);
        if (!json.empty()) { if (result.candidates++) result.json += ','; result.json += json; }
    };
    if (single) append(parsed);
    else for (const auto &value : parsed.items) append(value);
    result.json += "]"; result.valid = true;
    return result;
}
Reply scan(const std::string &api, const std::string &repo, const Fetch &fetch) {
    Reply result;
    WorkBudget total(TIME_MS, TOTAL_BYTES);
    size_t remaining = TOTAL_BYTES;
    for (size_t page = 1; page <= MAX_PAGES; ++page) {
        const auto cap = static_cast<size_t>(WorkBudget::remaining_bytes(std::min(PAGE_BYTES, remaining)));
        if (WorkBudget::expired() || !cap) { result.error = "更新检查预算已耗尽，请稍后重试"; return result; }
        std::string body;
        result.error = fetch(api + "?per_page=" + std::to_string(RELEASES_PER_PAGE) + "&page=" + std::to_string(page), body, cap, result.http_status);
        ++result.pages;
        if (!result.error.empty()) return result;
        if (body.size() > cap) { result.error = "GitHub 发布信息超过本次正文预算"; return result; }
        remaining -= body.size();
        if (result.http_status != 200) { result.error = "GitHub 发布列表未返回完整 HTTP 200 响应"; return result; }
        if (WorkBudget::expired()) { result.error = "更新检查时间预算已耗尽"; return result; }
        const auto projected = project(body, repo);
        if (WorkBudget::expired()) { result.error = "更新检查时间预算已耗尽"; return result; }
        if (!projected.valid) { result.error = projected.error; return result; }
        if (projected.candidates) { result.ok = true; result.json = projected.json; return result; }
        if (projected.records < RELEASES_PER_PAGE) { result.error = "暂未找到包含有效安装包与校验和的版本"; return result; }
    }
    result.error = "已检查最近 20 条发布，仍未找到有效安装包；请查看 GitHub 或稍后重试";
    return result;
}
} // namespace release_catalog
