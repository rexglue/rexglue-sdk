/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_GPU_METAL_METAL_CONVERTER_OPTIONS_H_
#define XENIA_GPU_METAL_METAL_CONVERTER_OPTIONS_H_

// The single definition of the dxilconv option string
// (adopt-xenios-shader-artifact-caching task 2.1, item 3 — option/fingerprint
// key coverage).
//
// WHY THIS IS ITS OWN HEADER: the option string is an INPUT to DXBC->DXIL
// conversion that changes the emitted DXIL for byte-identical guest ucode.
// Measured 2026-08-05 on the quad-domain conformance payload: the default
// "-skip-container-parts" yields a 3644-byte DXIL module, an empty option
// string yields 4436 bytes from the same DXBC. Anything downstream of that
// conversion — the metallib, and the DXIL artifacts the cache stages below add
// — therefore differs too.
//
// Two consumers must agree on the effective string and cannot be allowed to
// drift: DxbcToDxilConverter, which passes it to IDxbcConverter::Convert, and
// ComputeMetalPipelineStorageIdentity, which fingerprints it so a durable cache
// written under one option string is never served under another. Before this
// header the string lived only inside the converter, so no cache key could see
// it — the same shape as the two stale-artifact hazards already recorded in
// metal_pipeline_storage.h (the clip relabel, which kFetchAbiRevision closes,
// and the dxilconv converter-reuse fix, which kDxilconvGeneration closes).
//
// Header-only and dependency-free on purpose: metal_pipeline_storage.mm is
// linked into targets that deliberately do not link libdxilconv, so it must be
// able to fingerprint the options without pulling the converter in.

#include <cstdlib>
#include <string>

namespace rex {
namespace graphics {
namespace metal {

// -skip-container-parts drops the DXBC container parts that don't survive the
// DXIL round-trip and that the Metal Shader Converter does not consume, keeping
// the output to the DXIL module we care about.
inline constexpr char kMetalDxbcToDxilDefaultOptions[] =
    "-skip-container-parts";

// Debug override. Setting it changes generated code, so it also changes the
// durable-storage identity — a session run with an override keys its own cache
// directory contents rather than poisoning the default one.
inline constexpr char kMetalDxbcToDxilOptionsEnvVar[] = "XENIA_DXBC2DXIL_FLAGS";

// The option string this process will actually pass to dxilconv. Read from the
// environment on every call rather than cached, so there is no ordering
// dependency between converter construction and identity computation.
inline std::string MetalDxbcToDxilEffectiveOptions() {
  if (const char* env = std::getenv(kMetalDxbcToDxilOptionsEnvVar)) {
    return std::string(env);
  }
  return std::string(kMetalDxbcToDxilDefaultOptions);
}

// Metal Shader Converter IRCompatibilityFlagPositionInvariance: marks the
// vertex position output invariant so separately compiled shaders computing the
// same position produce bit-identical depth (multi-pass depth EQUAL). Changes
// the metallib, so it is part of the durable-storage identity.
// XE_METAL_POSITION_INVARIANCE=1 enables it.
inline bool MetalPositionInvarianceEnabled() {
  const char* v = std::getenv("XE_METAL_POSITION_INVARIANCE");
  return v && v[0] == '1';
}

// The metallib-affecting converter options, as fingerprinted by the durable
// pipeline storage identity. Unchanged from the dxilconv string by default so
// the default identity (and cache) is unaffected.
inline std::string MetalConverterIdentityOptions() {
  std::string options = MetalDxbcToDxilEffectiveOptions();
  if (MetalPositionInvarianceEnabled()) {
    options += "|msc-position-invariance";
  }
  return options;
}

}  // namespace metal
}  // namespace gpu
}  // namespace rex

#endif  // XENIA_GPU_METAL_METAL_CONVERTER_OPTIONS_H_
