#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <vector>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int main(int argc, char **argv) {
    unsigned long runs = argc > 1 ? strtoul(argv[1], nullptr, 10) : 500;
    uint64_t state = 0x9e3779b97f4a7c15ull;
    std::vector<uint8_t> input;
    LLVMFuzzerTestOneInput(nullptr, 0);
    for (unsigned long run = 0; run < runs; ++run) {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        size_t size = static_cast<size_t>(state % 4097);
        input.resize(size);
        for (size_t i = 0; i < size; ++i) {
            state ^= state << 13;
            state ^= state >> 7;
            state ^= state << 17;
            input[i] = static_cast<uint8_t>(state);
        }
        LLVMFuzzerTestOneInput(input.data(), input.size());
    }
    return 0;
}
