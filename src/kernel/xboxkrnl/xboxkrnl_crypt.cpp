/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <algorithm>
#include <limits>

#include <rex/kernel/xboxkrnl/private.h>
#include <rex/logging.h>
#include <rex/platform.h>
#include <rex/hook.h>
#include <rex/types.h>
#include <rex/runtime.h>
#include <rex/runtime/xtypes.h>
#include "kernel/kernel_macros.h"

#if REX_PLATFORM_WIN32
#include <windows.h>

#include <bcrypt.h>
#endif

#include "crypto/TinySHA1.hpp"
#include "crypto/des/des.h"
#include "crypto/des/des3.h"
#include "crypto/des/descbc.h"
#include "crypto/sha256.h"

extern "C" {
#include "aes_128/aes.h"
}

namespace rex::kernel::xboxkrnl {

struct XECRYPT_RC4_STATE {
  u8 s[256];
  u8 i;
  u8 j;
};
REX_STATIC_ASSERT_SIZE(XECRYPT_RC4_STATE, 0x102);

void XeCryptRc4Key_entry(GuestPtrT<XECRYPT_RC4_STATE> rc4_ctx, MappedVoid key,
                         [[maybe_unused]] u32 key_size) {
  rc4_ctx->i = rc4_ctx->j = 0;
  for (u32 x = 0; x < 0x100; x++) {
    rc4_ctx->s[x] = static_cast<u8>(x);
  }

  u32 idx = 0;
  for (u32 x = 0; x < 0x100; x++) {
    idx = (idx + rc4_ctx->s[x] + key[x % 0x10]) % 0x100;
    u8 temp = rc4_ctx->s[idx];
    rc4_ctx->s[idx] = rc4_ctx->s[x];
    rc4_ctx->s[x] = temp;
  }
}

void XeCryptRc4Ecb_entry(GuestPtrT<XECRYPT_RC4_STATE> rc4_ctx, MappedVoid data, u32 size) {
  for (u32 idx = 0; idx < size; idx++) {
    rc4_ctx->i = (rc4_ctx->i + 1) % 0x100;
    rc4_ctx->j = (rc4_ctx->j + rc4_ctx->s[rc4_ctx->i]) % 0x100;
    u8 temp = rc4_ctx->s[rc4_ctx->i];
    rc4_ctx->s[rc4_ctx->i] = rc4_ctx->s[rc4_ctx->j];
    rc4_ctx->s[rc4_ctx->j] = temp;

    u8 a = data[idx];
    u8 b = rc4_ctx->s[(rc4_ctx->s[rc4_ctx->i] + rc4_ctx->s[rc4_ctx->j]) % 0x100];
    data[idx] = static_cast<u8>(a ^ b);
  }
}

void XeCryptRc4_entry(MappedVoid key, u32 key_size, MappedVoid data, u32 size) {
  XECRYPT_RC4_STATE rc4_ctx;
  XeCryptRc4Key_entry(GuestPtrT<XECRYPT_RC4_STATE>::GetFromHost(&rc4_ctx), key, key_size);
  XeCryptRc4Ecb_entry(GuestPtrT<XECRYPT_RC4_STATE>::GetFromHost(&rc4_ctx), data, size);
}

struct XECRYPT_SHA_STATE {
  be_u32 count;
  be_u32 state[5];
  u8 buffer[64];
};
REX_STATIC_ASSERT_SIZE(XECRYPT_SHA_STATE, 0x58);

void InitSha1(sha1::SHA1* sha, const XECRYPT_SHA_STATE* state) {
  u32 digest[5];
  std::copy(std::begin(state->state), std::end(state->state), digest);

  sha->init(digest, state->buffer, state->count);
}

void StoreSha1(const sha1::SHA1* sha, XECRYPT_SHA_STATE* state) {
  std::copy_n(sha->getDigest(), rex::CountOf(state->state), state->state);

  state->count = static_cast<u32>(sha->getByteCount());
  std::copy_n(sha->getBlock(), sha->getBlockByteIndex(), state->buffer);
}

void XeCryptShaInit_entry(GuestPtrT<XECRYPT_SHA_STATE> sha_state) {
  sha_state.Zero();

  sha_state->state[0] = 0x67452301;
  sha_state->state[1] = 0xEFCDAB89;
  sha_state->state[2] = 0x98BADCFE;
  sha_state->state[3] = 0x10325476;
  sha_state->state[4] = 0xC3D2E1F0;
}

void XeCryptShaUpdate_entry(GuestPtrT<XECRYPT_SHA_STATE> sha_state, MappedVoid input,
                            u32 input_size) {
  sha1::SHA1 sha;
  InitSha1(&sha, sha_state);

  sha.processBytes(input, input_size);

  StoreSha1(&sha, sha_state);
}

void XeCryptShaFinal_entry(GuestPtrT<XECRYPT_SHA_STATE> sha_state, GuestPtrT<u8> out,
                           u32 out_size) {
  sha1::SHA1 sha;
  InitSha1(&sha, sha_state);

  u8 digest[0x14];
  sha.finalize(digest);

  std::copy_n(digest, std::min<size_t>(rex::CountOf(digest), out_size), static_cast<u8*>(out));
  std::copy_n(sha.getDigest(), rex::CountOf(sha_state->state), sha_state->state);
}

void XeCryptSha_entry(MappedVoid input_1, u32 input_1_size, MappedVoid input_2, u32 input_2_size,
                      MappedVoid input_3, u32 input_3_size, MappedVoid output, u32 output_size) {
  sha1::SHA1 sha;

  if (input_1 && input_1_size) {
    sha.processBytes(input_1, input_1_size);
  }
  if (input_2 && input_2_size) {
    sha.processBytes(input_2, input_2_size);
  }
  if (input_3 && input_3_size) {
    sha.processBytes(input_3, input_3_size);
  }

  u8 digest[0x14];
  sha.finalize(digest);
  std::copy_n(digest, std::min<size_t>(rex::CountOf(digest), output_size), output.As<u8*>());
}

struct XECRYPT_SHA256_STATE {
  be_u32 count;
  be_u32 state[8];
  u8 buffer[64];
};

void XeCryptSha256Init_entry(GuestPtrT<XECRYPT_SHA256_STATE> sha_state) {
  sha_state.Zero();

  sha_state->state[0] = 0x6a09e667;
  sha_state->state[1] = 0xbb67ae85;
  sha_state->state[2] = 0x3c6ef372;
  sha_state->state[3] = 0xa54ff53a;
  sha_state->state[4] = 0x510e527f;
  sha_state->state[5] = 0x9b05688c;
  sha_state->state[6] = 0x1f83d9ab;
  sha_state->state[7] = 0x5be0cd19;
}

void XeCryptSha256Update_entry(GuestPtrT<XECRYPT_SHA256_STATE> sha_state, MappedVoid input,
                               u32 input_size) {
  sha256::SHA256 sha;
  std::copy(std::begin(sha_state->state), std::end(sha_state->state), sha.getHashValues());
  std::copy(std::begin(sha_state->buffer), std::end(sha_state->buffer), sha.getBuffer());
  sha.setTotalSize(sha_state->count);

  sha.add(input, input_size);

  std::copy_n(sha.getHashValues(), rex::CountOf(sha_state->state), sha_state->state);
  std::copy_n(sha.getBuffer(), rex::CountOf(sha_state->buffer), sha_state->buffer);
  sha_state->count = static_cast<u32>(sha.getTotalSize());
}

void XeCryptSha256Final_entry(GuestPtrT<XECRYPT_SHA256_STATE> sha_state, GuestPtrT<u8> out,
                              u32 out_size) {
  sha256::SHA256 sha;
  std::copy(std::begin(sha_state->state), std::end(sha_state->state), sha.getHashValues());
  std::copy(std::begin(sha_state->buffer), std::end(sha_state->buffer), sha.getBuffer());
  sha.setTotalSize(sha_state->count);

  u8 hash[32];
  sha.getHash(hash);

  std::copy_n(hash, std::min<size_t>(rex::CountOf(hash), out_size), static_cast<u8*>(out));
  std::copy(std::begin(hash), std::end(hash), sha_state->buffer);
}

/**
 * Byteswaps each 8 bytes.
 *
 * @param qw_inp The input quadwords.
 * @param qw_out Receives the swapped quadwords.
 * @param size The count of quadwords.
 */
void XeCryptBnQw_SwapDwQwLeBe_entry(GuestPtrT<u64> qw_inp, GuestPtrT<u64> qw_out, u32 size) {
  memory::copy_and_swap<u64>(qw_out, qw_inp, size);
}

/** RSA key header, followed by the modulus and any private key data. */
struct XECRYPT_RSA {
  be_u32 size; /**< Modulus size in 8 byte units. */
  be_u32 public_exponent;
  be_u64 pad_8;
};
REX_STATIC_ASSERT_SIZE(XECRYPT_RSA, 0x10);

/**
 * Applies the RSA public key to qw_a.
 *
 * @param qw_a The input quadwords.
 * @param qw_b Receives the result.
 * @param rsa The key.
 * @return 0 on failure, which is not a BOOL return value.
 */
u32 XeCryptBnQwNeRsaPubCrypt_entry(ppc_ptr_t<u64> qw_a, ppc_ptr_t<u64> qw_b,
                                   ppc_ptr_t<XECRYPT_RSA> rsa) {
#if !REX_PLATFORM_WIN32
  REXLOG_ERROR(kernel,
               "XeCryptBnQwNeRsaPubCrypt called but no implementation available for "
               "this platform!");
  REX_ASSERT_ALWAYS();
  return 1;
#else
  u32 modulus_size = rsa->size * 8;

  ULONG key_size = sizeof(BCRYPT_RSAKEY_BLOB) + sizeof(u32) + modulus_size;
  auto key_buf = std::make_unique<u8[]>(key_size);
  auto* key_header = reinterpret_cast<BCRYPT_RSAKEY_BLOB*>(key_buf.get());

  key_header->Magic = BCRYPT_RSAPUBLIC_MAGIC;
  key_header->BitLength = modulus_size * 8;
  key_header->cbPublicExp = sizeof(u32);
  key_header->cbModulus = modulus_size;
  key_header->cbPrime1 = key_header->cbPrime2 = 0;

  u32* key_exponent = reinterpret_cast<u32*>(&key_header[1]);
  *key_exponent = rsa->public_exponent.value;

  // BCrypt needs the modulus reversed in 64 bit chunks.
  u64* key_modulus = reinterpret_cast<u64*>(&key_exponent[1]);
  u64* xecrypt_modulus = reinterpret_cast<u64*>(&rsa[1]);
  std::reverse_copy(xecrypt_modulus, xecrypt_modulus + rsa->size, key_modulus);

  BCRYPT_ALG_HANDLE h_algorithm = nullptr;
  NTSTATUS status =
      BCryptOpenAlgorithmProvider(&h_algorithm, BCRYPT_RSA_ALGORITHM, MS_PRIMITIVE_PROVIDER, 0);

  if (!BCRYPT_SUCCESS(status)) {
    REX_FAIL(kernel, 0,
             "XeCryptBnQwNeRsaPubCrypt: BCryptOpenAlgorithmProvider failed with "
             "status {:#X}!",
             status);
  }

  BCRYPT_KEY_HANDLE h_key = nullptr;
  status = BCryptImportKeyPair(h_algorithm, nullptr, BCRYPT_RSAPUBLIC_BLOB, &h_key, key_buf.get(),
                               key_size, 0);

  if (!BCRYPT_SUCCESS(status)) {
    REXLOG_ERROR(kernel,
                 "XeCryptBnQwNeRsaPubCrypt: BCryptImportKeyPair failed with status "
                 "{:#X}!",
                 status);

    if (h_algorithm) {
      BCryptCloseAlgorithmProvider(h_algorithm, 0);
    }

    return 0;
  }

  // BCrypt wants the input MSB first.
  u64* output = qw_b;
  u8* output_bytes = reinterpret_cast<u8*>(output);
  memory::copy_and_swap<u64>(output, qw_a, rsa->size);
  std::reverse(output_bytes, output_bytes + modulus_size);

  // BCryptDecrypt only works with private keys, so BCryptEncrypt performs the public decrypt.
  ULONG result_size = 0;
  status = BCryptEncrypt(h_key, output_bytes, modulus_size, nullptr, nullptr, 0, output_bytes,
                         modulus_size, &result_size, BCRYPT_PAD_NONE);

  assert(result_size == modulus_size);

  if (!BCRYPT_SUCCESS(status)) {
    REXLOG_ERROR(kernel, "XeCryptBnQwNeRsaPubCrypt: BCryptEncrypt failed with status {:#X}!",
                 status);
  } else {
    std::reverse(output_bytes, output_bytes + modulus_size);
    memory::copy_and_swap(output, output, rsa->size);
  }

  if (h_key) {
    BCryptDestroyKey(h_key);
  }
  if (h_algorithm) {
    BCryptCloseAlgorithmProvider(h_algorithm, 0);
  }

  return BCRYPT_SUCCESS(status) ? 1 : 0;
#endif
}

/**
 * Verifies a PKCS1 signature. Always reports success.
 *
 * @param hash Unused.
 * @param sig Unused.
 * @param size Unused.
 * @return 1 (BOOL TRUE).
 */
u32 XeCryptBnDwLePkcs1Verify_entry([[maybe_unused]] MappedVoid hash,
                                   [[maybe_unused]] MappedVoid sig, [[maybe_unused]] u32 size) {
  return 1;
}

void XeCryptRandom_entry(MappedVoid buf, u32 buf_size) {
  std::memset(buf, 0xFD, buf_size);
}

struct XECRYPT_DES_STATE {
  u32 keytab[16][2];
};

/**
 * Sets bit 0 of each byte to make its parity odd.
 *
 * @param inp The input key bytes.
 * @param inp_size The size of inp in bytes.
 * @param out_ptr Receives the key bytes with odd parity.
 */
void XeCryptDesParity_entry(MappedVoid inp, u32 inp_size, MappedVoid out_ptr) {
  DES::set_parity(inp, static_cast<int>(std::min<u32>(inp_size, std::numeric_limits<int>::max())),
                  out_ptr);
}

struct XECRYPT_DES3_STATE {
  XECRYPT_DES_STATE des_state[3];
};

void XeCryptDes3Key_entry(GuestPtrT<XECRYPT_DES3_STATE> state_ptr, MappedU64 key) {
  DES3 des3(key[0], key[1], key[2]);
  DES* des = des3.getDES();

  for (int i = 0; i < 3; i++) {
    std::memcpy(state_ptr->des_state[i].keytab, des[i].get_sub_key(), 128);
  }
}

void XeCryptDes3Ecb_entry(GuestPtrT<XECRYPT_DES3_STATE> state_ptr, MappedU64 inp, MappedU64 out,
                          u32 encrypt) {
  DES3 des3(reinterpret_cast<ui64*>(state_ptr->des_state[0].keytab),
            reinterpret_cast<ui64*>(state_ptr->des_state[1].keytab),
            reinterpret_cast<ui64*>(state_ptr->des_state[2].keytab));

  if (encrypt) {
    *out = des3.encrypt(*inp);
  } else {
    *out = des3.decrypt(*inp);
  }
}

void XeCryptDes3Cbc_entry(GuestPtrT<XECRYPT_DES3_STATE> state_ptr, MappedU64 inp, u32 inp_size,
                          MappedU64 out, MappedU64 feed, u32 encrypt) {
  DES3 des3(reinterpret_cast<ui64*>(state_ptr->des_state[0].keytab),
            reinterpret_cast<ui64*>(state_ptr->des_state[1].keytab),
            reinterpret_cast<ui64*>(state_ptr->des_state[2].keytab));

  REX_ASSERT(inp_size % 8 == 0);

  u64 last_block = *feed;
  for (u32 i = 0; i < inp_size / 8; i++) {
    u64 block = inp[i];
    if (encrypt) {
      last_block = des3.encrypt(block ^ last_block);
      out[i] = last_block;
    } else {
      out[i] = des3.decrypt(block) ^ last_block;
      last_block = block;
    }
  }

  *feed = last_block;
}

struct XECRYPT_AES_STATE {
  u8 keytabenc[11][4][4];
  u8 keytabdec[11][4][4];
};
REX_STATIC_ASSERT_SIZE(XECRYPT_AES_STATE, 0x160);

static inline u8 XeCryptAesMul2(u8 a) {
  return (a & 0x80) ? ((a << 1) ^ 0x1B) : (a << 1);
}

/**
 * Expands an AES 128 key. Fills the decryption key schedule of the context structure too, which
 * the encryption path does not use.
 *
 * @param state_ptr Receives the key schedules.
 * @param key The 16 byte key.
 */
void XeCryptAesKey_entry(GuestPtrT<XECRYPT_AES_STATE> state_ptr, MappedVoid key) {
  aes_key_schedule_128(key, reinterpret_cast<u8*>(state_ptr->keytabenc));
  std::memcpy(state_ptr->keytabdec[0], state_ptr->keytabenc[10], 16);
  for (u32 i = 1; i < 10; ++i) {
    const u8* enc = reinterpret_cast<const u8*>(state_ptr->keytabenc[10 - i]);
    u8* dec = reinterpret_cast<u8*>(state_ptr->keytabdec[i]);
    u8 t, u, v;
    t = enc[0] ^ enc[1] ^ enc[2] ^ enc[3];
    dec[0] = t ^ enc[0] ^ XeCryptAesMul2(enc[0] ^ enc[1]);
    dec[1] = t ^ enc[1] ^ XeCryptAesMul2(enc[1] ^ enc[2]);
    dec[2] = t ^ enc[2] ^ XeCryptAesMul2(enc[2] ^ enc[3]);
    dec[3] = t ^ enc[3] ^ XeCryptAesMul2(enc[3] ^ enc[0]);
    u = XeCryptAesMul2(XeCryptAesMul2(enc[0] ^ enc[2]));
    v = XeCryptAesMul2(XeCryptAesMul2(enc[1] ^ enc[3]));
    t = XeCryptAesMul2(u ^ v);
    dec[0] ^= t ^ u;
    dec[1] ^= t ^ v;
    dec[2] ^= t ^ u;
    dec[3] ^= t ^ v;
    t = enc[4] ^ enc[5] ^ enc[6] ^ enc[7];
    dec[4] = t ^ enc[4] ^ XeCryptAesMul2(enc[4] ^ enc[5]);
    dec[5] = t ^ enc[5] ^ XeCryptAesMul2(enc[5] ^ enc[6]);
    dec[6] = t ^ enc[6] ^ XeCryptAesMul2(enc[6] ^ enc[7]);
    dec[7] = t ^ enc[7] ^ XeCryptAesMul2(enc[7] ^ enc[4]);
    u = XeCryptAesMul2(XeCryptAesMul2(enc[4] ^ enc[6]));
    v = XeCryptAesMul2(XeCryptAesMul2(enc[5] ^ enc[7]));
    t = XeCryptAesMul2(u ^ v);
    dec[4] ^= t ^ u;
    dec[5] ^= t ^ v;
    dec[6] ^= t ^ u;
    dec[7] ^= t ^ v;
    t = enc[8] ^ enc[9] ^ enc[10] ^ enc[11];
    dec[8] = t ^ enc[8] ^ XeCryptAesMul2(enc[8] ^ enc[9]);
    dec[9] = t ^ enc[9] ^ XeCryptAesMul2(enc[9] ^ enc[10]);
    dec[10] = t ^ enc[10] ^ XeCryptAesMul2(enc[10] ^ enc[11]);
    dec[11] = t ^ enc[11] ^ XeCryptAesMul2(enc[11] ^ enc[8]);
    u = XeCryptAesMul2(XeCryptAesMul2(enc[8] ^ enc[10]));
    v = XeCryptAesMul2(XeCryptAesMul2(enc[9] ^ enc[11]));
    t = XeCryptAesMul2(u ^ v);
    dec[8] ^= t ^ u;
    dec[9] ^= t ^ v;
    dec[10] ^= t ^ u;
    dec[11] ^= t ^ v;
    t = enc[12] ^ enc[13] ^ enc[14] ^ enc[15];
    dec[12] = t ^ enc[12] ^ XeCryptAesMul2(enc[12] ^ enc[13]);
    dec[13] = t ^ enc[13] ^ XeCryptAesMul2(enc[13] ^ enc[14]);
    dec[14] = t ^ enc[14] ^ XeCryptAesMul2(enc[14] ^ enc[15]);
    dec[15] = t ^ enc[15] ^ XeCryptAesMul2(enc[15] ^ enc[12]);
    u = XeCryptAesMul2(XeCryptAesMul2(enc[12] ^ enc[14]));
    v = XeCryptAesMul2(XeCryptAesMul2(enc[13] ^ enc[15]));
    t = XeCryptAesMul2(u ^ v);
    dec[12] ^= t ^ u;
    dec[13] ^= t ^ v;
    dec[14] ^= t ^ u;
    dec[15] ^= t ^ v;
  }
  std::memcpy(state_ptr->keytabdec[10], state_ptr->keytabenc[0], 16);
}

void XeCryptAesEcb_entry(GuestPtrT<XECRYPT_AES_STATE> state_ptr, MappedVoid inp_ptr,
                         MappedVoid out_ptr, u32 encrypt) {
  const u8* keytab = reinterpret_cast<const u8*>(state_ptr->keytabenc);
  if (encrypt) {
    aes_encrypt_128(keytab, inp_ptr, out_ptr);
  } else {
    aes_decrypt_128(keytab, inp_ptr, out_ptr);
  }
}

void XeCryptAesCbc_entry(GuestPtrT<XECRYPT_AES_STATE> state_ptr, MappedVoid inp_ptr, u32 inp_size,
                         MappedVoid out_ptr, MappedVoid feed_ptr, u32 encrypt) {
  const u8* keytab = reinterpret_cast<const u8*>(state_ptr->keytabenc);
  const u8* inp = inp_ptr.As<const u8*>();
  u8* out = out_ptr.As<u8*>();
  u8* feed = feed_ptr.As<u8*>();
  if (encrypt) {
    for (u32 i = 0; i < inp_size; i += 16) {
      for (u32 j = 0; j < 16; ++j) {
        feed[j] ^= inp[j];
      }
      aes_encrypt_128(keytab, feed, feed);
      std::memcpy(out, feed, 16);
      inp += 16;
      out += 16;
    }
  } else {
    for (u32 i = 0; i < inp_size; i += 16) {
      // inp may alias out.
      u8 tmp[16];
      std::memcpy(tmp, inp, 16);
      aes_decrypt_128(keytab, inp, out);
      for (u32 j = 0; j < 16; ++j) {
        out[j] ^= feed[j];
      }
      std::memcpy(feed, tmp, 16);
      inp += 16;
      out += 16;
    }
  }
}

void XeCryptHmacSha_entry(MappedVoid key, u32 key_size_in, MappedVoid inp_1, u32 inp_1_size,
                          MappedVoid inp_2, u32 inp_2_size, MappedVoid inp_3, u32 inp_3_size,
                          MappedVoid out, u32 out_size) {
  u32 key_size = key_size_in;
  sha1::SHA1 sha;
  u8 kpad_i[0x40];
  u8 kpad_o[0x40];
  u8 tmp_key[0x40];
  std::memset(kpad_i, 0x36, 0x40);
  std::memset(kpad_o, 0x5C, 0x40);

  // A key longer than the block size is replaced by its hash.
  if (key_size > 0x40) {
    sha1::SHA1 sha_key;
    sha_key.processBytes(key, key_size);
    sha_key.finalize(tmp_key);

    key_size = 0x14u;
  } else {
    std::memcpy(tmp_key, key, key_size);
  }

  for (u32 i = 0; i < key_size; i++) {
    kpad_i[i] = tmp_key[i] ^ 0x36;
    kpad_o[i] = tmp_key[i] ^ 0x5C;
  }

  sha.processBytes(kpad_i, 0x40);

  if (inp_1_size) {
    sha.processBytes(inp_1, inp_1_size);
  }

  if (inp_2_size) {
    sha.processBytes(inp_2, inp_2_size);
  }

  if (inp_3_size) {
    sha.processBytes(inp_3, inp_3_size);
  }

  u8 digest[0x14];
  sha.finalize(digest);
  sha.reset();

  sha.processBytes(kpad_o, 0x40);
  sha.processBytes(digest, 0x14);
  sha.finalize(digest);

  std::memcpy(out, digest, std::min(out_size, 0x14u));
}

/** Retail key 0x19. */
static const u8 kKey19[] = {0xE1, 0xBC, 0x15, 0x9C, 0x73, 0xB1, 0xEA, 0xE9,
                            0xAB, 0x31, 0x70, 0xF3, 0xAD, 0x47, 0xEB, 0xF3};

u32 XeKeysHmacSha_entry(u32 key_num, MappedVoid inp_1, u32 inp_1_size, MappedVoid inp_2,
                        u32 inp_2_size, MappedVoid inp_3, u32 inp_3_size, MappedVoid out,
                        u32 out_size) {
  const u8* key = nullptr;
  if (key_num == 0x19) {
    key = kKey19;
  }

  if (key) {
    XeCryptHmacSha_entry(MappedVoid::GetFromHost(const_cast<u8*>(key)), 0x10, inp_1, inp_1_size,
                         inp_2, inp_2_size, inp_3, inp_3_size, out, out_size);

    return X_STATUS_SUCCESS;
  }

  return X_STATUS_UNSUCCESSFUL;
}

static const u8 kKeyObfuscationKey[16] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                                          0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};

u32 XeKeysAesCbcUsingKey_entry(MappedVoid obscured_key, MappedVoid inp_ptr, u32 inp_size,
                               MappedVoid out_ptr, MappedVoid feed_ptr, u32 encrypt) {
  u8 key[16];

  XECRYPT_AES_STATE aes;
  XeCryptAesKey_entry(GuestPtrT<XECRYPT_AES_STATE>::GetFromHost(&aes),
                      MappedVoid::GetFromHost(const_cast<u8*>(kKeyObfuscationKey)));
  XeCryptAesEcb_entry(GuestPtrT<XECRYPT_AES_STATE>::GetFromHost(&aes), obscured_key,
                      MappedVoid::GetFromHost(key), 0);

  XeCryptAesKey_entry(GuestPtrT<XECRYPT_AES_STATE>::GetFromHost(&aes),
                      MappedVoid::GetFromHost(key));
  XeCryptAesCbc_entry(GuestPtrT<XECRYPT_AES_STATE>::GetFromHost(&aes), inp_ptr, inp_size, out_ptr,
                      feed_ptr, encrypt);

  return X_STATUS_SUCCESS;
}

/**
 * Obscures a key by encrypting it with kKeyObfuscationKey.
 *
 * @param input The 16 byte key.
 * @param output Receives the obscured key.
 * @return X_STATUS_SUCCESS.
 */
u32 XeKeysObscureKey_entry(MappedVoid input, MappedVoid output) {
  XECRYPT_AES_STATE aes;
  XeCryptAesKey_entry(GuestPtrT<XECRYPT_AES_STATE>::GetFromHost(&aes),
                      MappedVoid::GetFromHost(const_cast<u8*>(kKeyObfuscationKey)));
  XeCryptAesEcb_entry(GuestPtrT<XECRYPT_AES_STATE>::GetFromHost(&aes), input, output, 1);

  return X_STATUS_SUCCESS;
}

u32 XeKeysHmacShaUsingKey_entry(MappedVoid obscured_key, MappedVoid inp_1, u32 inp_1_size,
                                MappedVoid inp_2, u32 inp_2_size, MappedVoid inp_3, u32 inp_3_size,
                                MappedVoid out, u32 out_size) {
  REX_KERNEL_ARG(obscured_key);

  u8 key[16];

  XECRYPT_AES_STATE aes;
  XeCryptAesKey_entry(GuestPtrT<XECRYPT_AES_STATE>::GetFromHost(&aes),
                      MappedVoid::GetFromHost(const_cast<u8*>(kKeyObfuscationKey)));
  XeCryptAesEcb_entry(GuestPtrT<XECRYPT_AES_STATE>::GetFromHost(&aes), obscured_key,
                      MappedVoid::GetFromHost(key), 0);

  XeCryptHmacSha_entry(MappedVoid::GetFromHost(key), 0x10, inp_1, inp_1_size, inp_2, inp_2_size,
                       inp_3, inp_3_size, out, out_size);
  return X_STATUS_SUCCESS;
}

u32 XeKeysConsolePrivateKeySign_entry([[maybe_unused]] MappedVoid hash,
                                      [[maybe_unused]] MappedVoid signature) {
  REX_KERNEL_STUB("XeKeysConsolePrivateKeySign");
  return 0;
}

u32 XeKeysConsoleSignatureVerification_entry([[maybe_unused]] MappedVoid hash,
                                             [[maybe_unused]] MappedVoid signature,
                                             [[maybe_unused]] MappedVoid pubkey) {
  REX_KERNEL_STUB("XeKeysConsoleSignatureVerification");
  return 0;
}

REX_EXPORT_STUB(__imp__XeKeysGetConsoleCertificate);
REX_EXPORT_STUB(__imp__XeCryptBnDwLeDhEqualBase);
REX_EXPORT_STUB(__imp__XeCryptBnDwLeDhInvalBase);
REX_EXPORT_STUB(__imp__XeCryptBnDwLeDhModExp);
REX_EXPORT_STUB(__imp__XeCryptBnDw_Copy);
REX_EXPORT_STUB(__imp__XeCryptBnDw_SwapLeBe);
REX_EXPORT_STUB(__imp__XeCryptBnDw_Zero);
REX_EXPORT_STUB(__imp__XeCryptBnDwLePkcs1Format);
REX_EXPORT_STUB(__imp__XeCryptBnQwBeSigCreate);
REX_EXPORT_STUB(__imp__XeCryptBnQwBeSigFormat);
REX_EXPORT_STUB(__imp__XeCryptBnQwBeSigVerify);
REX_EXPORT_STUB(__imp__XeCryptBnQwNeModExp);
REX_EXPORT_STUB(__imp__XeCryptBnQwNeModExpRoot);
REX_EXPORT_STUB(__imp__XeCryptBnQwNeModInv);
REX_EXPORT_STUB(__imp__XeCryptBnQwNeModMul);
REX_EXPORT_STUB(__imp__XeCryptBnQwNeRsaKeyGen);
REX_EXPORT_STUB(__imp__XeCryptBnQwNeRsaPrvCrypt);
REX_EXPORT_STUB(__imp__XeCryptBnQw_Copy);
REX_EXPORT_STUB(__imp__XeCryptBnQw_SwapDwQw);
REX_EXPORT_STUB(__imp__XeCryptBnQw_SwapLeBe);
REX_EXPORT_STUB(__imp__XeCryptBnQw_Zero);
REX_EXPORT_STUB(__imp__XeCryptChainAndSumMac);
REX_EXPORT_STUB(__imp__XeCryptDesKey);
REX_EXPORT_STUB(__imp__XeCryptDesEcb);
REX_EXPORT_STUB(__imp__XeCryptDesCbc);
REX_EXPORT_STUB(__imp__XeCryptHmacMd5Init);
REX_EXPORT_STUB(__imp__XeCryptHmacMd5Update);
REX_EXPORT_STUB(__imp__XeCryptHmacMd5Final);
REX_EXPORT_STUB(__imp__XeCryptHmacMd5);
REX_EXPORT_STUB(__imp__XeCryptHmacShaInit);
REX_EXPORT_STUB(__imp__XeCryptHmacShaUpdate);
REX_EXPORT_STUB(__imp__XeCryptHmacShaFinal);
REX_EXPORT_STUB(__imp__XeCryptHmacShaVerify);
REX_EXPORT_STUB(__imp__XeCryptMd5Init);
REX_EXPORT_STUB(__imp__XeCryptMd5Update);
REX_EXPORT_STUB(__imp__XeCryptMd5Final);
REX_EXPORT_STUB(__imp__XeCryptMd5);
REX_EXPORT_STUB(__imp__XeCryptParveEcb);
REX_EXPORT_STUB(__imp__XeCryptParveCbcMac);
REX_EXPORT_STUB(__imp__XeCryptRotSumSha);
REX_EXPORT_STUB(__imp__XeCryptSha256);
REX_EXPORT_STUB(__imp__XeCryptSha384Init);
REX_EXPORT_STUB(__imp__XeCryptSha384Update);
REX_EXPORT_STUB(__imp__XeCryptSha384Final);
REX_EXPORT_STUB(__imp__XeCryptSha384);
REX_EXPORT_STUB(__imp__XeCryptSha512Init);
REX_EXPORT_STUB(__imp__XeCryptSha512Update);
REX_EXPORT_STUB(__imp__XeCryptSha512Final);
REX_EXPORT_STUB(__imp__XeCryptSha512);
REX_EXPORT_STUB(__imp__XeCryptBnQwNeCompare);
REX_EXPORT_STUB(__imp__XeKeysGetFactoryChallenge);
REX_EXPORT_STUB(__imp__XeKeysSetFactoryResponse);
REX_EXPORT_STUB(__imp__XeKeysInitializeFuses);
REX_EXPORT_STUB(__imp__XeKeysSaveBootLoader);
REX_EXPORT_STUB(__imp__XeKeysSaveKeyVault);
REX_EXPORT_STUB(__imp__XeKeysGetStatus);
REX_EXPORT_STUB(__imp__XeKeysGeneratePrivateKey);
REX_EXPORT_STUB(__imp__XeKeysGetKeyProperties);
REX_EXPORT_STUB(__imp__XeKeysSetKey);
REX_EXPORT_STUB(__imp__XeKeysGenerateRandomKey);
REX_EXPORT_STUB(__imp__XeKeysGetKey);
REX_EXPORT_STUB(__imp__XeKeysGetDigest);
REX_EXPORT_STUB(__imp__XeKeysGetConsoleID);
REX_EXPORT_STUB(__imp__XeKeysGetConsoleType);
REX_EXPORT_STUB(__imp__XeKeysQwNeRsaPrvCrypt);
REX_EXPORT_STUB(__imp__XeKeysAesCbc);
REX_EXPORT_STUB(__imp__XeKeysDes2Cbc);
REX_EXPORT_STUB(__imp__XeKeysDesCbc);
REX_EXPORT_STUB(__imp__XeKeysSaveBootLoaderEx);
REX_EXPORT_STUB(__imp__XeKeysDes2CbcUsingKey);
REX_EXPORT_STUB(__imp__XeKeysDesCbcUsingKey);
REX_EXPORT_STUB(__imp__XeKeysObfuscate);
REX_EXPORT_STUB(__imp__XeKeysUnObfuscate);
REX_EXPORT_STUB(__imp__XeKeysVerifyRSASignature);
REX_EXPORT_STUB(__imp__XeKeysSaveSystemUpdate);
REX_EXPORT_STUB(__imp__XeKeysLockSystemUpdate);
REX_EXPORT_STUB(__imp__XeKeysExecute);
REX_EXPORT_STUB(__imp__XeKeysGetVersions);
REX_EXPORT_STUB(__imp__XeKeysSetRevocationList);
REX_EXPORT_STUB(__imp__XeKeysExSaveKeyVault);
REX_EXPORT_STUB(__imp__XeKeysExSetKey);
REX_EXPORT_STUB(__imp__XeKeysExGetKey);
REX_EXPORT_STUB(__imp__XeKeysSecurityInitialize);
REX_EXPORT_STUB(__imp__XeKeysSecurityLoadSettings);
REX_EXPORT_STUB(__imp__XeKeysSecuritySaveSettings);
REX_EXPORT_STUB(__imp__XeKeysSecuritySetDetected);
REX_EXPORT_STUB(__imp__XeKeysSecurityGetDetected);
REX_EXPORT_STUB(__imp__XeKeysSecuritySetActivated);
REX_EXPORT_STUB(__imp__XeKeysSecurityGetActivated);
REX_EXPORT_STUB(__imp__XeKeysDvdAuthAP25InstallTable);
REX_EXPORT_STUB(__imp__XeKeysDvdAuthAP25GetTableVersion);
REX_EXPORT_STUB(__imp__XeKeysGetProtectedFlag);
REX_EXPORT_STUB(__imp__XeKeysSetProtectedFlag);
REX_EXPORT_STUB(__imp__XeKeysGetUpdateSequence);
REX_EXPORT_STUB(__imp__XeKeysDvdAuthExActivate);
REX_EXPORT_STUB(__imp__XeKeysRevokeSaveSettings);
REX_EXPORT_STUB(__imp__XeKeysGetMediaID);
REX_EXPORT_STUB(__imp__XeKeysLoadKeyVault);
REX_EXPORT_STUB(__imp__XeKeysRevokeUpdateDynamic);
REX_EXPORT_STUB(__imp__XeKeysSecuritySetStat);
REX_EXPORT_STUB(__imp__XeKeysFcrtLoad);
REX_EXPORT_STUB(__imp__XeKeysFcrtSave);
REX_EXPORT_STUB(__imp__XeKeysFcrtSet);
REX_EXPORT_STUB(__imp__XeKeysRevokeIsDeviceRevoked);
REX_EXPORT_STUB(__imp__XeKeysDvdAuthExSave);
REX_EXPORT_STUB(__imp__XeKeysDvdAuthExInstall);
REX_EXPORT_STUB(__imp__XeKeysObfuscateEx);
REX_EXPORT_STUB(__imp__XeKeysUnObfuscateEx);
REX_EXPORT_STUB(__imp__XeKeysVerifyPIRSSignature);
REX_EXPORT_STUB(__imp__XeCryptAesCtr);
REX_EXPORT_STUB(__imp__XeCryptAesCbcMac);
REX_EXPORT_STUB(__imp__XeCryptAesDmMac);
REX_EXPORT_STUB(__imp__XeKeysGetTruncatedSecondaryConsoleId);
REX_EXPORT_STUB(__imp__XeCryptSha224Init);
REX_EXPORT_STUB(__imp__XeCryptAesCreateKeySchedule);
REX_EXPORT_STUB(__imp__XeCryptAesEncryptOne);
REX_EXPORT_STUB(__imp__XeCryptAesDecryptOne);
REX_EXPORT_STUB(__imp__XeCryptAesCbcEncrypt);
REX_EXPORT_STUB(__imp__XeCryptAesCbcDecrypt);
REX_EXPORT_STUB(__imp__XeCryptAesGcmInitialize);
REX_EXPORT_STUB(__imp__XeCryptAesGcmUpdate);
REX_EXPORT_STUB(__imp__XeCryptAesGcmFinalize);
REX_EXPORT_STUB(__imp__XeCryptEccGetCurveParameters);
REX_EXPORT_STUB(__imp__XeCryptEccEcdhGenerateKeypair);
REX_EXPORT_STUB(__imp__XeCryptEccEcdhExponentiate);
REX_EXPORT_STUB(__imp__XeCryptEccEcdsaGenerateSignature);
REX_EXPORT_STUB(__imp__XeCryptEccEcdsaVerifySignature);

}  // namespace rex::kernel::xboxkrnl

REX_EXPORT(__imp__XeCryptRc4Key, rex::kernel::xboxkrnl::XeCryptRc4Key_entry)
REX_EXPORT(__imp__XeCryptRc4Ecb, rex::kernel::xboxkrnl::XeCryptRc4Ecb_entry)
REX_EXPORT(__imp__XeCryptRc4, rex::kernel::xboxkrnl::XeCryptRc4_entry)
REX_EXPORT(__imp__XeCryptShaInit, rex::kernel::xboxkrnl::XeCryptShaInit_entry)
REX_EXPORT(__imp__XeCryptShaUpdate, rex::kernel::xboxkrnl::XeCryptShaUpdate_entry)
REX_EXPORT(__imp__XeCryptShaFinal, rex::kernel::xboxkrnl::XeCryptShaFinal_entry)
REX_EXPORT(__imp__XeCryptSha, rex::kernel::xboxkrnl::XeCryptSha_entry)
REX_EXPORT(__imp__XeCryptSha256Init, rex::kernel::xboxkrnl::XeCryptSha256Init_entry)
REX_EXPORT(__imp__XeCryptSha256Update, rex::kernel::xboxkrnl::XeCryptSha256Update_entry)
REX_EXPORT(__imp__XeCryptSha256Final, rex::kernel::xboxkrnl::XeCryptSha256Final_entry)
REX_EXPORT(__imp__XeCryptBnQw_SwapDwQwLeBe, rex::kernel::xboxkrnl::XeCryptBnQw_SwapDwQwLeBe_entry)
REX_EXPORT(__imp__XeCryptBnQwNeRsaPubCrypt, rex::kernel::xboxkrnl::XeCryptBnQwNeRsaPubCrypt_entry)
REX_EXPORT(__imp__XeCryptBnDwLePkcs1Verify, rex::kernel::xboxkrnl::XeCryptBnDwLePkcs1Verify_entry)
REX_EXPORT(__imp__XeCryptRandom, rex::kernel::xboxkrnl::XeCryptRandom_entry)
REX_EXPORT(__imp__XeCryptDesParity, rex::kernel::xboxkrnl::XeCryptDesParity_entry)
REX_EXPORT(__imp__XeCryptDes3Key, rex::kernel::xboxkrnl::XeCryptDes3Key_entry)
REX_EXPORT(__imp__XeCryptDes3Ecb, rex::kernel::xboxkrnl::XeCryptDes3Ecb_entry)
REX_EXPORT(__imp__XeCryptDes3Cbc, rex::kernel::xboxkrnl::XeCryptDes3Cbc_entry)
REX_EXPORT(__imp__XeCryptAesKey, rex::kernel::xboxkrnl::XeCryptAesKey_entry)
REX_EXPORT(__imp__XeCryptAesEcb, rex::kernel::xboxkrnl::XeCryptAesEcb_entry)
REX_EXPORT(__imp__XeCryptAesCbc, rex::kernel::xboxkrnl::XeCryptAesCbc_entry)
REX_EXPORT(__imp__XeCryptHmacSha, rex::kernel::xboxkrnl::XeCryptHmacSha_entry)
REX_EXPORT(__imp__XeKeysHmacSha, rex::kernel::xboxkrnl::XeKeysHmacSha_entry)
REX_EXPORT(__imp__XeKeysAesCbcUsingKey, rex::kernel::xboxkrnl::XeKeysAesCbcUsingKey_entry)
REX_EXPORT(__imp__XeKeysObscureKey, rex::kernel::xboxkrnl::XeKeysObscureKey_entry)
REX_EXPORT(__imp__XeKeysHmacShaUsingKey, rex::kernel::xboxkrnl::XeKeysHmacShaUsingKey_entry)
REX_EXPORT(__imp__XeKeysConsolePrivateKeySign,
           rex::kernel::xboxkrnl::XeKeysConsolePrivateKeySign_entry)
REX_EXPORT(__imp__XeKeysConsoleSignatureVerification,
           rex::kernel::xboxkrnl::XeKeysConsoleSignatureVerification_entry)

REX_EXPORT_STUB(__imp__DevAuthGetStatistics);
REX_EXPORT_STUB(__imp__DevAuthShouldAlwaysEnforce);
