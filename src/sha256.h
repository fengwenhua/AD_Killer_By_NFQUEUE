#pragma once
#include <array>
#include <cstdint>
#include <cstring>
#include <string>

// Streaming SHA-256 (FIPS 180-4). No Android crypto-library ABI dependency.
class Sha256 {
    std::array<uint32_t, 8> state_{{0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,
                                  0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19}};
    std::array<uint8_t, 64> buffer_{};
    uint64_t bytes_ = 0;
    size_t used_ = 0;
    static uint32_t rotate(uint32_t value, unsigned bits) {
        return (value >> bits) | (value << (32 - bits));
    }
    void block(const uint8_t *input) {
        static constexpr uint32_t k[64] = {
            0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
            0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
            0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
            0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
            0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
            0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
            0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
            0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};
        uint32_t w[64];
        for (size_t i = 0; i < 16; ++i)
            w[i] = (uint32_t(input[i*4]) << 24) | (uint32_t(input[i*4+1]) << 16) |
                   (uint32_t(input[i*4+2]) << 8) | input[i*4+3];
        for (size_t i = 16; i < 64; ++i) {
            uint32_t x = w[i-15], y = w[i-2];
            w[i] = w[i-16] + (rotate(x,7)^rotate(x,18)^(x>>3)) + w[i-7] +
                   (rotate(y,17)^rotate(y,19)^(y>>10));
        }
        auto s = state_;
        for (size_t i = 0; i < 64; ++i) {
            uint32_t t1 = s[7] + (rotate(s[4],6)^rotate(s[4],11)^rotate(s[4],25)) +
                          ((s[4]&s[5])^(~s[4]&s[6])) + k[i] + w[i];
            uint32_t t2 = (rotate(s[0],2)^rotate(s[0],13)^rotate(s[0],22)) +
                          ((s[0]&s[1])^(s[0]&s[2])^(s[1]&s[2]));
            s = {{t1+t2,s[0],s[1],s[2],s[3]+t1,s[4],s[5],s[6]}};
        }
        for (size_t i = 0; i < 8; ++i) state_[i] += s[i];
    }
public:
    void update(const void *data, size_t size) {
        auto input = static_cast<const uint8_t *>(data);
        bytes_ += size;
        while (size) {
            size_t count = size < 64-used_ ? size : 64-used_;
            memcpy(buffer_.data()+used_, input, count);
            input += count; size -= count; used_ += count;
            if (used_ == 64) { block(buffer_.data()); used_ = 0; }
        }
    }
    std::string finish() {
        uint64_t bits = bytes_ * 8;
        uint8_t marker = 0x80, zero = 0;
        update(&marker, 1);
        while (used_ != 56) update(&zero, 1);
        uint8_t length[8];
        for (size_t i = 0; i < 8; ++i) length[7-i] = uint8_t(bits >> (i*8));
        update(length, 8);
        const char *hex = "0123456789abcdef";
        std::string result;
        for (uint32_t word : state_)
            for (int shift = 28; shift >= 0; shift -= 4) result += hex[(word >> shift)&15];
        return result;
    }
};
