#include "bounded_json.h"
#include "release_catalog.h"
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    std::string input;
    if (size) input.assign(reinterpret_cast<const char *>(data), size);
    bounded_json::Value parsed;
    bounded_json::parse(input, parsed);
    const std::string repo = "https://github.com/fengwenhua/AD_Killer_By_NFQUEUE";
    for (bool notes : {false, true}) {
        const auto page = release_catalog::project(input, repo, notes);
        if (page.valid) {
            bounded_json::Value projected;
            if (!bounded_json::parse(page.json, projected) || projected.kind != bounded_json::Kind::ARRAY) std::abort();
        }
    }
    return 0;
}
