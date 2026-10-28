#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <vector>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int main(int argc, char **argv) {
    unsigned long runs = argc > 1 ? strtoul(argv[1], nullptr, 10) : 500;
    const size_t max_len = argc > 2 ? strtoul(argv[2], nullptr, 10) : 4096;
    if (max_len < 1 || max_len > 131072) return 2;
    for (int file = 3; file < argc; ++file) {
        std::ifstream stream(argv[file], std::ios::binary);
        if (!stream) return 2;
        std::vector<uint8_t> seed{std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
        if (seed.size() > max_len) return 2;
        LLVMFuzzerTestOneInput(seed.data(), seed.size());
    }
    uint64_t state = 0x9e3779b97f4a7c15ull;
    std::vector<uint8_t> input;
    LLVMFuzzerTestOneInput(nullptr, 0);
    for (unsigned long run = 0; run < runs; ++run) {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        size_t size = static_cast<size_t>(state % (max_len + 1));
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
