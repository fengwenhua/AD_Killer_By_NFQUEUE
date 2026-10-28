#include "module_package.h"
#include "fuzz_support.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    FuzzFile file(data, size);
    const auto stable = validate_module_identity(file.path, "1.2.2", "stable", 12200);
    if (stable.empty()) {
        fuzz_require(!validate_module_identity(file.path, "1.2.1", "stable", 12200).empty());
        fuzz_require(!validate_module_identity(file.path, "1.2.2", "preview", 12200).empty());
        fuzz_require(!validate_module_identity(file.path, "1.2.2", "stable", 2147483647).empty());
    }
    validate_module_identity(file.path, "1.2.2", "preview", 12200);
    return 0;
}
