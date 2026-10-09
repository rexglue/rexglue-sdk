#include "ui/sdl3/image_decode.h"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO
#define STBI_ONLY_PNG
#define STBI_NO_FAILURE_STRINGS
#include <stb_image.h>

namespace rex::ui {

std::vector<uint8_t> DecodeImageRGBA(const uint8_t* data, size_t size, int& out_width,
                                     int& out_height) {
  out_width = 0;
  out_height = 0;
  if (!data || size == 0) {
    return {};
  }
  int channels = 0;
  stbi_uc* pixels = stbi_load_from_memory(data, static_cast<int>(size), &out_width, &out_height,
                                          &channels, /*req_comp=*/4);
  if (!pixels) {
    out_width = 0;
    out_height = 0;
    return {};
  }
  const size_t byte_count = static_cast<size_t>(out_width) * out_height * 4;
  std::vector<uint8_t> result(pixels, pixels + byte_count);
  stbi_image_free(pixels);
  return result;
}

}  // namespace rex::ui
