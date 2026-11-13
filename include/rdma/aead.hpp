#pragma once
// LRP-sec-2 安全层:AES-256-GCM(AES-128 亦支持) + AES-CMAC 密钥派生
// 算法为公开标准(FIPS-197 / SP 800-38D / RFC 4493),实现为本项目原创代码
// 已知答案测试见 tests/test_loopback.cpp::test_aead_kat

#include <cstddef>
#include <cstdint>

namespace rdma {
namespace aead {

constexpr size_t kKeyLen = 32;
constexpr size_t kNonceLen = 12;
constexpr size_t kTagLen = 16;

// AES 分组加密(key_len=16|32)
void aes_encrypt_block(const uint8_t* key, size_t key_len, const uint8_t in[16],
                       uint8_t out[16]);

// AES-CMAC(RFC 4493),tag 16B
void cmac(const uint8_t* key, size_t key_len, const uint8_t* msg, size_t len, uint8_t tag[16]);

// SP 800-108 计数器模式 KDF(以 CMAC 为 PRF):out = KDF(root, label, ctx)
void kdf(const uint8_t* root, size_t root_len, const char* label, const uint8_t* ctx,
         size_t ctx_len, uint8_t* out, size_t out_len);

// AES-256-GCM(96-bit nonce):ct 与 pt 等长,tag 16B
void seal(const uint8_t key[kKeyLen], const uint8_t nonce[kNonceLen], const uint8_t* aad,
          size_t aad_len, const uint8_t* pt, size_t pt_len, uint8_t* ct, uint8_t tag[kTagLen]);

bool open(const uint8_t key[kKeyLen], const uint8_t nonce[kNonceLen], const uint8_t* aad,
          size_t aad_len, const uint8_t* ct, size_t ct_len, const uint8_t tag[kTagLen],
          uint8_t* pt);

}  // namespace aead
}  // namespace rdma
