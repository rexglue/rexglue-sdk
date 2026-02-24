#pragma once

#include <cstddef>
#include <cstdint>

namespace rex::system {

// Well-known xboxkrnl.exe export ordinals used during kernel state setup.
// These are stable ABI constants from the Xbox 360 kernel.
constexpr uint16_t kOrdinal_XexExecutableModuleHandle = 0x0193;
constexpr uint16_t kOrdinal_ExLoadedImageName = 0x01AF;
constexpr size_t kExLoadedImageNameSize = 256;

}  // namespace rex::system
