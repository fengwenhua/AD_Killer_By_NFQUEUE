#include "iptables.h"
#include "logging.h"
#include "source_config.h"
#include <cassert>
#include <cstdio>
#include <string>

int main(int argc, char **argv) {
    assert(argc == 2);
    if (std::string(argv[1]) == "root") { puts(DATA_DIR); return 0; }
    configure_process_log(DATA_DIR "/test.log", 65536, 1);
    const auto result = std::string(argv[1]) == "inspect" ? iptables_inspect_owned()
        : iptables_remove_all(IptablesCleanupLimits{2000, 200, 30});
    printf("{\"state\":%s,\"remaining\":[", source_json_string(result.name()).c_str());
    for (size_t i = 0; i < result.remaining.size(); ++i) printf("%s%s", i ? "," : "", source_json_string(result.remaining[i]).c_str());
    printf("],\"errors\":[");
    for (size_t i = 0; i < result.errors.size(); ++i) printf("%s%s", i ? "," : "", source_json_string(result.errors[i]).c_str());
    printf("]}\n"); return result.ok() ? 0 : 1;
}
