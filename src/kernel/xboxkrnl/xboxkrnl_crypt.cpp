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
#include <array>
#include <cstring>
#include <vector>

#include <rex/logging/log.h>
#include <rex/memory/mapped_ptr.h>
#include <rex/platform.h>
#include <rex/ppc/hook.h>
#include <rex/types.h>
#include <rex/kernel/title_runtime.h>
#include <rex/kernel/types/thread.h>
#include <rex/kernel/types/status.h>
#include "kernel/argument_check.h"
#include "kernel/stub_log.h"

#include "kernel/title_state.h"
#include "kernel/kernel.h"
#include "excrypt.h"

extern "C" {
#include "rijndael.h"
}

namespace rex::kernel::xboxkrnl {

struct XECRYPT_RC4_STATE {
  u8 s[256];
  u8 i;
  u8 j;
};
REX_STATIC_ASSERT_SIZE(XECRYPT_RC4_STATE, 0x102);

void ToHost(const XECRYPT_RC4_STATE& title, EXCRYPT_RC4_STATE& host) {
  std::copy(std::begin(title.s), std::end(title.s), host.S);
  host.i = title.i;
  host.j = title.j;
}

void ToGuest(const EXCRYPT_RC4_STATE& host, XECRYPT_RC4_STATE& title) {
  std::copy(std::begin(host.S), std::end(host.S), title.s);
  title.i = host.i;
  title.j = host.j;
}

void XeCryptRc4Key_entry(GuestPtrT<XECRYPT_RC4_STATE> rc4_ctx, MappedVoid key, u32 key_size) {
  EXCRYPT_RC4_STATE state;
  ExCryptRc4Key(&state, key.As<const u8*>(), key_size);
  ToGuest(state, *rc4_ctx);
}

void XeCryptRc4Ecb_entry(GuestPtrT<XECRYPT_RC4_STATE> rc4_ctx, MappedVoid data, u32 size) {
  EXCRYPT_RC4_STATE state;
  ToHost(*rc4_ctx, state);
  ExCryptRc4Ecb(&state, data.As<u8*>(), size);
  ToGuest(state, *rc4_ctx);
}

void XeCryptRc4_entry(MappedVoid key, u32 key_size, MappedVoid data, u32 size) {
  ExCryptRc4(key.As<const u8*>(), key_size, data.As<u8*>(), size);
}

struct XECRYPT_SHA_STATE {
  be_u32 count;
  be_u32 state[5];
  u8 buffer[64];
};
REX_STATIC_ASSERT_SIZE(XECRYPT_SHA_STATE, 0x58);

struct XECRYPT_SHA256_STATE {
  be_u32 count;
  be_u32 state[8];
  u8 buffer[64];
};
REX_STATIC_ASSERT_SIZE(XECRYPT_SHA256_STATE, 0x64);

/** SHA-384 and SHA-512 state in guest memory. */
struct XECRYPT_SHA512_STATE {
  be_u32 count;
  u32 reserved;
  be_u64 state[8];
  u8 buffer[128];
};
REX_STATIC_ASSERT_SIZE(XECRYPT_SHA512_STATE, 0xC8);

/** HMAC-SHA state in guest memory, the inner and the outer SHA-1 state. */
struct XECRYPT_HMACSHA_STATE {
  XECRYPT_SHA_STATE sha[2];
};
REX_STATIC_ASSERT_SIZE(XECRYPT_HMACSHA_STATE, 0xB0);

template <typename GuestState, typename HostState>
  requires requires(HostState host) {
    host.state;
    host.buffer;
  }
void ToHost(const GuestState& title, HostState& host) {
  host.count = title.count;
  std::copy(std::begin(title.state), std::end(title.state), host.state);
  std::copy(std::begin(title.buffer), std::end(title.buffer), host.buffer);
}

template <typename HostState, typename GuestState>
  requires requires(HostState host) {
    host.state;
    host.buffer;
  }
void ToGuest(const HostState& host, GuestState& title) {
  title.count = host.count;
  std::copy(std::begin(host.state), std::end(host.state), title.state);
  std::copy_n(host.buffer, host.count % sizeof(host.buffer), title.buffer);
}

void ToHost(const XECRYPT_SHA256_STATE& title, EXCRYPT_SHA256_STATE& host) {
  host.count = title.count * 8;
  std::copy(std::begin(title.state), std::end(title.state), host.hash);
  std::memcpy(host.wbuf, title.buffer, sizeof(title.buffer));
}

void ToGuest(const EXCRYPT_SHA256_STATE& host, XECRYPT_SHA256_STATE& title) {
  title.count = host.count / 8;
  std::copy(std::begin(host.hash), std::end(host.hash), title.state);
  std::memcpy(title.buffer, host.wbuf, title.count % sizeof(title.buffer));
}

void ToHost(const XECRYPT_SHA512_STATE& title, EXCRYPT_SHA512_STATE& host) {
  host.count = u64{title.count} * 8;
  std::copy(std::begin(title.state), std::end(title.state), host.hash);
  std::memcpy(host.wbuf, title.buffer, sizeof(title.buffer));
}

void ToGuest(const EXCRYPT_SHA512_STATE& host, XECRYPT_SHA512_STATE& title) {
  title.count = static_cast<u32>(host.count / 8);
  std::copy(std::begin(host.hash), std::end(host.hash), title.state);
  std::memcpy(title.buffer, host.wbuf, title.count % sizeof(title.buffer));
}

void ToHost(const XECRYPT_HMACSHA_STATE& title, EXCRYPT_HMACSHA_STATE& host) {
  ToHost(title.sha[0], host.ShaState[0]);
  ToHost(title.sha[1], host.ShaState[1]);
}

void ToGuest(const EXCRYPT_HMACSHA_STATE& host, XECRYPT_HMACSHA_STATE& title) {
  ToGuest(host.ShaState[0], title.sha[0]);
  ToGuest(host.ShaState[1], title.sha[1]);
}

void XeCryptShaInit_entry(GuestPtrT<XECRYPT_SHA_STATE> sha_state) {
  EXCRYPT_SHA_STATE state{};
  ExCryptShaInit(&state);
  ToGuest(state, *sha_state);
}

void XeCryptShaUpdate_entry(GuestPtrT<XECRYPT_SHA_STATE> sha_state, MappedVoid input,
                            u32 input_size) {
  EXCRYPT_SHA_STATE state;
  ToHost(*sha_state, state);
  ExCryptShaUpdate(&state, input.As<const u8*>(), input_size);
  ToGuest(state, *sha_state);
}

void XeCryptShaFinal_entry(GuestPtrT<XECRYPT_SHA_STATE> sha_state, MappedVoid out, u32 out_size) {
  EXCRYPT_SHA_STATE state;
  ToHost(*sha_state, state);
  ExCryptShaFinal(&state, out.As<u8*>(), out_size);
  ToGuest(state, *sha_state);
}

void XeCryptSha_entry(MappedVoid input_1, u32 input_1_size, MappedVoid input_2, u32 input_2_size,
                      MappedVoid input_3, u32 input_3_size, MappedVoid output, u32 output_size) {
  ExCryptSha(input_1.As<const u8*>(), input_1_size, input_2.As<const u8*>(), input_2_size,
             input_3.As<const u8*>(), input_3_size, output.As<u8*>(), output_size);
}

void XeCryptSha256Init_entry(GuestPtrT<XECRYPT_SHA256_STATE> sha_state) {
  EXCRYPT_SHA256_STATE state{};
  ExCryptSha256Init(&state);
  ToGuest(state, *sha_state);
}

void XeCryptSha256Update_entry(GuestPtrT<XECRYPT_SHA256_STATE> sha_state, MappedVoid input,
                               u32 input_size) {
  EXCRYPT_SHA256_STATE state;
  ToHost(*sha_state, state);
  ExCryptSha256Update(&state, input.As<const u8*>(), input_size);
  ToGuest(state, *sha_state);
}

void XeCryptSha256Final_entry(GuestPtrT<XECRYPT_SHA256_STATE> sha_state, MappedVoid out,
                              u32 out_size) {
  EXCRYPT_SHA256_STATE state;
  ToHost(*sha_state, state);
  ExCryptSha256Final(&state, out.As<u8*>(), out_size);
  ToGuest(state, *sha_state);
}

void XeCryptSha256_entry(MappedVoid input_1, u32 input_1_size, MappedVoid input_2, u32 input_2_size,
                         MappedVoid input_3, u32 input_3_size, MappedVoid output, u32 output_size) {
  ExCryptSha256(input_1.As<const u8*>(), input_1_size, input_2.As<const u8*>(), input_2_size,
                input_3.As<const u8*>(), input_3_size, output.As<u8*>(), output_size);
}

void XeCryptSha384Init_entry(GuestPtrT<XECRYPT_SHA512_STATE> sha_state) {
  EXCRYPT_SHA384_STATE state{};
  ExCryptSha384Init(&state);
  ToGuest(state, *sha_state);
}

void XeCryptSha384Update_entry(GuestPtrT<XECRYPT_SHA512_STATE> sha_state, MappedVoid input,
                               u32 input_size) {
  EXCRYPT_SHA384_STATE state;
  ToHost(*sha_state, state);
  ExCryptSha384Update(&state, input.As<const u8*>(), input_size);
  ToGuest(state, *sha_state);
}

void XeCryptSha384Final_entry(GuestPtrT<XECRYPT_SHA512_STATE> sha_state, MappedVoid out,
                              u32 out_size) {
  EXCRYPT_SHA384_STATE state;
  ToHost(*sha_state, state);
  ExCryptSha384Final(&state, out.As<u8*>(), out_size);
  ToGuest(state, *sha_state);
}

void XeCryptSha384_entry(MappedVoid input_1, u32 input_1_size, MappedVoid input_2, u32 input_2_size,
                         MappedVoid input_3, u32 input_3_size, MappedVoid output, u32 output_size) {
  ExCryptSha384(input_1.As<const u8*>(), input_1_size, input_2.As<const u8*>(), input_2_size,
                input_3.As<const u8*>(), input_3_size, output.As<u8*>(), output_size);
}

void XeCryptSha512Init_entry(GuestPtrT<XECRYPT_SHA512_STATE> sha_state) {
  EXCRYPT_SHA512_STATE state{};
  ExCryptSha512Init(&state);
  ToGuest(state, *sha_state);
}

void XeCryptSha512Update_entry(GuestPtrT<XECRYPT_SHA512_STATE> sha_state, MappedVoid input,
                               u32 input_size) {
  EXCRYPT_SHA512_STATE state;
  ToHost(*sha_state, state);
  ExCryptSha512Update(&state, input.As<const u8*>(), input_size);
  ToGuest(state, *sha_state);
}

void XeCryptSha512Final_entry(GuestPtrT<XECRYPT_SHA512_STATE> sha_state, MappedVoid out,
                              u32 out_size) {
  EXCRYPT_SHA512_STATE state;
  ToHost(*sha_state, state);
  ExCryptSha512Final(&state, out.As<u8*>(), out_size);
  ToGuest(state, *sha_state);
}

void XeCryptSha512_entry(MappedVoid input_1, u32 input_1_size, MappedVoid input_2, u32 input_2_size,
                         MappedVoid input_3, u32 input_3_size, MappedVoid output, u32 output_size) {
  ExCryptSha512(input_1.As<const u8*>(), input_1_size, input_2.As<const u8*>(), input_2_size,
                input_3.As<const u8*>(), input_3_size, output.As<u8*>(), output_size);
}

/**
 * Byte swaps quadwords from qw_inp into qw_out.
 *
 * @param qw_out Receives the swapped quadwords.
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

namespace {

using Number = std::vector<u64>;

constexpr u32 kBignumMaxDigits = 0x40;

Number LoadNumber(const be_u64* source, u32 count) {
  Number number(count);
  for (u32 i = 0; i < count; ++i) {
    number[i] = source[i];
  }
  return number;
}

void StoreNumber(const Number& number, be_u64* destination) {
  for (u32 i = 0; i < number.size(); ++i) {
    destination[i] = number[i];
  }
}

/**
 * Compares two numbers from the top quadword down.
 *
 * @return 1 when first is greater, -1 when it is less, otherwise 0.
 */
i32 CompareNumbers(const Number& first, const Number& second) {
  const auto order = std::lexicographical_compare_three_way(first.rbegin(), first.rend(),
                                                            second.rbegin(), second.rend());
  return order > 0 ? 1 : (order < 0 ? -1 : 0);
}

/**
 * Subtracts amount from value in place.
 *
 * @param value The minuend, receives the difference.
 * @param amount The subtrahend.
 * @return True when the subtraction borrowed.
 */
bool SubtractNumber(Number& value, const Number& amount) {
  u64 borrow = 0;
  for (size_t i = 0; i < value.size(); ++i) {
    const u64 before = value[i];
    value[i] = before - amount[i] - borrow;
    borrow = (before < amount[i] || (before == amount[i] && borrow)) ? 1 : 0;
  }
  return borrow != 0;
}

/**
 * Returns first * second * R^-1 modulo the odd modulus, with R = 2^(64 * size).
 *
 * @param first The first factor.
 * @param second The second factor.
 * @param modulus The odd modulus.
 * @param inverse The inverse of the low modulus quadword modulo 2^64.
 */
Number MontgomeryMultiply(const Number& first, const Number& second, const Number& modulus,
                          u64 inverse) {
  using U128 = unsigned __int128;
  const size_t size = modulus.size();
  const u64 negated_inverse = 0 - inverse;
  Number accumulator(size + 2, 0);
  for (size_t i = 0; i < size; ++i) {
    u64 carry = 0;
    for (size_t j = 0; j < size; ++j) {
      const U128 sum = U128{accumulator[j]} + U128{first[j]} * second[i] + carry;
      accumulator[j] = static_cast<u64>(sum);
      carry = static_cast<u64>(sum >> 64);
    }
    const U128 top = U128{accumulator[size]} + carry;
    accumulator[size] = static_cast<u64>(top);
    accumulator[size + 1] = static_cast<u64>(top >> 64);

    const u64 factor = accumulator[0] * negated_inverse;
    carry = static_cast<u64>((U128{accumulator[0]} + U128{factor} * modulus[0]) >> 64);
    for (size_t j = 1; j < size; ++j) {
      const U128 sum = U128{accumulator[j]} + U128{factor} * modulus[j] + carry;
      accumulator[j - 1] = static_cast<u64>(sum);
      carry = static_cast<u64>(sum >> 64);
    }
    const U128 high = U128{accumulator[size]} + carry;
    accumulator[size - 1] = static_cast<u64>(high);
    accumulator[size] = accumulator[size + 1] + static_cast<u64>(high >> 64);
  }
  const bool overflow = accumulator[size] != 0;
  accumulator.resize(size);
  if (overflow || CompareNumbers(accumulator, modulus) >= 0) {
    SubtractNumber(accumulator, modulus);
  }
  return accumulator;
}

constexpr u32 kRsaMinDigits = 2;

/**
 * Adds amount to value in place.
 *
 * @param value The first addend, receives the sum.
 * @param amount The second addend.
 * @return True when the sum carried out of the top quadword.
 */
bool AddNumber(Number& value, const Number& amount) {
  u64 carry = 0;
  for (size_t i = 0; i < value.size(); ++i) {
    const u64 sum = value[i] + amount[i] + carry;
    carry = (sum < value[i] || (sum == value[i] && carry)) ? 1 : 0;
    value[i] = sum;
  }
  return carry != 0;
}

Number ReduceNumber(const Number& value, const Number& modulus) {
  Number remainder(modulus.size(), 0);
  for (size_t bit = value.size() * 64; bit-- > 0;) {
    u64 carry = (value[bit / 64] >> (bit % 64)) & 1;
    for (u64& word : remainder) {
      const u64 next = word >> 63;
      word = (word << 1) | carry;
      carry = next;
    }
    if (carry || CompareNumbers(remainder, modulus) >= 0) {
      SubtractNumber(remainder, modulus);
    }
  }
  return remainder;
}

/** Modular arithmetic for an odd modulus in Montgomery form. */
class Montgomery {
 public:
  explicit Montgomery(Number modulus)
      : modulus_(std::move(modulus)), inverse_(ExCryptBnQwNeModInv(modulus_[0])) {
    Number radix_squared(modulus_.size() * 2 + 1, 0);
    radix_squared.back() = 1;
    radix_squared_ = ReduceNumber(radix_squared, modulus_);
  }

  Number Multiply(const Number& first, const Number& second) const {
    return MontgomeryMultiply(first, second, modulus_, inverse_);
  }

  Number Power(const Number& base, const Number& exponent) const {
    const Number base_form = Multiply(base, radix_squared_);
    Number one(modulus_.size(), 0);
    one[0] = 1;
    Number result = Multiply(one, radix_squared_);
    for (size_t bit = exponent.size() * 64; bit-- > 0;) {
      result = Multiply(result, result);
      if ((exponent[bit / 64] >> (bit % 64)) & 1) {
        result = Multiply(result, base_form);
      }
    }
    return Multiply(result, one);
  }

  Number MultiplyModulo(const Number& first, const Number& second) const {
    return Multiply(Multiply(first, second), radix_squared_);
  }

 private:
  Number modulus_;
  u64 inverse_;
  Number radix_squared_;
};

bool IsRsaSizeValid(const XECRYPT_RSA& rsa) {
  return rsa.size >= kRsaMinDigits && rsa.size <= kBignumMaxDigits;
}

}  // namespace

/**
 * Applies the RSA public key to qw_a.
 *
 * @param qw_a The input quadwords, least significant first.
 * @param qw_b Receives the result, left untouched on failure.
 * @param rsa The key, a header followed by the modulus.
 * @return 1 on success, 0 when the modulus size is not 2 to 64 quadwords, the modulus is even or
 * the input is not below the modulus.
 */
u32 XeCryptBnQwNeRsaPubCrypt_entry(MappedU64 qw_a, MappedU64 qw_b, GuestPtrT<XECRYPT_RSA> rsa) {
  if (!IsRsaSizeValid(*rsa)) {
    return 0;
  }
  const u32 digits = rsa->size;
  const Number modulus = LoadNumber(reinterpret_cast<const be_u64*>(&rsa[1]), digits);
  const Number input = LoadNumber(qw_a, digits);
  if (!(modulus[0] & 1) || CompareNumbers(input, modulus) >= 0) {
    return 0;
  }
  const Montgomery context(modulus);
  StoreNumber(context.Power(input, Number{u32{rsa->public_exponent}}), qw_b);
  return 1;
}

/**
 * Applies the RSA private key to qw_a through the Chinese remainder theorem. An input not below the
 * modulus still produces a result.
 *
 * @param qw_a The input quadwords, least significant first.
 * @param qw_b Receives the result.
 * @param rsa The key, a header then the modulus, p, q, dp, dq and qinv.
 * @return 1 on success, otherwise 0.
 */
u32 XeCryptBnQwNeRsaPrvCrypt_entry(MappedU64 qw_a, MappedU64 qw_b, GuestPtrT<XECRYPT_RSA> rsa) {
  if (!IsRsaSizeValid(*rsa) || rsa->size % 2 != 0) {
    return 0;
  }
  const u32 digits = rsa->size;
  const u32 half = digits / 2;
  const be_u64* fields = reinterpret_cast<const be_u64*>(&rsa[1]);
  const Number modulus = LoadNumber(fields, digits);
  const Number prime_p = LoadNumber(fields + digits, half);
  const Number prime_q = LoadNumber(fields + digits + half, half);
  const Number exponent_p = LoadNumber(fields + digits + static_cast<size_t>(half) * 2, half);
  const Number exponent_q = LoadNumber(fields + digits + static_cast<size_t>(half) * 3, half);
  const Number coefficient = LoadNumber(fields + digits + static_cast<size_t>(half) * 4, half);
  if (!(modulus[0] & 1) || !(prime_p[0] & 1) || !(prime_q[0] & 1)) {
    return 0;
  }
  const Number input = LoadNumber(qw_a, digits);
  const Montgomery context_n(modulus);
  const Montgomery context_p(prime_p);
  const Montgomery context_q(prime_q);

  const Number residue_p = context_p.Power(ReduceNumber(input, prime_p), exponent_p);
  Number residue_q = context_q.Power(ReduceNumber(input, prime_q), exponent_q);
  Number difference = residue_p;
  Number residue_q_in_p = ReduceNumber(residue_q, prime_p);
  if (SubtractNumber(difference, residue_q_in_p)) {
    AddNumber(difference, prime_p);
  }
  const Number factor = context_p.MultiplyModulo(difference, ReduceNumber(coefficient, prime_p));

  Number factor_wide = factor;
  factor_wide.resize(digits, 0);
  Number prime_q_wide = prime_q;
  prime_q_wide.resize(digits, 0);
  Number result = context_n.MultiplyModulo(factor_wide, prime_q_wide);
  residue_q.resize(digits, 0);
  AddNumber(result, residue_q);
  StoreNumber(result, qw_b);
  return 1;
}

/**
 * Verifies a PKCS1 signature block against a hash.
 *
 * @param hash The 20 byte hash.
 * @param sig The signature block.
 * @param size The block size in bytes, 39 to 512.
 * @return 1 when the block is the PKCS1 block of the hash, otherwise 0.
 */
u32 XeCryptBnDwLePkcs1Verify_entry(MappedVoid hash, MappedVoid sig, u32 size) {
  return ExCryptBnDwLePkcs1Verify(hash.As<const u8*>(), sig.As<const u8*>(), size) ? 1 : 0;
}

struct XECRYPT_MD5_STATE {
  be_u32 count;
  be_u32 state[4];
  u8 buffer[64];
};
REX_STATIC_ASSERT_SIZE(XECRYPT_MD5_STATE, 0x54);

/** HMAC-MD5 state in guest memory, the inner and the outer MD5 state. */
struct XECRYPT_HMACMD5_STATE {
  XECRYPT_MD5_STATE md5[2];
};
REX_STATIC_ASSERT_SIZE(XECRYPT_HMACMD5_STATE, 0xA8);

constexpr u32 kMD5DigestSize = 16;

void ToHost(const XECRYPT_HMACMD5_STATE& title, EXCRYPT_HMACMD5_STATE& host) {
  ToHost(title.md5[0], host.Md5State[0]);
  ToHost(title.md5[1], host.Md5State[1]);
}

void ToGuest(const EXCRYPT_HMACMD5_STATE& host, XECRYPT_HMACMD5_STATE& title) {
  ToGuest(host.Md5State[0], title.md5[0]);
  ToGuest(host.Md5State[1], title.md5[1]);
}

/**
 * Pads and finishes an MD5 state.
 *
 * @return The 16 byte digest.
 */
std::array<u8, kMD5DigestSize> FinishMD5(EXCRYPT_MD5_STATE& state) {
  static constexpr std::array<u8, 64> kPadding = {0x80};
  const u64 bit_count = u64{state.count} * 8;
  const u32 padding_size = 1 + (119 - state.count % 64) % 64;
  ExCryptMd5Update(&state, kPadding.data(), padding_size);
  std::array<u8, 8> length;
  for (u32 i = 0; i < length.size(); ++i) {
    length[i] = static_cast<u8>(bit_count >> (8 * i));
  }
  ExCryptMd5Update(&state, length.data(), length.size());
  std::array<u8, kMD5DigestSize> digest;
  for (u32 i = 0; i < digest.size(); ++i) {
    digest[i] = static_cast<u8>(state.state[i / 4] >> (8 * (i % 4)));
  }
  return digest;
}

void FinishHmacMd5(EXCRYPT_HMACMD5_STATE& state, u8* output, u32 output_size) {
  const auto inner = FinishMD5(state.Md5State[0]);
  ExCryptMd5Update(&state.Md5State[1], inner.data(), inner.size());
  const auto digest = FinishMD5(state.Md5State[1]);
  std::copy_n(digest.begin(), std::min<size_t>(output_size, digest.size()), output);
}

void XeCryptMd5Init_entry(GuestPtrT<XECRYPT_MD5_STATE> md5_state) {
  EXCRYPT_MD5_STATE state{};
  ExCryptMd5Init(&state);
  ToGuest(state, *md5_state);
}

void XeCryptMd5Update_entry(GuestPtrT<XECRYPT_MD5_STATE> md5_state, MappedVoid input,
                            u32 input_size) {
  EXCRYPT_MD5_STATE state;
  ToHost(*md5_state, state);
  ExCryptMd5Update(&state, input.As<const u8*>(), input_size);
  ToGuest(state, *md5_state);
}

void XeCryptMd5Final_entry(GuestPtrT<XECRYPT_MD5_STATE> md5_state, MappedVoid out, u32 out_size) {
  EXCRYPT_MD5_STATE state;
  ToHost(*md5_state, state);
  const auto digest = FinishMD5(state);
  std::copy_n(digest.begin(), std::min<size_t>(out_size, digest.size()), out.As<u8*>());
  ToGuest(state, *md5_state);
}

void XeCryptMd5_entry(MappedVoid input_1, u32 input_1_size, MappedVoid input_2, u32 input_2_size,
                      MappedVoid input_3, u32 input_3_size, MappedVoid output, u32 output_size) {
  EXCRYPT_MD5_STATE state{};
  ExCryptMd5Init(&state);
  for (const auto& [input, size] :
       {std::pair{input_1, input_1_size}, std::pair{input_2, input_2_size},
        std::pair{input_3, input_3_size}}) {
    if (input) {
      ExCryptMd5Update(&state, input.As<const u8*>(), size);
    }
  }
  const auto digest = FinishMD5(state);
  std::copy_n(digest.begin(), std::min<size_t>(output_size, digest.size()), output.As<u8*>());
}

void XeCryptHmacMd5Init_entry(GuestPtrT<XECRYPT_HMACMD5_STATE> hmac_state, MappedVoid key,
                              u32 key_size) {
  EXCRYPT_HMACMD5_STATE state{};
  ExCryptHmacMd5Init(&state, key.As<const u8*>(), key_size);
  ToGuest(state, *hmac_state);
}

void XeCryptHmacMd5Update_entry(GuestPtrT<XECRYPT_HMACMD5_STATE> hmac_state, MappedVoid input,
                                u32 input_size) {
  EXCRYPT_HMACMD5_STATE state;
  ToHost(*hmac_state, state);
  ExCryptHmacMd5Update(&state, input.As<const u8*>(), input_size);
  ToGuest(state, *hmac_state);
}

void XeCryptHmacMd5Final_entry(GuestPtrT<XECRYPT_HMACMD5_STATE> hmac_state, MappedVoid out,
                               u32 out_size) {
  EXCRYPT_HMACMD5_STATE state;
  ToHost(*hmac_state, state);
  FinishHmacMd5(state, out.As<u8*>(), out_size);
  ToGuest(state, *hmac_state);
}

void XeCryptHmacMd5_entry(MappedVoid key, u32 key_size, MappedVoid inp_1, u32 inp_1_size,
                          MappedVoid inp_2, u32 inp_2_size, MappedVoid inp_3, u32 inp_3_size,
                          MappedVoid out, u32 out_size) {
  EXCRYPT_HMACMD5_STATE state{};
  ExCryptHmacMd5Init(&state, key.As<const u8*>(), key_size);
  for (const auto& [input, size] :
       {std::pair{inp_1, inp_1_size}, std::pair{inp_2, inp_2_size}, std::pair{inp_3, inp_3_size}}) {
    if (input) {
      ExCryptHmacMd5Update(&state, input.As<const u8*>(), size);
    }
  }
  FinishHmacMd5(state, out.As<u8*>(), out_size);
}

constexpr u32 kPkcs1MinSize = 39;
constexpr u32 kPkcs1MaxSize = 512;

/**
 * Writes a PKCS1 padded SHA1 hash in the little endian order of the bignum exports.
 *
 * @param hash The 20 byte SHA1 hash.
 * @param format 0 for the standard digest info, 1 for the short form, 2 for no digest info.
 * @param output_sig Receives the padded block.
 * @param output_sig_size The size of the block in bytes, 39 to 512. Anything else writes nothing.
 */
void XeCryptBnDwLePkcs1Format_entry(MappedVoid hash, u32 format, MappedVoid output_sig,
                                    u32 output_sig_size) {
  if (output_sig_size < kPkcs1MinSize || output_sig_size > kPkcs1MaxSize) {
    return;
  }
  ExCryptBnDwLePkcs1Format(hash.As<const u8*>(), format, output_sig.As<u8*>(), output_sig_size);
}

/**
 * Builds the 256 byte masked signature block of a 20 byte SHA1 hash and a 10 byte salt.
 *
 * @param sig Receives the block.
 * @param hash The 20 byte SHA1 hash.
 * @param salt The 10 byte salt.
 */
void XeCryptBnQwBeSigFormat_entry(MappedVoid sig, MappedVoid hash, MappedVoid salt) {
  ExCryptBnQwBeSigFormat(sig.As<EXCRYPT_SIG*>(), hash.As<const u8*>(), salt.As<const u8*>());
}

constexpr u32 kSigDigits = 0x20;

/**
 * Signs the formatted block of a SHA1 hash and a 10 byte salt with an RSA private key.
 *
 * @param sig Receives the 256 byte signature, left untouched on failure.
 * @param hash The 20 byte SHA1 hash.
 * @param salt The 10 byte salt.
 * @param rsa The private key, a header followed by the modulus and the CRT numbers.
 * @return 1 on success, 0 when the key is not 32 quadwords or the private operation fails.
 */
u32 XeCryptBnQwBeSigCreate_entry(MappedVoid sig, MappedVoid hash, MappedVoid salt,
                                 GuestPtrT<XECRYPT_RSA> rsa) {
  if (rsa->size != kSigDigits) {
    return 0;
  }
  EXCRYPT_SIG block;
  ExCryptBnQwBeSigFormat(&block, hash.As<const u8*>(), salt.As<const u8*>());
  return XeCryptBnQwNeRsaPrvCrypt_entry(MappedU64::FromHost(reinterpret_cast<be_u64*>(&block)),
                                        MappedU64::FromHost(sig.As<be_u64*>()), rsa);
}

/**
 * Verifies a signature against the formatted block of a SHA1 hash and a 10 byte salt.
 *
 * @param sig The 256 byte signature.
 * @param hash The 20 byte SHA1 hash.
 * @param salt The 10 byte salt.
 * @param rsa The public key, a header followed by the modulus.
 * @return 1 when the signature matches, 0 when the key is not 32 quadwords with an exponent of 3
 * or 65537, the public operation fails or the blocks differ.
 */
u32 XeCryptBnQwBeSigVerify_entry(MappedVoid sig, MappedVoid hash, MappedVoid salt,
                                 GuestPtrT<XECRYPT_RSA> rsa) {
  const u32 exponent = rsa->public_exponent;
  if (rsa->size != kSigDigits || (exponent != 3 && exponent != 0x10001)) {
    return 0;
  }
  EXCRYPT_SIG recovered;
  if (!XeCryptBnQwNeRsaPubCrypt_entry(MappedU64::FromHost(sig.As<be_u64*>()),
                                      MappedU64::FromHost(reinterpret_cast<be_u64*>(&recovered)),
                                      rsa)) {
    return 0;
  }
  EXCRYPT_SIG expected;
  ExCryptBnQwBeSigFormat(&expected, hash.As<const u8*>(), salt.As<const u8*>());
  return std::memcmp(&recovered, &expected, sizeof(expected)) == 0 ? 1 : 0;
}

void XeCryptRandom_entry(MappedVoid buf, u32 buf_size) {
  std::memset(buf, 0xFD, buf_size);
}

/** DES state in guest memory, 16 round keys of 8 bytes each. */
struct XECRYPT_DES_STATE {
  u8 keytab[16][8];
};
REX_STATIC_ASSERT_SIZE(XECRYPT_DES_STATE, 0x80);

struct XECRYPT_DES3_STATE {
  XECRYPT_DES_STATE des_state[3];
};
REX_STATIC_ASSERT_SIZE(XECRYPT_DES3_STATE, 0x180);

/** AES 128 state in guest memory, the encryption and the decryption key schedules. */
struct XECRYPT_AES_STATE {
  u8 keytabenc[11][4][4];
  u8 keytabdec[11][4][4];
};
REX_STATIC_ASSERT_SIZE(XECRYPT_AES_STATE, 0x160);

/** AES key schedule in guest memory, the words of EXCRYPT_AES_SCHEDULE in big endian. */
struct AESSchedule {
  be_u32 words[sizeof(EXCRYPT_AES_SCHEDULE) / sizeof(u32)];
};
REX_STATIC_ASSERT_SIZE(AESSchedule, 0x1D4);

/** Maps each title round key bit to its ExCrypt round key bit, -1 where the title bit stays 0. */
constexpr std::array<i8, 64> kDESRoundKeyBits = {
    -1, -1, 11, 10, 9,  8,  7,  6,  -1, -1, 23, 22, 21, 20, 19, 18, -1, -1, 35, 34, 33, 32,
    31, 30, -1, -1, 47, 46, 45, 44, 43, 42, 15, 14, 13, 12, -1, -1, 5,  4,  27, 26, 25, 24,
    -1, -1, 17, 16, 39, 38, 37, 36, -1, -1, 29, 28, 3,  2,  1,  0,  -1, -1, 41, 40,
};

void ToHost(const XECRYPT_DES_STATE& title, EXCRYPT_DES_STATE& host) {
  for (u32 round = 0; round < 16; ++round) {
    u64 key = 0;
    for (u32 bit = 0; bit < 64; ++bit) {
      if (kDESRoundKeyBits[bit] >= 0 && (title.keytab[round][bit / 8] >> (bit % 8)) & 1) {
        key |= u64{1} << kDESRoundKeyBits[bit];
      }
    }
    host.keytab[round] = key;
  }
}

void ToGuest(const EXCRYPT_DES_STATE& host, XECRYPT_DES_STATE& title) {
  for (u32 round = 0; round < 16; ++round) {
    for (u32 byte = 0; byte < 8; ++byte) {
      u8 value = 0;
      for (u32 bit = 0; bit < 8; ++bit) {
        const i8 source = kDESRoundKeyBits[byte * 8 + bit];
        if (source >= 0 && (host.keytab[round] >> source) & 1) {
          value |= static_cast<u8>(1 << bit);
        }
      }
      title.keytab[round][byte] = value;
    }
  }
}

void ToHost(const XECRYPT_DES3_STATE& title, EXCRYPT_DES3_STATE& host) {
  for (u32 i = 0; i < 3; ++i) {
    ToHost(title.des_state[i], host.des_state[i]);
  }
}

void ToGuest(const EXCRYPT_DES3_STATE& host, XECRYPT_DES3_STATE& title) {
  for (u32 i = 0; i < 3; ++i) {
    ToGuest(host.des_state[i], title.des_state[i]);
  }
}

void ToHost(const XECRYPT_AES_STATE& title, EXCRYPT_AES_STATE& host) {
  ExCryptAesKey(&host, &title.keytabenc[0][0][0]);
}

void ToHost(const AESSchedule& title, EXCRYPT_AES_SCHEDULE& host) {
  std::array<u32, std::size(title.words)> words;
  std::transform(std::begin(title.words), std::end(title.words), words.begin(),
                 [](const be_u32& word) { return static_cast<u32>(word); });
  std::memcpy(&host, words.data(), sizeof(host));
}

void ToGuest(const EXCRYPT_AES_SCHEDULE& host, AESSchedule& title) {
  std::array<u32, std::size(title.words)> words;
  std::memcpy(words.data(), &host, sizeof(host));
  std::copy(words.begin(), words.end(), std::begin(title.words));
}

/**
 * Fills the encryption and decryption key schedules of a guest AES state.
 *
 * @param key The 16 byte key.
 * @param title Receives the schedules.
 */
void FillAESState(const u8* key, XECRYPT_AES_STATE& title) {
  u32 encryption[RKLENGTH(128)];
  u32 decryption[RKLENGTH(128)];
  rijndaelSetupEncrypt(encryption, key, 128);
  std::copy(std::begin(encryption), std::end(encryption), decryption);
  rijndaelSetupDecrypt(decryption, key, 128);
  for (u32 round = 0; round < 11; ++round) {
    for (u32 column = 0; column < 4; ++column) {
      for (u32 row = 0; row < 4; ++row) {
        const u32 shift = 24 - 8 * row;
        title.keytabenc[round][column][row] =
            static_cast<u8>(encryption[round * 4 + column] >> shift);
        title.keytabdec[round][column][row] =
            static_cast<u8>(decryption[(10 - round) * 4 + column] >> shift);
      }
    }
  }
}

/**
 * Sets bit 0 of each byte so its parity is odd.
 *
 * @param out_ptr Receives the adjusted bytes.
 */
void XeCryptDesParity_entry(MappedVoid inp, u32 inp_size, MappedVoid out_ptr) {
  std::vector<u8> key(inp.As<const u8*>(), inp.As<const u8*>() + inp_size);
  std::ranges::transform(key, key.begin(), [](u8 byte) { return static_cast<u8>(byte & 0xFE); });
  ExCryptDesParity(key.data(), inp_size, out_ptr.As<u8*>());
}

void XeCryptDesKey_entry(GuestPtrT<XECRYPT_DES_STATE> state_ptr, MappedU64 key) {
  const u64 key_block = *key;
  EXCRYPT_DES_STATE state;
  ExCryptDesKey(&state, reinterpret_cast<const u8*>(&key_block));
  ToGuest(state, *state_ptr);
}

void XeCryptDesEcb_entry(GuestPtrT<XECRYPT_DES_STATE> state_ptr, MappedU64 inp, MappedU64 out,
                         u32 encrypt) {
  EXCRYPT_DES_STATE state;
  ToHost(*state_ptr, state);
  const u64 block = *inp;
  u64 result = 0;
  ExCryptDesEcb(&state, reinterpret_cast<const u8*>(&block), reinterpret_cast<u8*>(&result),
                static_cast<u8>(encrypt != 0));
  *out = result;
}

void XeCryptDesCbc_entry(GuestPtrT<XECRYPT_DES_STATE> state_ptr, MappedU64 inp, u32 inp_size,
                         MappedU64 out, MappedU64 feed, u32 encrypt) {
  REX_ASSERT(inp_size % 8 == 0);

  EXCRYPT_DES_STATE state;
  ToHost(*state_ptr, state);
  const u32 blocks = inp_size / 8;
  std::vector<u64> input(blocks);
  std::vector<u64> output(blocks);
  for (u32 i = 0; i < blocks; ++i) {
    input[i] = inp[i];
  }
  u64 last_block = *feed;
  ExCryptDesCbc(&state, reinterpret_cast<const u8*>(input.data()), blocks * 8,
                reinterpret_cast<u8*>(output.data()), reinterpret_cast<u8*>(&last_block),
                static_cast<u8>(encrypt != 0));
  for (u32 i = 0; i < blocks; ++i) {
    out[i] = output[i];
  }
  *feed = last_block;
}

void XeCryptDes3Key_entry(GuestPtrT<XECRYPT_DES3_STATE> state_ptr, MappedU64 key) {
  const u64 keys[3] = {key[0], key[1], key[2]};
  EXCRYPT_DES3_STATE state;
  ExCryptDes3Key(&state, keys);
  ToGuest(state, *state_ptr);
}

void XeCryptDes3Ecb_entry(GuestPtrT<XECRYPT_DES3_STATE> state_ptr, MappedU64 inp, MappedU64 out,
                          u32 encrypt) {
  EXCRYPT_DES3_STATE state;
  ToHost(*state_ptr, state);
  const u64 block = *inp;
  u64 result = 0;
  ExCryptDes3Ecb(&state, reinterpret_cast<const u8*>(&block), reinterpret_cast<u8*>(&result),
                 static_cast<u8>(encrypt != 0));
  *out = result;
}

void XeCryptDes3Cbc_entry(GuestPtrT<XECRYPT_DES3_STATE> state_ptr, MappedU64 inp, u32 inp_size,
                          MappedU64 out, MappedU64 feed, u32 encrypt) {
  REX_ASSERT(inp_size % 8 == 0);

  EXCRYPT_DES3_STATE state;
  ToHost(*state_ptr, state);
  const u32 blocks = inp_size / 8;
  std::vector<u64> input(blocks);
  std::vector<u64> output(blocks);
  for (u32 i = 0; i < blocks; ++i) {
    input[i] = inp[i];
  }
  u64 last_block = *feed;
  ExCryptDes3Cbc(&state, reinterpret_cast<const u8*>(input.data()), blocks * 8,
                 reinterpret_cast<u8*>(output.data()), reinterpret_cast<u8*>(&last_block),
                 static_cast<u8>(encrypt != 0));
  for (u32 i = 0; i < blocks; ++i) {
    out[i] = output[i];
  }
  *feed = last_block;
}

/**
 * Expands a 16 byte AES 128 key into the encryption and decryption key schedules.
 *
 * @param state_ptr Receives the schedules.
 * @param key The 16 byte key.
 */
void XeCryptAesKey_entry(GuestPtrT<XECRYPT_AES_STATE> state_ptr, MappedVoid key) {
  FillAESState(key.As<const u8*>(), *state_ptr);
}

void XeCryptAesEcb_entry(GuestPtrT<XECRYPT_AES_STATE> state_ptr, MappedVoid inp_ptr,
                         MappedVoid out_ptr, u32 encrypt) {
  alignas(16) EXCRYPT_AES_STATE state;
  ToHost(*state_ptr, state);
  ExCryptAesEcb(&state, inp_ptr.As<const u8*>(), out_ptr.As<u8*>(), static_cast<u8>(encrypt != 0));
}

void XeCryptAesCbc_entry(GuestPtrT<XECRYPT_AES_STATE> state_ptr, MappedVoid inp_ptr, u32 inp_size,
                         MappedVoid out_ptr, MappedVoid feed_ptr, u32 encrypt) {
  alignas(16) EXCRYPT_AES_STATE state;
  ToHost(*state_ptr, state);
  ExCryptAesCbc(&state, inp_ptr.As<const u8*>(), inp_size, out_ptr.As<u8*>(), feed_ptr.As<u8*>(),
                static_cast<u8>(encrypt != 0));
}

/**
 * Builds an AES key schedule, leaving it untouched for a key size other than 16, 24 or 32 bytes.
 *
 * @param key_size The key size in bytes.
 * @param schedule_ptr Receives the schedule.
 */
void XeCryptAesCreateKeySchedule_entry(MappedVoid key, u32 key_size,
                                       GuestPtrT<AESSchedule> schedule_ptr) {
  if (key_size != 0x10 && key_size != 0x18 && key_size != 0x20) {
    return;
  }
  alignas(16) EXCRYPT_AES_SCHEDULE schedule;
  ToHost(*schedule_ptr, schedule);
  std::array<u32, sizeof(schedule.keytab) / sizeof(u32)> words;
  std::memcpy(words.data(), schedule.keytab, sizeof(schedule.keytab));
  const u32 num_rounds = (key_size >> 2) + 5;
  u32* decryption = words.data() + (num_rounds + 1) * 4;
  rijndaelSetupEncrypt(words.data(), key.As<const u8*>(), key_size * 8);
  std::memmove(decryption, words.data(), (num_rounds + 2) * 0x10);
  rijndaelSetupDecrypt(decryption, key.As<const u8*>(), key_size * 8);
  std::memcpy(schedule.keytab, words.data(), sizeof(schedule.keytab));
  schedule.num_rounds = num_rounds;
  ToGuest(schedule, *schedule_ptr);
}

void XeCryptAesEncryptOne_entry(GuestPtrT<AESSchedule> schedule_ptr, MappedVoid inp_ptr,
                                MappedVoid out_ptr) {
  alignas(16) EXCRYPT_AES_SCHEDULE schedule;
  ToHost(*schedule_ptr, schedule);
  ExCryptAesEncryptOne(&schedule, inp_ptr.As<const u8*>(), out_ptr.As<u8*>());
}

void XeCryptAesDecryptOne_entry(GuestPtrT<AESSchedule> schedule_ptr, MappedVoid inp_ptr,
                                MappedVoid out_ptr) {
  alignas(16) EXCRYPT_AES_SCHEDULE schedule;
  ToHost(*schedule_ptr, schedule);
  ExCryptAesDecryptOne(&schedule, inp_ptr.As<const u8*>(), out_ptr.As<u8*>());
}

void XeCryptAesCbcEncrypt_entry(GuestPtrT<AESSchedule> schedule_ptr, MappedVoid inp_ptr,
                                u32 inp_size, MappedVoid out_ptr, MappedVoid feed_ptr) {
  alignas(16) EXCRYPT_AES_SCHEDULE schedule;
  ToHost(*schedule_ptr, schedule);
  ExCryptAesCbcEncrypt(&schedule, inp_ptr.As<const u8*>(), inp_size, out_ptr.As<u8*>(),
                       feed_ptr.As<u8*>());
}

void XeCryptAesCbcDecrypt_entry(GuestPtrT<AESSchedule> schedule_ptr, MappedVoid inp_ptr,
                                u32 inp_size, MappedVoid out_ptr, MappedVoid feed_ptr) {
  alignas(16) EXCRYPT_AES_SCHEDULE schedule;
  ToHost(*schedule_ptr, schedule);
  ExCryptAesCbcDecrypt(&schedule, inp_ptr.As<const u8*>(), inp_size, out_ptr.As<u8*>(),
                       feed_ptr.As<u8*>());
}

/**
 * Returns a pointer into the elliptic curve table for a curve ID. It returns 0 for a valid ID, 4
 * for an ID past the table, and 1 when out_ptr is null.
 *
 * @param curve_id Range 0 to 14.
 * @param out_ptr Receives the table pointer, or 0 for an ID past the table.
 */
u32 XeCryptEccGetCurveParameters_entry(u32 curve_id, GuestPtrT<be_u32> out_ptr) {
  if (!out_ptr) {
    return 1;
  }
  constexpr u32 kCurveCount = 15;
  if (curve_id >= kCurveCount) {
    *out_ptr = 0;
    return 4;
  }
  *out_ptr = kernel::GetCurrentState().GetKernelGlobals() +
             static_cast<u32>(offsetof(kernel::KernelGuestGlobals, ecc_curves)) + 4 * curve_id;
  return 0;
}

void XeCryptBnDw_Zero_entry(MappedVoid data, u32 dwords) {
  ExCryptBnDw_Zero(data.As<u32*>(), dwords);
}

void XeCryptBnDw_Copy_entry(MappedVoid source, MappedVoid dest, u32 dwords) {
  ExCryptBnDw_Copy(source.As<const u32*>(), dest.As<u32*>(), dwords);
}

void XeCryptBnDw_SwapLeBe_entry(MappedVoid source, MappedVoid dest, u32 dwords) {
  ExCryptBnDw_SwapLeBe(source.As<const u32*>(), dest.As<u32*>(), dwords);
}

void XeCryptBnQw_Zero_entry(MappedVoid data, u32 qwords) {
  ExCryptBnQw_Zero(data.As<u64*>(), qwords);
}

void XeCryptBnQw_Copy_entry(MappedVoid source, MappedVoid dest, u32 qwords) {
  ExCryptBnQw_Copy(source.As<const u64*>(), dest.As<u64*>(), qwords);
}

void XeCryptBnQw_SwapLeBe_entry(MappedVoid source, MappedVoid dest, u32 qwords) {
  ExCryptBnQw_SwapLeBe(source.As<const u64*>(), dest.As<u64*>(), qwords);
}

void XeCryptBnQw_SwapDwQw_entry(MappedVoid source, MappedVoid dest, u32 qwords) {
  ExCryptBnQw_SwapDwQw(source.As<const u64*>(), dest.As<u64*>(), qwords);
}

/**
 * Compares two numbers of num_qwords quadwords.
 *
 * @param num_qwords The number of quadwords in each.
 * @return 1 when input1 is greater, -1 when it is less, otherwise 0.
 */
i32 XeCryptBnQwNeCompare_entry(MappedU64 input1, MappedU64 input2, u32 num_qwords) {
  return CompareNumbers(LoadNumber(input1, num_qwords), LoadNumber(input2, num_qwords));
}

/**
 * Computes A * B * R^-1 modulo the odd modulus, with R = 2^(64 * modulus_size).
 *
 * @param input_a The first factor.
 * @param input_b The second factor.
 * @param output_c Receives the product.
 * @param inverse The inverse of the low modulus quadword modulo 2^64.
 * @param modulus The odd modulus.
 * @param modulus_size The modulus size in quadwords, 1 to 64.
 */
void XeCryptBnQwNeModMul_entry(MappedU64 input_a, MappedU64 input_b, MappedU64 output_c,
                               u64 inverse, MappedU64 modulus, u32 modulus_size) {
  if (modulus_size == 0 || modulus_size > kBignumMaxDigits) {
    return;
  }
  StoreNumber(
      MontgomeryMultiply(LoadNumber(input_a, modulus_size), LoadNumber(input_b, modulus_size),
                         LoadNumber(modulus, modulus_size), inverse),
      output_c);
}

u64 XeCryptBnQwNeModInv_entry(u64 input) {
  return ExCryptBnQwNeModInv(input);
}

void XeCryptParveEcb_entry(MappedVoid key, MappedVoid sbox, MappedVoid input, MappedVoid output) {
  ExCryptParveEcb(key.As<const u8*>(), sbox.As<const u8*>(), input.As<const u8*>(),
                  output.As<u8*>());
}

void XeCryptParveCbcMac_entry(MappedVoid key, MappedVoid sbox, MappedVoid iv, MappedVoid input,
                              u32 input_size, MappedVoid output) {
  ExCryptParveCbcMac(key.As<const u8*>(), sbox.As<const u8*>(), iv.As<const u8*>(),
                     input.As<const u8*>(), input_size, output.As<u8*>());
}

void XeCryptChainAndSumMac_entry(MappedU32 cd, MappedU32 ab, MappedU32 input, u32 input_dwords,
                                 MappedU32 output) {
  const u32 cd_words[2] = {cd[0], cd[1]};
  const u32 ab_words[2] = {ab[0], ab[1]};
  std::vector<u32> words(input_dwords);
  for (u32 i = 0; i < input_dwords; ++i) {
    words[i] = input[i];
  }
  u32 result[2];
  ExCryptChainAndSumMac(cd_words, ab_words, words.data(), input_dwords, result);
  output[0] = result[0];
  output[1] = result[1];
}

void XeCryptRotSumSha_entry(MappedVoid input_1, u32 input_1_size, MappedVoid input_2,
                            u32 input_2_size, MappedVoid output, u32 output_size) {
  ExCryptRotSumSha(input_1.As<const u8*>(), input_1_size, input_2.As<const u8*>(), input_2_size,
                   output.As<u8*>(), output_size);
}

void XeCryptHmacShaInit_entry(GuestPtrT<XECRYPT_HMACSHA_STATE> hmac_state, MappedVoid key,
                              u32 key_size) {
  EXCRYPT_HMACSHA_STATE state{};
  ExCryptHmacShaInit(&state, key.As<const u8*>(), key_size);
  ToGuest(state, *hmac_state);
}

void XeCryptHmacShaUpdate_entry(GuestPtrT<XECRYPT_HMACSHA_STATE> hmac_state, MappedVoid input,
                                u32 input_size) {
  EXCRYPT_HMACSHA_STATE state;
  ToHost(*hmac_state, state);
  ExCryptHmacShaUpdate(&state, input.As<const u8*>(), input_size);
  ToGuest(state, *hmac_state);
}

void XeCryptHmacShaFinal_entry(GuestPtrT<XECRYPT_HMACSHA_STATE> hmac_state, MappedVoid out,
                               u32 out_size) {
  EXCRYPT_HMACSHA_STATE state;
  ToHost(*hmac_state, state);
  ExCryptHmacShaFinal(&state, out.As<u8*>(), out_size);
  ToGuest(state, *hmac_state);
}

void XeCryptHmacSha_entry(MappedVoid key, u32 key_size, MappedVoid inp_1, u32 inp_1_size,
                          MappedVoid inp_2, u32 inp_2_size, MappedVoid inp_3, u32 inp_3_size,
                          MappedVoid out, u32 out_size) {
  ExCryptHmacSha(key.As<const u8*>(), key_size, inp_1.As<const u8*>(), inp_1_size,
                 inp_2.As<const u8*>(), inp_2_size, inp_3.As<const u8*>(), inp_3_size,
                 out.As<u8*>(), out_size);
}

/**
 * Checks the first compare_size bytes (at most 20) of an HMAC SHA digest.
 *
 * @param key_size The key size in bytes.
 * @param inp_1 The first input.
 * @param inp_1_size The first input size in bytes.
 * @param inp_2 The second input.
 * @param inp_2_size The second input size in bytes.
 * @param inp_3 The third input.
 * @param inp_3_size The third input size in bytes.
 * @param compare The digest to compare against.
 * @param compare_size The number of bytes to compare.
 * @return 1 when the bytes match, otherwise 0.
 */
u32 XeCryptHmacShaVerify_entry(MappedVoid key, u32 key_size, MappedVoid inp_1, u32 inp_1_size,
                               MappedVoid inp_2, u32 inp_2_size, MappedVoid inp_3, u32 inp_3_size,
                               MappedVoid compare, u32 compare_size) {
  return ExCryptHmacShaVerify(key.As<const u8*>(), key_size, inp_1.As<const u8*>(), inp_1_size,
                              inp_2.As<const u8*>(), inp_2_size, inp_3.As<const u8*>(), inp_3_size,
                              compare.As<const u8*>(), compare_size)
             ? 1
             : 0;
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
    XeCryptHmacSha_entry(MappedVoid::FromHost(const_cast<u8*>(key)), 0x10, inp_1, inp_1_size, inp_2,
                         inp_2_size, inp_3, inp_3_size, out, out_size);

    return X_STATUS_SUCCESS;
  }

  return X_STATUS_UNSUCCESSFUL;
}

static const u8 kKeyObfuscationKey[16] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                                          0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};

static void UnobscureKey(const u8* obscured_key, u8* key) {
  alignas(16) EXCRYPT_AES_STATE aes;
  ExCryptAesKey(&aes, kKeyObfuscationKey);
  ExCryptAesEcb(&aes, obscured_key, key, 0);
}

u32 XeKeysAesCbcUsingKey_entry(MappedVoid obscured_key, MappedVoid inp_ptr, u32 inp_size,
                               MappedVoid out_ptr, MappedVoid feed_ptr, u32 encrypt) {
  u8 key[16];
  UnobscureKey(obscured_key.As<const u8*>(), key);

  alignas(16) EXCRYPT_AES_STATE aes;
  ExCryptAesKey(&aes, key);
  ExCryptAesCbc(&aes, inp_ptr.As<const u8*>(), inp_size, out_ptr.As<u8*>(), feed_ptr.As<u8*>(),
                static_cast<u8>(encrypt != 0));

  return X_STATUS_SUCCESS;
}

u32 XeKeysObscureKey_entry(MappedVoid input, MappedVoid output) {
  alignas(16) EXCRYPT_AES_STATE aes;
  ExCryptAesKey(&aes, kKeyObfuscationKey);
  ExCryptAesEcb(&aes, input.As<const u8*>(), output.As<u8*>(), 1);

  return X_STATUS_SUCCESS;
}

u32 XeKeysHmacShaUsingKey_entry(MappedVoid obscured_key, MappedVoid inp_1, u32 inp_1_size,
                                MappedVoid inp_2, u32 inp_2_size, MappedVoid inp_3, u32 inp_3_size,
                                MappedVoid out, u32 out_size) {
  REX_KERNEL_REQUIRE_NONNULL(obscured_key);

  u8 key[16];
  UnobscureKey(obscured_key.As<const u8*>(), key);

  XeCryptHmacSha_entry(MappedVoid::FromHost(key), 0x10, inp_1, inp_1_size, inp_2, inp_2_size, inp_3,
                       inp_3_size, out, out_size);
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
REX_EXPORT_STUB(__imp__XeCryptBnQwNeModExp);
REX_EXPORT_STUB(__imp__XeCryptBnQwNeModExpRoot);
REX_EXPORT_STUB(__imp__XeCryptBnQwNeRsaKeyGen);
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
REX_EXPORT_STUB(__imp__XeCryptAesGcmInitialize);
REX_EXPORT_STUB(__imp__XeCryptAesGcmUpdate);
REX_EXPORT_STUB(__imp__XeCryptAesGcmFinalize);
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
REX_EXPORT(__imp__XeCryptSha256, rex::kernel::xboxkrnl::XeCryptSha256_entry)
REX_EXPORT(__imp__XeCryptSha384Init, rex::kernel::xboxkrnl::XeCryptSha384Init_entry)
REX_EXPORT(__imp__XeCryptSha384Update, rex::kernel::xboxkrnl::XeCryptSha384Update_entry)
REX_EXPORT(__imp__XeCryptSha384Final, rex::kernel::xboxkrnl::XeCryptSha384Final_entry)
REX_EXPORT(__imp__XeCryptSha384, rex::kernel::xboxkrnl::XeCryptSha384_entry)
REX_EXPORT(__imp__XeCryptSha512Init, rex::kernel::xboxkrnl::XeCryptSha512Init_entry)
REX_EXPORT(__imp__XeCryptSha512Update, rex::kernel::xboxkrnl::XeCryptSha512Update_entry)
REX_EXPORT(__imp__XeCryptSha512Final, rex::kernel::xboxkrnl::XeCryptSha512Final_entry)
REX_EXPORT(__imp__XeCryptSha512, rex::kernel::xboxkrnl::XeCryptSha512_entry)
REX_EXPORT(__imp__XeCryptHmacShaInit, rex::kernel::xboxkrnl::XeCryptHmacShaInit_entry)
REX_EXPORT(__imp__XeCryptHmacShaUpdate, rex::kernel::xboxkrnl::XeCryptHmacShaUpdate_entry)
REX_EXPORT(__imp__XeCryptHmacShaFinal, rex::kernel::xboxkrnl::XeCryptHmacShaFinal_entry)
REX_EXPORT(__imp__XeCryptHmacShaVerify, rex::kernel::xboxkrnl::XeCryptHmacShaVerify_entry)
REX_EXPORT(__imp__XeCryptBnQw_SwapDwQwLeBe, rex::kernel::xboxkrnl::XeCryptBnQw_SwapDwQwLeBe_entry)
REX_EXPORT(__imp__XeCryptBnQwNeRsaPrvCrypt, rex::kernel::xboxkrnl::XeCryptBnQwNeRsaPrvCrypt_entry)
REX_EXPORT(__imp__XeCryptBnQwNeRsaPubCrypt, rex::kernel::xboxkrnl::XeCryptBnQwNeRsaPubCrypt_entry)
REX_EXPORT(__imp__XeCryptMd5Init, rex::kernel::xboxkrnl::XeCryptMd5Init_entry)
REX_EXPORT(__imp__XeCryptMd5Update, rex::kernel::xboxkrnl::XeCryptMd5Update_entry)
REX_EXPORT(__imp__XeCryptMd5Final, rex::kernel::xboxkrnl::XeCryptMd5Final_entry)
REX_EXPORT(__imp__XeCryptMd5, rex::kernel::xboxkrnl::XeCryptMd5_entry)
REX_EXPORT(__imp__XeCryptHmacMd5Init, rex::kernel::xboxkrnl::XeCryptHmacMd5Init_entry)
REX_EXPORT(__imp__XeCryptHmacMd5Update, rex::kernel::xboxkrnl::XeCryptHmacMd5Update_entry)
REX_EXPORT(__imp__XeCryptHmacMd5Final, rex::kernel::xboxkrnl::XeCryptHmacMd5Final_entry)
REX_EXPORT(__imp__XeCryptHmacMd5, rex::kernel::xboxkrnl::XeCryptHmacMd5_entry)
REX_EXPORT(__imp__XeCryptBnDwLePkcs1Format, rex::kernel::xboxkrnl::XeCryptBnDwLePkcs1Format_entry)
REX_EXPORT(__imp__XeCryptBnQwBeSigCreate, rex::kernel::xboxkrnl::XeCryptBnQwBeSigCreate_entry)
REX_EXPORT(__imp__XeCryptBnQwBeSigVerify, rex::kernel::xboxkrnl::XeCryptBnQwBeSigVerify_entry)
REX_EXPORT(__imp__XeCryptBnQwBeSigFormat, rex::kernel::xboxkrnl::XeCryptBnQwBeSigFormat_entry)
REX_EXPORT(__imp__XeCryptBnDwLePkcs1Verify, rex::kernel::xboxkrnl::XeCryptBnDwLePkcs1Verify_entry)
REX_EXPORT(__imp__XeCryptRandom, rex::kernel::xboxkrnl::XeCryptRandom_entry)
REX_EXPORT(__imp__XeCryptDesParity, rex::kernel::xboxkrnl::XeCryptDesParity_entry)
REX_EXPORT(__imp__XeCryptBnDw_Zero, rex::kernel::xboxkrnl::XeCryptBnDw_Zero_entry)
REX_EXPORT(__imp__XeCryptBnDw_Copy, rex::kernel::xboxkrnl::XeCryptBnDw_Copy_entry)
REX_EXPORT(__imp__XeCryptBnDw_SwapLeBe, rex::kernel::xboxkrnl::XeCryptBnDw_SwapLeBe_entry)
REX_EXPORT(__imp__XeCryptBnQw_Zero, rex::kernel::xboxkrnl::XeCryptBnQw_Zero_entry)
REX_EXPORT(__imp__XeCryptBnQw_Copy, rex::kernel::xboxkrnl::XeCryptBnQw_Copy_entry)
REX_EXPORT(__imp__XeCryptBnQw_SwapLeBe, rex::kernel::xboxkrnl::XeCryptBnQw_SwapLeBe_entry)
REX_EXPORT(__imp__XeCryptBnQw_SwapDwQw, rex::kernel::xboxkrnl::XeCryptBnQw_SwapDwQw_entry)
REX_EXPORT(__imp__XeCryptBnQwNeCompare, rex::kernel::xboxkrnl::XeCryptBnQwNeCompare_entry)
REX_EXPORT(__imp__XeCryptBnQwNeModMul, rex::kernel::xboxkrnl::XeCryptBnQwNeModMul_entry)
REX_EXPORT(__imp__XeCryptBnQwNeModInv, rex::kernel::xboxkrnl::XeCryptBnQwNeModInv_entry)
REX_EXPORT(__imp__XeCryptParveEcb, rex::kernel::xboxkrnl::XeCryptParveEcb_entry)
REX_EXPORT(__imp__XeCryptParveCbcMac, rex::kernel::xboxkrnl::XeCryptParveCbcMac_entry)
REX_EXPORT(__imp__XeCryptChainAndSumMac, rex::kernel::xboxkrnl::XeCryptChainAndSumMac_entry)
REX_EXPORT(__imp__XeCryptRotSumSha, rex::kernel::xboxkrnl::XeCryptRotSumSha_entry)
REX_EXPORT(__imp__XeCryptDesKey, rex::kernel::xboxkrnl::XeCryptDesKey_entry)
REX_EXPORT(__imp__XeCryptDesEcb, rex::kernel::xboxkrnl::XeCryptDesEcb_entry)
REX_EXPORT(__imp__XeCryptDesCbc, rex::kernel::xboxkrnl::XeCryptDesCbc_entry)
REX_EXPORT(__imp__XeCryptDes3Key, rex::kernel::xboxkrnl::XeCryptDes3Key_entry)
REX_EXPORT(__imp__XeCryptDes3Ecb, rex::kernel::xboxkrnl::XeCryptDes3Ecb_entry)
REX_EXPORT(__imp__XeCryptDes3Cbc, rex::kernel::xboxkrnl::XeCryptDes3Cbc_entry)
REX_EXPORT(__imp__XeCryptAesKey, rex::kernel::xboxkrnl::XeCryptAesKey_entry)
REX_EXPORT(__imp__XeCryptAesEcb, rex::kernel::xboxkrnl::XeCryptAesEcb_entry)
REX_EXPORT(__imp__XeCryptAesCbc, rex::kernel::xboxkrnl::XeCryptAesCbc_entry)
REX_EXPORT(__imp__XeCryptAesCreateKeySchedule,
           rex::kernel::xboxkrnl::XeCryptAesCreateKeySchedule_entry)
REX_EXPORT(__imp__XeCryptAesEncryptOne, rex::kernel::xboxkrnl::XeCryptAesEncryptOne_entry)
REX_EXPORT(__imp__XeCryptAesDecryptOne, rex::kernel::xboxkrnl::XeCryptAesDecryptOne_entry)
REX_EXPORT(__imp__XeCryptAesCbcEncrypt, rex::kernel::xboxkrnl::XeCryptAesCbcEncrypt_entry)
REX_EXPORT(__imp__XeCryptAesCbcDecrypt, rex::kernel::xboxkrnl::XeCryptAesCbcDecrypt_entry)
REX_EXPORT(__imp__XeCryptEccGetCurveParameters,
           rex::kernel::xboxkrnl::XeCryptEccGetCurveParameters_entry)
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
