// Self-contained SHA-256 (FIPS 180-4) for the auto-updater's checksum checks.
// No OpenSSL, no platform headers — builds cleanly under MinGW as well as GCC.
#include "updater_sha256.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace updater {
namespace {

// Compression-round constants K[0..63] (first 32 bits of the fractional parts
// of the cube roots of the first 63 primes).
constexpr uint32_t kK[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u,
    0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
    0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
    0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
    0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
    0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
    0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
    0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
    0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u,
    0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u,
    0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
    0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u,
};

constexpr uint32_t rotr32(uint32_t x, unsigned n) {
    return (x >> n) | (x << (32u - n));
}

// FIPS 180-4 §4.1.2 message-schedule and compression-function helpers.
constexpr uint32_t bigSigma0(uint32_t x) { return rotr32(x, 2) ^ rotr32(x, 13) ^ rotr32(x, 22); }
constexpr uint32_t bigSigma1(uint32_t x) { return rotr32(x, 6) ^ rotr32(x, 11) ^ rotr32(x, 25); }
constexpr uint32_t smallSigma0(uint32_t x) { return rotr32(x, 7) ^ rotr32(x, 18) ^ (x >> 3); }
constexpr uint32_t smallSigma1(uint32_t x) { return rotr32(x, 17) ^ rotr32(x, 19) ^ (x >> 10); }
constexpr uint32_t choose(uint32_t x, uint32_t y, uint32_t z) { return (x & y) ^ (~x & z); }
constexpr uint32_t majority(uint32_t x, uint32_t y, uint32_t z) { return (x & y) ^ (x & z) ^ (y & z); }

// Incremental SHA-256 state: buffers full 64-byte blocks and compresses them.
class Sha256Ctx {
public:
    Sha256Ctx()
        : h_{0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
             0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u} {}

    void update(const uint8_t* data, std::size_t size) {
        for (std::size_t i = 0; i < size; ++i) {
            buffer_[bufferLen_] = data[i];
            ++bufferLen_;
            totalBytes_ += 1;
            if (bufferLen_ == 64) {
                compress(buffer_.data());
                bufferLen_ = 0;
            }
        }
    }

    // Appends the FIPS padding and writes the 32-byte big-endian digest.
    void finish(uint8_t out[32]) {
        const uint64_t bitLen = totalBytes_ * 8ull;
        const uint8_t stop = 0x80u;
        const uint8_t zero = 0x00u;
        update(&stop, 1);
        while (bufferLen_ != 56) {
            update(&zero, 1);
        }
        uint8_t len[8];
        for (int i = 0; i < 8; ++i) {
            len[i] = static_cast<uint8_t>(bitLen >> (56 - 8 * i));
        }
        update(len, 8);
        for (int i = 0; i < 8; ++i) {
            out[4 * i + 0] = static_cast<uint8_t>(h_[i] >> 24);
            out[4 * i + 1] = static_cast<uint8_t>(h_[i] >> 16);
            out[4 * i + 2] = static_cast<uint8_t>(h_[i] >> 8);
            out[4 * i + 3] = static_cast<uint8_t>(h_[i]);
        }
    }

private:
    void compress(const uint8_t block[64]) {
        uint32_t w[64];
        for (int i = 0; i < 16; ++i) {
            w[i] = (static_cast<uint32_t>(block[4 * i]) << 24) |
                   (static_cast<uint32_t>(block[4 * i + 1]) << 16) |
                   (static_cast<uint32_t>(block[4 * i + 2]) << 8) |
                   (static_cast<uint32_t>(block[4 * i + 3]));
        }
        for (int i = 16; i < 64; ++i) {
            w[i] = smallSigma1(w[i - 2]) + w[i - 7] + smallSigma0(w[i - 15]) + w[i - 16];
        }
        uint32_t a = h_[0], b = h_[1], c = h_[2], d = h_[3];
        uint32_t e = h_[4], f = h_[5], g = h_[6], hh = h_[7];
        for (int i = 0; i < 64; ++i) {
            const uint32_t t1 = hh + bigSigma1(e) + choose(e, f, g) + kK[i] + w[i];
            const uint32_t t2 = bigSigma0(a) + majority(a, b, c);
            hh = g;
            g = f;
            f = e;
            e = d + t1;
            d = c;
            c = b;
            b = a;
            a = t1 + t2;
        }
        h_[0] += a; h_[1] += b; h_[2] += c; h_[3] += d;
        h_[4] += e; h_[5] += f; h_[6] += g; h_[7] += hh;
    }

    std::array<uint32_t, 8> h_;
    std::array<uint8_t, 64> buffer_{};
    std::size_t bufferLen_ = 0;
    uint64_t totalBytes_ = 0;
};

// Lowercase hex encoding of a 32-byte digest.
std::string toHex(const uint8_t digest[32]) {
    static const char* kHex = "0123456789abcdef";
    std::string out;
    out.reserve(64);
    for (int i = 0; i < 32; ++i) {
        out.push_back(kHex[digest[i] >> 4]);
        out.push_back(kHex[digest[i] & 0x0Fu]);
    }
    return out;
}

} // namespace

std::string sha256Hex(const void* data, std::size_t size) {
    Sha256Ctx ctx;
    if (size > 0 && data != nullptr) {
        ctx.update(static_cast<const uint8_t*>(data), size);
    }
    uint8_t digest[32];
    ctx.finish(digest);
    return toHex(digest);
}

std::string sha256FileHex(const std::string& path, std::string& error) {
    error.clear();
    FILE* f = std::fopen(path.c_str(), "rb");
    if (f == nullptr) {
        error = "cannot open file: " + path;
        return {};
    }
    // 64 KiB chunks: a 40 MB binary hashes with a handful of syscalls.
    constexpr std::size_t kChunk = 64 * 1024;
    std::array<uint8_t, kChunk> chunk{};
    Sha256Ctx ctx;
    for (;;) {
        const std::size_t n = std::fread(chunk.data(), 1, kChunk, f);
        if (n > 0) {
            ctx.update(chunk.data(), n);
        }
        if (n != kChunk) {
            if (std::ferror(f) != 0) {
                std::fclose(f);
                error = "read error: " + path;
                return {};
            }
            break; // EOF
        }
    }
    std::fclose(f);
    uint8_t digest[32];
    ctx.finish(digest);
    return toHex(digest);
}

} // namespace updater
