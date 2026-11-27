/**
 * @file        tests/unit/gpu/primitive_processor_simd_test.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "rex_test.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <memory>
#include <random>
#include <vector>

#include <rex/settings_registry.h>
#include <rex/xenos/register.h>

#include "fake_title_context.h"
#include "gpu/xenos/index_conversion.h"
#include "gpu/xenos/primitive_processor.h"
#include "gpu/xenos/shared_memory.h"
#include "gpu/xenos/xenos_settings.h"
#include "test_memory.h"

namespace {

constexpr std::array<u32, 6> kCounts = {0, 1, 15, 16, 17, 70};
constexpr u16 kReset16 = 0x1234;
constexpr u32 kMask24 = 0x00FFFFFF;
constexpr u32 kReset32 = 0x00ABCDEF;

std::vector<u16> Indices16(u32 count, bool with_reset, bool with_ffff) {
  std::mt19937 engine(7 + count);
  std::vector<u16> data(count);
  for (u16& value : data) {
    value = static_cast<u16>(engine() % 0x1000);
  }
  if (count && with_reset) {
    data[count - 1] = kReset16;
  }
  if (count > 1 && with_ffff) {
    data[count / 2] = 0xFFFF;
  }
  return data;
}

std::vector<u32> Indices32(u32 count, bool with_reset) {
  std::mt19937 engine(11 + count);
  std::vector<u32> data(count);
  for (u32& value : data) {
    value = engine() & 0x0FFFFFFF;
  }
  if (count && with_reset) {
    data[count - 1] = 0xF1000000 | kReset32;
  }
  return data;
}

template <typename T>
struct AlignedCopy {
  AlignedCopy(const std::vector<T>& values, size_t offset) : storage(values.size() + 64) {
    std::copy(values.begin(), values.end(), Begin(offset));
    start = Begin(offset);
  }
  T* Begin(size_t offset) {
    void* pointer = storage.data();
    size_t space = storage.size() * sizeof(T);
    std::align(16, sizeof(T), pointer, space);
    return static_cast<T*>(pointer) + offset;
  }
  std::vector<T> storage;
  T* start;
};

struct XenosOwner : rex::gpu::XenosSettings {
  XenosOwner() { REQUIRE(REX_SETTINGS().Register(*this)); }
  ~XenosOwner() { REX_SETTINGS().Unregister(*this); }
};

struct MemoryFixture {
  MemoryFixture() { rex::test::ReleaseTestMemory(); }
  ~MemoryFixture() { rex::test::ReleaseTestMemory(); }
};

class UnusedSharedMemory final : public rex::gpu::SharedMemory {
 public:
  explicit UnusedSharedMemory(rex::test::FakeTitleContext& title) : SharedMemory(title, title) {}

 protected:
  bool UploadRanges(const std::vector<std::pair<u32, u32>>&) override { return false; }
};

class RecordingPrimitiveProcessor final : public rex::gpu::PrimitiveProcessor {
 public:
  RecordingPrimitiveProcessor(const rex::xenos::RegisterFile& registers,
                              const rex::gpu::XenosSettings& settings,
                              rex::test::FakeTitleContext& title, rex::gpu::SharedMemory& shared)
      : PrimitiveProcessor(registers, settings, title, title, shared) {}

  bool Init(bool triangle_fans_supported) {
    return InitializeCommon(true, triangle_fans_supported, true, true, true, true);
  }

  std::vector<u16> ReadIndices(const ProcessingResult& result) const {
    const std::vector<u8>* bytes = &builtin_;
    size_t offset = result.host_index_buffer_handle;
    if (result.index_buffer_type == kHostConverted) {
      bytes = &converted_[result.host_index_buffer_handle];
      offset = 0;
    }
    std::vector<u16> indices(result.host_draw_vertex_count);
    std::memcpy(indices.data(), bytes->data() + offset, indices.size() * sizeof(u16));
    return indices;
  }

 protected:
  bool InitializeBuiltinIndexBuffer(size_t size_bytes,
                                    std::function<void(void*)> fill_callback) override {
    builtin_.assign(size_bytes, 0);
    fill_callback(builtin_.data());
    return true;
  }
  void* RequestHostConvertedIndexBufferForCurrentFrame(rex::xenos::IndexFormat format,
                                                       u32 index_count, bool, u32,
                                                       size_t& driver_handle_out) override {
    REQUIRE(format == rex::xenos::IndexFormat::kInt16);
    converted_.emplace_back(index_count * sizeof(u16) + 16);
    driver_handle_out = converted_.size() - 1;
    return converted_.back().data();
  }

 private:
  std::vector<u8> builtin_;
  std::vector<std::vector<u8>> converted_;
};

struct TessellatedDraw {
  bool processed = false;
  rex::xenos::PrimitiveType host_primitive_type = rex::xenos::PrimitiveType::kNone;
  std::vector<u16> host_indices;
};

TessellatedDraw DrawTessellated(rex::xenos::PrimitiveType primitive_type,
                                rex::xenos::SourceSelect source, u32 count,
                                bool triangle_fans_supported) {
  XenosOwner settings;
  auto& memory = rex::test::GetTestMemory();
  rex::test::FakeTitleContext title(memory);
  UnusedSharedMemory shared(title);
  rex::xenos::RegisterFile registers;
  RecordingPrimitiveProcessor processor(registers, settings, title, shared);
  REQUIRE(processor.Init(triangle_fans_supported));

  constexpr u32 kIndexBase = 0x1000;
  if (source == rex::xenos::SourceSelect::kDMA) {
    auto* guest = memory.TranslatePhysical<u16*>(kIndexBase);
    for (u32 i = 0; i < count; ++i) {
      guest[i] = static_cast<u16>(100 + i);
    }
  }
  rex::xenos::reg::VGT_DRAW_INITIATOR initiator{};
  initiator.prim_type = primitive_type;
  initiator.source_select = source;
  initiator.major_mode = rex::xenos::MajorMode::kExplicit;
  initiator.index_size = rex::xenos::IndexFormat::kInt16;
  initiator.num_indices = count;
  registers[rex::xenos::Register::VGT_DRAW_INITIATOR] = initiator.value;
  rex::xenos::reg::VGT_DMA_SIZE dma_size{};
  dma_size.num_words = count;
  dma_size.swap_mode = rex::xenos::Endian::kNone;
  registers[rex::xenos::Register::VGT_DMA_SIZE] = dma_size.value;
  registers[rex::xenos::Register::VGT_DMA_BASE] = kIndexBase;
  rex::xenos::reg::VGT_OUTPUT_PATH_CNTL path{};
  path.path_select = rex::xenos::VGTOutputPath::kTessellationEnable;
  registers[rex::xenos::Register::VGT_OUTPUT_PATH_CNTL] = path.value;
  rex::xenos::reg::VGT_HOS_CNTL hos{};
  hos.tess_mode = rex::xenos::TessellationMode::kDiscrete;
  registers[rex::xenos::Register::VGT_HOS_CNTL] = hos.value;

  TessellatedDraw draw;
  rex::gpu::PrimitiveProcessor::ProcessingResult result{};
  draw.processed = processor.Process(result);
  if (draw.processed) {
    draw.host_primitive_type = result.host_primitive_type;
    draw.host_indices = processor.ReadIndices(result);
  }
  return draw;
}

}  // namespace

REX_TEST_CASE("16 bit reset scans match a plain loop", "[gpu][primitive_processor]") {
  for (const u32 count : kCounts) {
    for (const bool with_reset : {false, true}) {
      for (const bool with_ffff : {false, true}) {
        const auto values = Indices16(count, with_reset, with_ffff);
        bool want_reset = false;
        bool want_ffff = false;
        for (const u16 value : values) {
          want_reset |= value == kReset16;
          want_ffff |= value == 0xFFFF;
        }
        for (size_t offset = 0; offset < 8; ++offset) {
          AlignedCopy<u16> placed(values, offset);
          INFO("count " << count << " offset " << offset);
          CHECK(rex::gpu::IsResetIndexUsed(placed.start, count, kReset16) == want_reset);
          bool used = !want_reset;
          bool ffff = !want_ffff;
          rex::gpu::Get16BitResetIndexUsage(placed.start, count, kReset16, used, ffff);
          CHECK(used == want_reset);
          CHECK(ffff == want_ffff);
        }
      }
    }
  }
}

REX_TEST_CASE("32 bit reset scan masks before comparing", "[gpu][primitive_processor]") {
  for (const u32 count : kCounts) {
    for (const bool with_reset : {false, true}) {
      const auto values = Indices32(count, with_reset);
      bool want = false;
      for (const u32 value : values) {
        want |= (value & kMask24) == kReset32;
      }
      for (size_t offset = 0; offset < 4; ++offset) {
        AlignedCopy<u32> placed(values, offset);
        INFO("count " << count << " offset " << offset);
        CHECK(rex::gpu::IsResetIndexUsed(placed.start, count, kReset32, kMask24) == want);
      }
    }
  }
}

REX_TEST_CASE("16 bit reset replacement matches a plain loop", "[gpu][primitive_processor]") {
  for (const u32 count : kCounts) {
    const auto values = Indices16(count, true, true);
    for (size_t offset = 0; offset < 8; ++offset) {
      for (size_t dest_offset = 0; dest_offset < 8; ++dest_offset) {
        AlignedCopy<u16> source(values, offset);
        AlignedCopy<u16> dest16(std::vector<u16>(count), dest_offset);
        AlignedCopy<u32> dest32(std::vector<u32>(count), dest_offset % 4);
        rex::gpu::ReplaceResetIndex16To16(dest16.start, source.start, count, kReset16);
        rex::gpu::ReplaceResetIndex16To32(dest32.start, source.start, count, kReset16);
        INFO("count " << count << " source " << offset << " dest " << dest_offset);
        for (u32 i = 0; i < count; ++i) {
          const bool reset = values[i] == kReset16;
          REQUIRE(dest16.start[i] == (reset ? u16{0xFFFF} : values[i]));
          REQUIRE(dest32.start[i] == (reset ? UINT32_MAX : u32{values[i]}));
        }
      }
    }
  }
}

namespace {

template <rex::xenos::Endian HostSwap>
void CheckReplaceResetIndex32To24() {
  for (const u32 count : kCounts) {
    const auto values = Indices32(count, true);
    for (size_t offset = 0; offset < 4; ++offset) {
      for (size_t dest_offset = 0; dest_offset < 4; ++dest_offset) {
        AlignedCopy<u32> source(values, offset);
        AlignedCopy<u32> dest(std::vector<u32>(count), dest_offset);
        rex::gpu::ReplaceResetIndex32To24<HostSwap>(dest.start, source.start, count, kReset32,
                                                    kMask24);
        INFO("count " << count << " source " << offset << " dest " << dest_offset);
        for (u32 i = 0; i < count; ++i) {
          const u32 index = values[i] & kMask24;
          const u32 want = index != kReset32 ? rex::xenos::GPUSwap(index, HostSwap) : UINT32_MAX;
          REQUIRE(dest.start[i] == want);
        }
      }
    }
  }
}

}  // namespace

REX_TEST_CASE("32 bit reset replacement matches a plain loop for every host swap",
              "[gpu][primitive_processor]") {
  CheckReplaceResetIndex32To24<rex::xenos::Endian::kNone>();
  CheckReplaceResetIndex32To24<rex::xenos::Endian::k8in16>();
  CheckReplaceResetIndex32To24<rex::xenos::Endian::k8in32>();
  CheckReplaceResetIndex32To24<rex::xenos::Endian::k16in32>();
}

REX_TEST_CASE_METHOD(MemoryFixture,
                     "A tessellated triangle strip is drawn as a list that keeps its winding",
                     "[gpu][primitive_processor]") {
  const std::vector<u16> want_auto = {0, 1, 2, 2, 1, 3, 2, 3, 4};
  const std::vector<u16> want_dma = {100, 101, 102, 102, 101, 103, 102, 103, 104};
  for (const bool fans_supported : {false, true}) {
    const auto auto_draw = DrawTessellated(rex::xenos::PrimitiveType::kTriangleStrip,
                                           rex::xenos::SourceSelect::kAutoIndex, 5, fans_supported);
    REQUIRE(auto_draw.processed);
    CHECK(auto_draw.host_primitive_type == rex::xenos::PrimitiveType::kTriangleList);
    CHECK(auto_draw.host_indices == want_auto);

    const auto dma_draw = DrawTessellated(rex::xenos::PrimitiveType::kTriangleStrip,
                                          rex::xenos::SourceSelect::kDMA, 5, fans_supported);
    REQUIRE(dma_draw.processed);
    CHECK(dma_draw.host_primitive_type == rex::xenos::PrimitiveType::kTriangleList);
    CHECK(dma_draw.host_indices == want_dma);
  }
}

REX_TEST_CASE_METHOD(MemoryFixture,
                     "A tessellated triangle fan is drawn as a list with or without a fan buffer",
                     "[gpu][primitive_processor]") {
  const std::vector<u16> want_auto = {1, 2, 0, 2, 3, 0, 3, 4, 0};
  const std::vector<u16> want_dma = {101, 102, 100, 102, 103, 100, 103, 104, 100};
  for (const bool fans_supported : {false, true}) {
    const auto auto_draw = DrawTessellated(rex::xenos::PrimitiveType::kTriangleFan,
                                           rex::xenos::SourceSelect::kAutoIndex, 5, fans_supported);
    REQUIRE(auto_draw.processed);
    CHECK(auto_draw.host_primitive_type == rex::xenos::PrimitiveType::kTriangleList);
    CHECK(auto_draw.host_indices == want_auto);

    const auto dma_draw = DrawTessellated(rex::xenos::PrimitiveType::kTriangleFan,
                                          rex::xenos::SourceSelect::kDMA, 5, fans_supported);
    REQUIRE(dma_draw.processed);
    CHECK(dma_draw.host_primitive_type == rex::xenos::PrimitiveType::kTriangleList);
    CHECK(dma_draw.host_indices == want_dma);
  }
}
