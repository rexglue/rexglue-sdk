/**
 * SHA256 hashing utilities implementation
 *
 * Uses picosha2 header-only library for SHA256 computation.
 */

#include "thirdparty/picosha2/picosha2.h"

#include <fstream>
#include <sstream>

#include <rex/crypto/sha256.h>

namespace rex::crypto {

std::string sha256(std::string_view data) {
  std::string hash_hex;
  picosha2::hash256_hex_string(data.begin(), data.end(), hash_hex);
  return hash_hex;
}

std::string sha256_file(const std::filesystem::path& path) {
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    return "";
  }
  std::ostringstream ss;
  ss << file.rdbuf();
  return sha256(ss.str());
}

}  // namespace rex::crypto
