#include "libkcrypto/sha256.h"

// libcpio/libelf와 같은 이유로 <cstdint>를 쓰지 않는다(freestanding
// 타깃엔 표준 헤더가 없음) - x86_64에서 unsigned int=32비트/unsigned
// long=64비트는 이 프로젝트 전역이 이미 전제하는 사실.
namespace sha256 {

namespace {

using u8 = unsigned char;
using u32 = unsigned int;
using u64 = unsigned long;

// FIPS 180-4 §4.2.2 - 처음 64개 소수의 세제곱근 소수부 32비트.
constexpr u32 kRoundConstants[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

u32 rotr(u32 x, u32 n) {
    return (x >> n) | (x << (32 - n));
}

// 한 64바이트 블록을 압축해 h[8]을 제자리에서 갱신한다(FIPS 180-4 §6.2.2).
void compressBlock(u32 h[8], const u8 block[64]) {
    u32 w[64];
    for (u32 i = 0; i < 16; ++i) {
        w[i] = (static_cast<u32>(block[i * 4]) << 24) | (static_cast<u32>(block[i * 4 + 1]) << 16) |
               (static_cast<u32>(block[i * 4 + 2]) << 8) | static_cast<u32>(block[i * 4 + 3]);
    }
    for (u32 i = 16; i < 64; ++i) {
        const u32 s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        const u32 s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }

    u32 a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
    for (u32 i = 0; i < 64; ++i) {
        const u32 s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
        const u32 ch = (e & f) ^ ((~e) & g);
        const u32 temp1 = hh + s1 + ch + kRoundConstants[i] + w[i];
        const u32 s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
        const u32 maj = (a & b) ^ (a & c) ^ (b & c);
        const u32 temp2 = s0 + maj;
        hh = g;
        g = f;
        f = e;
        e = d + temp1;
        d = c;
        c = b;
        b = a;
        a = temp1 + temp2;
    }

    h[0] += a;
    h[1] += b;
    h[2] += c;
    h[3] += d;
    h[4] += e;
    h[5] += f;
    h[6] += g;
    h[7] += hh;
}

}  // namespace

Digest hash(const unsigned char* data, unsigned long length) {
    u32 h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};

    // 마지막 블록(들)을 미리 조립한다 - 패스워드 해싱(최대 64바이트)이
    // 유일한 예상 용도라 전체 메시지 + 패딩이 128바이트(블록 2개)를
    // 넘을 일이 없지만, 이 함수 자체는 임의 길이를 올바르게 처리한다
    // (스트리밍 API가 아니므로 전체를 스택에 올려도 안전한 범위로
    // 호출부가 제한할 책임 - RM-23F4B687 §4).
    u64 fullBlocks = length / 64;
    for (u64 i = 0; i < fullBlocks; ++i) {
        compressBlock(h, data + i * 64);
    }

    const u64 remaining = length - fullBlocks * 64;
    u8 tail[128] = {};
    for (u64 i = 0; i < remaining; ++i) {
        tail[i] = data[fullBlocks * 64 + i];
    }
    tail[remaining] = 0x80;

    const u64 bitLength = length * 8;
    const bool needsSecondBlock = remaining >= 56;
    const u64 tailBlocks = needsSecondBlock ? 2 : 1;
    u8* lengthField = tail + tailBlocks * 64 - 8;
    for (u32 i = 0; i < 8; ++i) {
        lengthField[i] = static_cast<u8>(bitLength >> (56 - i * 8));
    }

    for (u64 i = 0; i < tailBlocks; ++i) {
        compressBlock(h, tail + i * 64);
    }

    Digest digest{};
    for (u32 i = 0; i < 8; ++i) {
        digest.bytes[i * 4] = static_cast<u8>(h[i] >> 24);
        digest.bytes[i * 4 + 1] = static_cast<u8>(h[i] >> 16);
        digest.bytes[i * 4 + 2] = static_cast<u8>(h[i] >> 8);
        digest.bytes[i * 4 + 3] = static_cast<u8>(h[i]);
    }
    return digest;
}

void toHex(const Digest& digest, char* out) {
    constexpr char kHexDigits[] = "0123456789abcdef";
    for (unsigned int i = 0; i < kDigestBytes; ++i) {
        out[i * 2] = kHexDigits[digest.bytes[i] >> 4];
        out[i * 2 + 1] = kHexDigits[digest.bytes[i] & 0x0f];
    }
}

}  // namespace sha256
