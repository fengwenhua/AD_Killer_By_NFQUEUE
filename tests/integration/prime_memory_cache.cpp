// Test-only admission of frozen raw inputs. Never linked into the module CLI.
#include "common.h"
#include "source_cache.h"
#include "source_sync.h"
#include "source_config.h"
#include "hagezi_source.h"
#include "exclusive_file_lock.h"
#include "operation_gate.h"
#include "logging.h"
#include <ctime>
#include <fstream>
#include <iostream>
#include <sstream>
#include <sys/stat.h>

int main(int argc, char **argv) {
    if (argc != 2 || geteuid() != 0) return 2;
    const std::string directory = argv[1];
    OperationGate gate(DATA_DIR, "", operation_executable(argv[0]));
    ExclusiveFileLock update(UPDATE_LOCK_FILE);
    if (!gate.ok() || !update.ok()) return 3;
    struct Input { std::string url; std::vector<std::string> files; };
    std::vector<Input> inputs;
    std::ifstream list(directory + "/sources.tsv");
    std::string line;
    while (std::getline(list, line)) {
        std::istringstream row(line);
        std::string format, file, url;
        if (!std::getline(row, format, '\t') || !std::getline(row, file, '\t') ||
            !std::getline(row, url) || file.empty() || file.find('/') != std::string::npos ||
            file == "." || file == ".." || format != source_format_name(recipe_source_format(url))) return 4;
        inputs.push_back({url, {directory + "/" + file}});
    }
    if (!list.eof() || inputs.empty()) return 4;
    std::ifstream pro(directory + "/hagezi-pro.txt"), fake(directory + "/hagezi-fake.txt");
    const bool paired = pro.good() && fake.good();
    if (pro.good() != fake.good()) return 4;
    if (paired) inputs.push_back({HAGEZI_SOURCE_ID, {directory + "/hagezi-pro.txt", directory + "/hagezi-fake.txt"}});
    std::set<std::string> keys;
    std::vector<RuleSource> sources;
    for (const auto &input : inputs) { keys.insert(source_identity_key(input.url)); sources.push_back({input.url, true}); }
    if (!paired) sources.push_back({HAGEZI_SOURCE_ID, false});
    SourceCache cache(DATA_DIR);
    if (!cache.available()) return 5;
    for (const auto &input : inputs) {
        const auto validated = is_builtin_source(input.url)
            ? merge_hagezi_pair(input.files[0], input.files[1])
            : validate_rule_source_file(input.files[0].c_str(), {}, recipe_source_format(input.url));
        if (!validated.accepted) { std::cerr << validated.reason << '\n'; return 6; }
        std::vector<CachedComponent> components;
        const auto urls = source_component_urls(input.url);
        for (size_t i = 0; i < input.files.size(); ++i) {
            CachedComponent value;
            value.path = input.files[i]; value.url = value.effective_url = urls[i];
            value.fetched_at = time(nullptr);
            struct stat info {};
            if (stat(value.path.c_str(), &info) || info.st_size <= 0) return 7;
            value.bytes = static_cast<uint64_t>(info.st_size);
            if (!hash_source_component(value)) return 7;
            components.push_back(std::move(value));
        }
        std::string error;
        if (!cache.commit(source_identity_key(input.url), components, time(nullptr), keys, error)) {
            std::cerr << error << '\n'; return 8;
        }
    }
    return write_file_atomic(FILE_UPDATE_SOURCES, serialize_rule_sources(sources)) ? 0 : 9;
}
