/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_GPU_METAL_METAL_DXBC_CLIP_RELABEL_H_
#define XENIA_GPU_METAL_METAL_DXBC_CLIP_RELABEL_H_

// Candidate-1 mechanism for the geometry-emulation clip-distance bridge
// (openspec/changes/fix-metal-geometry-clip-distance-bridge, task 2).
//
// The audit established that `SV_ClipDistance` is valid as a rasterizer-facing
// output but is rejected as an object-to-mesh PAYLOAD semantic: the reflection
// reserves storage (16 -> 32 bytes), the generated geometry stage declares the
// semantic on both its input and output, every artifact is individually
// well-formed, and only the link is refused.
//
// This relabels the semantic ACROSS THE BOUNDARY ONLY:
//   - the object stage's OUTPUT signature, and
//   - the mesh stage's INPUT signature
// become a collision-free private semantic, while the mesh stage's OUTPUT
// signature keeps `SV_ClipDistance` so the rasterizer still receives it.
//
// It is deliberately NOT a strip. The payload keeps its exact register
// assignment, component mask, semantic index and byte offsets; only the host
// bridge's label for the intermediate hop changes. Values produced by the guest
// vertex program are carried unmodified.
//
// The replacement name is EXACTLY as long as "SV_ClipDistance" (15 characters)
// so the edit is in place: no string table is rebuilt, no offset moves, and the
// container's chunk sizes and total size are untouched. That is what lets the
// transport oracle compare payload bytes before and after.

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "gpu/xenos/format/dxbc.h"

namespace rex {
namespace graphics {
namespace metal {

// Exactly 15 characters, matching "SV_ClipDistance", so the rewrite is in
// place. "XE" prefixed so it cannot collide with a guest interpolator
// semantic, which the translator emits as TEXCOORD.
//
// HAZARD, checked by the transport oracle: DXBC signature chunks may POOL
// identical semantic strings. If a geometry stage's input and output elements
// share one "SV_ClipDistance" string, renaming for the input would also rename
// the output and defeat the whole point of the mechanism. The relabeler is
// therefore applied per chunk, and the oracle asserts the mesh stage's OUTPUT
// still reads SV_ClipDistance afterwards.
inline constexpr const char* kMetalClipPayloadSemantic = "XECLIPTRANSPORT";

// Which signature chunk to rewrite.
enum class DxbcSignatureKind {
  kInput,        // ISGN
  kOutput,       // OSGN
  kOutputGS,     // OSG5 (stream-carrying output, used by geometry stages)
};

namespace dxbc_relabel_detail {

inline uint32_t ReadU32(const std::vector<uint8_t>& b, size_t off) {
  uint32_t v = 0;
  std::memcpy(&v, b.data() + off, sizeof(v));
  return v;
}

inline void WriteU32(std::vector<uint8_t>& b, size_t off, uint32_t v) {
  std::memcpy(b.data() + off, &v, sizeof(v));
}

inline uint32_t FourCC(const char* s) {
  return uint32_t(uint8_t(s[0])) | (uint32_t(uint8_t(s[1])) << 8) |
         (uint32_t(uint8_t(s[2])) << 16) | (uint32_t(uint8_t(s[3])) << 24);
}

}  // namespace dxbc_relabel_detail

// Rewrites clip-distance signature elements in one chunk of a DXBC container.
//
// Returns the number of elements relabeled. The container is modified in place
// and keeps its exact size; `system_value` becomes kUndefined so the element is
// a plain user parameter, and the semantic string is overwritten in place.
//
// Only elements whose system value is kClipDistance are touched. Position,
// cull distance, point size and guest interpolators are left alone, which is
// what keeps the transform free of collisions.
inline uint32_t RelabelDxbcClipDistance(std::vector<uint8_t>& container,
                                        DxbcSignatureKind kind) {
  using namespace dxbc_relabel_detail;
  if (container.size() < sizeof(uint32_t) * 8) {
    return 0;
  }
  const uint32_t chunk_count = ReadU32(container, sizeof(uint32_t) * 7);
  const size_t chunk_table = sizeof(uint32_t) * 8;
  if (chunk_table + chunk_count * sizeof(uint32_t) > container.size()) {
    return 0;
  }

  uint32_t want = 0;
  switch (kind) {
    case DxbcSignatureKind::kInput:
      want = FourCC("ISGN");
      break;
    case DxbcSignatureKind::kOutput:
      want = FourCC("OSGN");
      break;
    case DxbcSignatureKind::kOutputGS:
      want = FourCC("OSG5");
      break;
  }

  const size_t name_length = std::strlen(kMetalClipPayloadSemantic);
  uint32_t relabeled = 0;

  for (uint32_t i = 0; i < chunk_count; ++i) {
    const uint32_t chunk_off =
        ReadU32(container, chunk_table + i * sizeof(uint32_t));
    if (size_t(chunk_off) + sizeof(uint32_t) * 2 > container.size()) continue;
    if (ReadU32(container, chunk_off) != want) continue;

    // Chunk payload begins after the fourcc and size dwords. Signature
    // `semantic_name_ptr` values are relative to this position.
    const size_t data = size_t(chunk_off) + sizeof(uint32_t) * 2;
    if (data + sizeof(uint32_t) * 2 > container.size()) continue;
    const uint32_t param_count = ReadU32(container, data);
    const uint32_t param_ptr = ReadU32(container, data + sizeof(uint32_t));

    // OSG5 carries a leading stream dword per parameter.
    const size_t stride = (kind == DxbcSignatureKind::kOutputGS)
                              ? sizeof(::rex::gpu::dxbc::SignatureParameterForGS)
                              : sizeof(::rex::gpu::dxbc::SignatureParameter);
    const size_t sv_offset_in_param =
        (kind == DxbcSignatureKind::kOutputGS)
            ? sizeof(uint32_t) * 3   // stream, name_ptr, semantic_index
            : sizeof(uint32_t) * 2;  // name_ptr, semantic_index
    const size_t name_offset_in_param =
        (kind == DxbcSignatureKind::kOutputGS) ? sizeof(uint32_t)
                                              : 0;

    for (uint32_t p = 0; p < param_count; ++p) {
      const size_t param = data + param_ptr + size_t(p) * stride;
      if (param + stride > container.size()) break;
      const uint32_t system_value = ReadU32(container, param + sv_offset_in_param);
      if (system_value != uint32_t(::rex::gpu::dxbc::SystemValueName::kClipDistance)) continue;

      // Demote to a plain user parameter. Register index, component mask,
      // semantic index and component type are all left untouched, so the
      // payload layout is bit-identical.
      WriteU32(container, param + sv_offset_in_param,
               uint32_t(::rex::gpu::dxbc::SystemValueName::kUndefined));

      const uint32_t name_ptr = ReadU32(container, param + name_offset_in_param);
      const size_t name_pos = data + name_ptr;
      if (name_pos + name_length + 1 > container.size()) continue;
      // In-place, same length, NUL preserved.
      std::memcpy(container.data() + name_pos, kMetalClipPayloadSemantic,
                  name_length);
      container[name_pos + name_length] = '\0';
      ++relabeled;
    }
  }
  return relabeled;
}

}  // namespace metal
}  // namespace gpu
}  // namespace rex

#endif  // XENIA_GPU_METAL_METAL_DXBC_CLIP_RELABEL_H_
