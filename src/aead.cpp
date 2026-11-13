#include "rdma/aead.hpp"

#include <cstring>
#include <vector>

namespace rdma {
namespace aead {

namespace {

const uint8_t kSbox[256] = {
    0x63, 0x7c, 0x77, 0x7b, 0xf2, 0x6b, 0x6f, 0xc5, 0x30, 0x01, 0x67, 0x2b, 0xfe, 0xd7, 0xab, 0x76,
    0xca, 0x82, 0xc9, 0x7d, 0xfa, 0x59, 0x47, 0xf0, 0xad, 0xd4, 0xa2, 0xaf, 0x9c, 0xa4, 0x72, 0xc0,
    0xb7, 0xfd, 0x93, 0x26, 0x36, 0x3f, 0xf7, 0xcc, 0x34, 0xa5, 0xe5, 0xf1, 0x71, 0xd8, 0x31, 0x15,
    0x04, 0xc7, 0x23, 0xc3, 0x18, 0x96, 0x05, 0x9a, 0x07, 0x12, 0x80, 0xe2, 0xeb, 0x27, 0xb2, 0x75,
    0x09, 0x83, 0x2c, 0x1a, 0x1b, 0x6e, 0x5a, 0xa0, 0x52, 0x3b, 0xd6, 0xb3, 0x29, 0xe3, 0x2f, 0x84,
    0x53, 0xd1, 0x00, 0xed, 0x20, 0xfc, 0xb1, 0x5b, 0x6a, 0xcb, 0xbe, 0x39, 0x4a, 0x4c, 0x58, 0xcf,
    0xd0, 0xef, 0xaa, 0xfb, 0x43, 0x4d, 0x33, 0x85, 0x45, 0xf9, 0x02, 0x7f, 0x50, 0x3c, 0x9f, 0xa8,
    0x51, 0xa3, 0x40, 0x8f, 0x92, 0x9d, 0x38, 0xf5, 0xbc, 0xb6, 0xda, 0x21, 0x10, 0xff, 0xf3, 0xd2,
    0xcd, 0x0c, 0x13, 0xec, 0x5f, 0x97, 0x44, 0x17, 0xc4, 0xa7, 0x7e, 0x3d, 0x64, 0x5d, 0x19, 0x73,
    0x60, 0x81, 0x4f, 0xdc, 0x22, 0x2a, 0x90, 0x88, 0x46, 0xee, 0xb8, 0x14, 0xde, 0x5e, 0x0b, 0xdb,
    0xe0, 0x32, 0x3a, 0x0a, 0x49, 0x06, 0x24, 0x5c, 0xc2, 0xd3, 0xac, 0x62, 0x91, 0x95, 0xe4, 0x79,
    0xe7, 0xc8, 0x37, 0x6d, 0x8d, 0xd5, 0x4e, 0xa9, 0x6c, 0x56, 0xf4, 0xea, 0x65, 0x7a, 0xae, 0x08,
    0xba, 0x78, 0x25, 0x2e, 0x1c, 0xa6, 0xb4, 0xc6, 0xe8, 0xdd, 0x74, 0x1f, 0x4b, 0xbd, 0x8b, 0x8a,
    0x70, 0x3e, 0xb5, 0x66, 0x48, 0x03, 0xf6, 0x0e, 0x61, 0x35, 0x57, 0xb9, 0x86, 0xc1, 0x1d, 0x9e,
    0xe1, 0xf8, 0x98, 0x11, 0x69, 0xd9, 0x8e, 0x94, 0x9b, 0x1e, 0x87, 0xe9, 0xce, 0x55, 0x28, 0xdf,
    0x8c, 0xa1, 0x89, 0x0d, 0xbf, 0xe6, 0x42, 0x68, 0x41, 0x99, 0x2d, 0x0f, 0xb0, 0x54, 0xbb, 0x16,
};

uint8_t xtime(uint8_t v) { return uint8_t((v << 1) ^ ((v >> 7) * 0x1b)); }

void sub_bytes(uint8_t s[16]) {
  for (int i = 0; i < 16; i++) s[i] = kSbox[s[i]];
}

void shift_rows(uint8_t s[16]) {
  uint8_t t;
  t = s[1]; s[1] = s[5]; s[5] = s[9]; s[9] = s[13]; s[13] = t;
  t = s[2]; s[2] = s[10]; s[10] = t; t = s[6]; s[6] = s[14]; s[14] = t;
  t = s[15]; s[15] = s[11]; s[11] = s[7]; s[7] = s[3]; s[3] = t;
}

void mix_columns(uint8_t s[16]) {
  for (int c = 0; c < 4; c++) {
    uint8_t* p = s + 4 * c;
    uint8_t a0 = p[0], a1 = p[1], a2 = p[2], a3 = p[3];
    p[0] = uint8_t(xtime(a0) ^ xtime(a1) ^ a1 ^ a2 ^ a3);
    p[1] = uint8_t(a0 ^ xtime(a1) ^ xtime(a2) ^ a2 ^ a3);
    p[2] = uint8_t(a0 ^ a1 ^ xtime(a2) ^ xtime(a3) ^ a3);
    p[3] = uint8_t(xtime(a0) ^ a0 ^ a1 ^ a2 ^ xtime(a3));
  }
}

void add_round_key(uint8_t s[16], const uint8_t* rk) {
  for (int i = 0; i < 16; i++) s[i] ^= rk[i];
}

int key_schedule(const uint8_t* key, size_t key_len, uint8_t rk[240]) {
  int nk = int(key_len / 4);
  int nr = nk + 6;
  uint8_t w[60 * 4];
  std::memcpy(w, key, key_len);
  uint8_t rcon = 1;
  for (int i = nk; i < 4 * (nr + 1); i++) {
    uint8_t t[4] = {w[4 * (i - 1)], w[4 * (i - 1) + 1], w[4 * (i - 1) + 2], w[4 * (i - 1) + 3]};
    if (i % nk == 0) {
      uint8_t tmp = t[0];
      t[0] = uint8_t(kSbox[t[1]] ^ rcon);
      t[1] = kSbox[t[2]];
      t[2] = kSbox[t[3]];
      t[3] = kSbox[tmp];
      rcon = xtime(rcon);
    } else if (nk > 6 && i % nk == 4) {
      for (int j = 0; j < 4; j++) t[j] = kSbox[t[j]];
    }
    for (int j = 0; j < 4; j++) w[4 * i + j] = uint8_t(w[4 * (i - nk) + j] ^ t[j]);
  }
  std::memcpy(rk, w, size_t(4 * (nr + 1)) * 4);
  return nr;
}

void encrypt_block(const uint8_t* key, size_t key_len, const uint8_t in[16], uint8_t out[16]) {
  uint8_t rk[240];
  int nr = key_schedule(key, key_len, rk);
  uint8_t s[16];
  std::memcpy(s, in, 16);
  add_round_key(s, rk);
  for (int r = 1; r < nr; r++) {
    sub_bytes(s);
    shift_rows(s);
    mix_columns(s);
    add_round_key(s, rk + 16 * r);
  }
  sub_bytes(s);
  shift_rows(s);
  add_round_key(s, rk + 16 * nr);
  std::memcpy(out, s, 16);
}

// ---- GF(2^128) 乘法(GHASH 用,大端位序) ----
void gf_mult(const uint8_t x[16], const uint8_t y[16], uint8_t out[16]) {
  uint8_t z[16] = {0};
  uint8_t v[16];
  std::memcpy(v, y, 16);
  for (int i = 0; i < 128; i++) {
    if ((x[i / 8] >> (7 - (i % 8))) & 1) {
      for (int j = 0; j < 16; j++) z[j] ^= v[j];
    }
    uint8_t lsb = v[15] & 1;
    for (int j = 15; j > 0; j--) v[j] = uint8_t((v[j] >> 1) | (v[j - 1] << 7));
    v[0] = uint8_t(v[0] >> 1);
    if (lsb) v[0] ^= 0xe1;
  }
  std::memcpy(out, z, 16);
}

void xor16(uint8_t* d, const uint8_t* s) {
  for (int i = 0; i < 16; i++) d[i] ^= s[i];
}

void inc32(uint8_t counter[16]) {
  for (int i = 15; i >= 12; i--) {
    if (++counter[i]) break;
  }
}

void gctr(const uint8_t* key, size_t key_len, const uint8_t cb[16], const uint8_t* in, size_t len,
          uint8_t* out) {
  uint8_t counter[16];
  std::memcpy(counter, cb, 16);
  uint8_t ks[16];
  for (size_t off = 0; off < len; off += 16) {
    inc32(counter);
    encrypt_block(key, key_len, counter, ks);
    size_t n = len - off < 16 ? len - off : 16;
    for (size_t i = 0; i < n; i++) out[off + i] = in[off + i] ^ ks[i];
  }
}

void ghash(const uint8_t h[16], const uint8_t* a, size_t alen, const uint8_t* c, size_t clen,
           uint8_t out[16]) {
  uint8_t y[16] = {0};
  uint8_t block[16];
  uint8_t mul[16];
  for (size_t off = 0; off < alen; off += 16) {
    std::memset(block, 0, 16);
    size_t n = alen - off < 16 ? alen - off : 16;
    std::memcpy(block, a + off, n);
    xor16(y, block);
    gf_mult(y, h, mul);
    std::memcpy(y, mul, 16);
  }
  for (size_t off = 0; off < clen; off += 16) {
    std::memset(block, 0, 16);
    size_t n = clen - off < 16 ? clen - off : 16;
    std::memcpy(block, c + off, n);
    xor16(y, block);
    gf_mult(y, h, mul);
    std::memcpy(y, mul, 16);
  }
  std::memset(block, 0, 16);
  uint64_t abits = uint64_t(alen) * 8;
  uint64_t cbits = uint64_t(clen) * 8;
  for (int i = 0; i < 8; i++) {
    block[i] = uint8_t(abits >> (56 - 8 * i));
    block[8 + i] = uint8_t(cbits >> (56 - 8 * i));
  }
  xor16(y, block);
  gf_mult(y, h, mul);
  std::memcpy(out, mul, 16);
}

void gcm_core(const uint8_t* key, size_t key_len, const uint8_t nonce[12], const uint8_t* aad,
              size_t aad_len, const uint8_t* in, size_t len, uint8_t* out,
              const uint8_t* mac_body, uint8_t tag[16]) {
  uint8_t h[16];
  std::memset(h, 0, 16);
  encrypt_block(key, key_len, h, h);

  uint8_t j0[16];
  std::memcpy(j0, nonce, 12);
  j0[12] = j0[13] = j0[14] = 0;
  j0[15] = 1;

  gctr(key, key_len, j0, in, len, out);

  uint8_t s[16];
  ghash(h, aad, aad_len, mac_body, len, s);
  uint8_t ek0[16];
  encrypt_block(key, key_len, j0, ek0);
  for (int i = 0; i < 16; i++) tag[i] = ek0[i] ^ s[i];
}

bool ct_eq(const uint8_t* a, const uint8_t* b, size_t n) {
  uint8_t d = 0;
  for (size_t i = 0; i < n; i++) d |= uint8_t(a[i] ^ b[i]);
  return d == 0;
}

}  // namespace

void aes_encrypt_block(const uint8_t* key, size_t key_len, const uint8_t in[16], uint8_t out[16]) {
  encrypt_block(key, key_len, in, out);
}

void cmac(const uint8_t* key, size_t key_len, const uint8_t* msg, size_t len, uint8_t tag[16]) {
  uint8_t zero[16] = {0};
  uint8_t l[16], k1[16], k2[16];
  encrypt_block(key, key_len, zero, l);

  auto shift_left1 = [](const uint8_t* in, uint8_t* out) {
    uint8_t carry = 0;
    for (int i = 15; i >= 0; i--) {
      out[i] = uint8_t((in[i] << 1) | carry);
      carry = uint8_t(in[i] >> 7);
    }
  };
  shift_left1(l, k1);
  if (l[0] & 0x80) k1[15] ^= 0x87;
  shift_left1(k1, k2);
  if (k1[0] & 0x80) k2[15] ^= 0x87;

  size_t n = len == 0 ? 1 : (len + 15) / 16;
  bool last_complete = (len > 0 && len % 16 == 0);
  uint8_t x[16] = {0};
  uint8_t block[16];
  for (size_t i = 0; i < n - 1; i++) {
    std::memcpy(block, msg + 16 * i, 16);
    xor16(x, block);
    encrypt_block(key, key_len, x, x);
  }
  std::memset(block, 0, 16);
  size_t last_len = len == 0 ? 0 : len - 16 * (n - 1);
  if (last_len) std::memcpy(block, msg + 16 * (n - 1), last_len);
  if (!last_complete) block[last_len] = 0x80;
  xor16(block, last_complete ? k1 : k2);
  xor16(x, block);
  encrypt_block(key, key_len, x, tag);
}

void kdf(const uint8_t* root, size_t root_len, const char* label, const uint8_t* ctx,
         size_t ctx_len, uint8_t* out, size_t out_len) {
  size_t label_len = std::strlen(label);
  std::vector<uint8_t> buf;
  size_t done = 0;
  for (uint32_t i = 1; done < out_len; i++) {
    buf.clear();
    buf.push_back(uint8_t(i >> 24));
    buf.push_back(uint8_t(i >> 16));
    buf.push_back(uint8_t(i >> 8));
    buf.push_back(uint8_t(i));
    buf.insert(buf.end(), label, label + label_len);
    buf.push_back(0);
    buf.insert(buf.end(), ctx, ctx + ctx_len);
    uint32_t bits = uint32_t(out_len * 8);
    buf.push_back(uint8_t(bits >> 24));
    buf.push_back(uint8_t(bits >> 16));
    buf.push_back(uint8_t(bits >> 8));
    buf.push_back(uint8_t(bits));
    uint8_t blk[16];
    cmac(root, root_len, buf.data(), buf.size(), blk);
    size_t take = out_len - done < 16 ? out_len - done : 16;
    std::memcpy(out + done, blk, take);
    done += take;
  }
}

void seal(const uint8_t key[kKeyLen], const uint8_t nonce[kNonceLen], const uint8_t* aad,
          size_t aad_len, const uint8_t* pt, size_t pt_len, uint8_t* ct, uint8_t tag[kTagLen]) {
  gcm_core(key, kKeyLen, nonce, aad, aad_len, pt, pt_len, ct, ct, tag);
}

bool open(const uint8_t key[kKeyLen], const uint8_t nonce[kNonceLen], const uint8_t* aad,
          size_t aad_len, const uint8_t* ct, size_t ct_len, const uint8_t tag[kTagLen],
          uint8_t* pt) {
  uint8_t expect[kTagLen];
  gcm_core(key, kKeyLen, nonce, aad, aad_len, ct, ct_len, pt, ct, expect);
  return ct_eq(expect, tag, kTagLen);
}

}  // namespace aead
}  // namespace rdma
