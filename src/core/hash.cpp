#include <rex/hash.h>

#include <fstream>
#include <memory>

#include <format>

namespace rex {

namespace {

constexpr size_t kChunkSize = size_t{256} * 1024;

struct StateDeleter {
  void operator()(XXH3_state_t* state) const { XXH3_freeState(state); }
};

std::string HexDigest(XXH128_hash_t digest) {
  return std::format("{:016x}{:016x}", digest.high64, digest.low64);
}

}  // namespace

std::string HashBytes(std::string_view data) {
  return HexDigest(XXH3_128bits(data.data(), data.size()));
}

std::string HashFile(const std::filesystem::path& path) {
  std::error_code ec;
  if (!std::filesystem::is_regular_file(path, ec)) {
    return {};
  }
  std::ifstream file(path, std::ios::binary);
  if (!file)
    return {};

  std::unique_ptr<XXH3_state_t, StateDeleter> state(XXH3_createState());
  if (!state || XXH3_128bits_reset(state.get()) == XXH_ERROR)
    return {};

  auto chunk = std::make_unique_for_overwrite<char[]>(kChunkSize);
  while (file.read(chunk.get(), kChunkSize) || file.gcount() > 0) {
    const auto got = static_cast<size_t>(file.gcount());
    if (XXH3_128bits_update(state.get(), chunk.get(), got) == XXH_ERROR)
      return {};
  }

  if (file.bad())
    return {};

  return HexDigest(XXH3_128bits_digest(state.get()));
}

}  // namespace rex
