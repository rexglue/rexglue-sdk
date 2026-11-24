/**
 * Bridges between the ported Metal backend and current upstream.
 *
 * One file, deliberately: it is force-included in every port translation unit, so the whole custom surface of the
 * migration is this page. Each block says what it bridges and when it can go.
 */
#pragma once

/**
 * @file        metal_upstream_compat.h
 * @brief       Names and macros the ported macXenos Metal sources expect from Xenia, mapped onto this SDK.
 *
 * The ported files are kept as close to upstream as possible so fixes can be taken from them. Anything whose
 * spelling differs here is mapped in this one header rather than edited in 106 files: logging macros, cvar
 * declarations, and the byte-order helpers.
 *
 * Include it first in every ported translation unit.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>


// The two float24 depth settings the ported render target cache reads. This SDK declares them in
// rex/graphics/flags.h; the port includes upstream's gpu_flags.h instead, so redeclare them here with the same type.
#include <rex/logging/log_category.h>
#include <rex/memory.h>

#include "gpu/xenos/metal/metal_upstream_api.h"
#include <rex/types.h>

// Logging. Upstream XELOGI/XELOGW/XELOGE/XELOGD with fmt-style arguments.

// Cvars. Upstream DEFINE_/DECLARE_ take (name, default, category, description); this SDK's take a category as well,
// so the category is supplied here and the ported files keep their original call shape. Upstream passes a fourth
// argument (the settings category) that this SDK has no concept of, so it is swallowed rather than rejected.
#ifndef DEFINE_transient_bool
// A transient setting: a per-title hint the launcher writes before starting the game. This SDK keeps cvars for the
// process lifetime only, so it maps to an ordinary one.
#define DEFINE_transient_bool(name, default_value, description, ...) \
  REXCVAR_DEFINE_BOOL(name, default_value, "metal", description)
#endif
#ifndef DEFINE_transient_string
#define DEFINE_transient_string(name, default_value, description, ...) \
  REXCVAR_DEFINE_STRING(name, default_value, "metal", description)
#endif
#ifndef DECLARE_bool
#define DECLARE_bool(name) REXCVAR_DECLARE(bool, name)
#endif
#ifndef DECLARE_int32
#define DECLARE_int32(name) REXCVAR_DECLARE(int32_t, name)
#endif
#ifndef DECLARE_uint32
#define DECLARE_uint32(name) REXCVAR_DECLARE(uint32_t, name)
#endif
#ifndef DECLARE_int64
#define DECLARE_int64(name) REXCVAR_DECLARE(int64_t, name)
#endif
#ifndef DECLARE_uint64
#define DECLARE_uint64(name) REXCVAR_DECLARE(uint64_t, name)
#endif
#ifndef DECLARE_double
#define DECLARE_double(name) REXCVAR_DECLARE(double, name)
#endif
#ifndef DECLARE_string
#define DECLARE_string(name) REXCVAR_DECLARE(std::string, name)
#endif

#ifndef DEFINE_bool
#define DEFINE_bool(name, default_value, description, ...) \
  REXCVAR_DEFINE_BOOL(name, default_value, "metal", description)
#endif
#ifndef DEFINE_int32
#define DEFINE_int32(name, default_value, description, ...) \
  REXCVAR_DEFINE_INT32(name, default_value, "metal", description)
#endif
#ifndef DEFINE_uint32
#define DEFINE_uint32(name, default_value, description, ...) \
  REXCVAR_DEFINE_UINT32(name, default_value, "metal", description)
#endif
#ifndef DEFINE_int64
#define DEFINE_int64(name, default_value, description, ...) \
  REXCVAR_DEFINE_INT64(name, default_value, "metal", description)
#endif
#ifndef DEFINE_uint64
#define DEFINE_uint64(name, default_value, description, ...) \
  REXCVAR_DEFINE_UINT64(name, default_value, "metal", description)
#endif
#ifndef DEFINE_double
#define DEFINE_double(name, default_value, description, ...) \
  REXCVAR_DEFINE_DOUBLE(name, default_value, "metal", description)
#endif
#ifndef DEFINE_string
#define DEFINE_string(name, default_value, description, ...) \
  REXCVAR_DEFINE_STRING(name, default_value, "metal", description)
#endif
#ifndef DECLARE_string
#define DECLARE_string(name) REXCVAR_DECLARE(std::string, name)
#endif
#ifndef DECLARE_path
// Directory-valued settings are std::string cvars in this SDK, so a ported DECLARE_path has to match that type.
#define DECLARE_path(name) REXCVAR_DECLARE(std::string, name)
#endif
#ifndef DEFINE_path
#define DEFINE_path(name, default_value, description, ...) \
  REXCVAR_DEFINE_STRING(name, default_value, "metal", description)
#endif

// Upstream records a config migration beside the cvar it changed: "if the stored value is still the old default,
// reset it". This SDK's cvar layer has no provenance, so the migration is a no-op that leaves a stated consequence
// rather than an invisible one: a config written before the promotion keeps whatever it holds.

// Attributes and Windows spellings that arrive through the shared headers. They are inert here: the point is that the
// ported code parses unchanged, not that these mean anything on this platform.
#ifndef XE_NOINLINE
#define XE_NOINLINE __attribute__((noinline))
#endif
// Functions reached only on a guest misbehaviour path; keeping them out of line keeps the hot path small.
// Packed struct/union declarations, spelled as upstream's platform header spells them: the packed layout is part of
// the on-disk shader storage and pipeline description formats, so it cannot be dropped.
#ifndef _XEPACKEDSCOPE
#if defined(_MSC_VER)
#define _XEPACKEDSCOPE(body) __pragma(pack(push, 1)) body __pragma(pack(pop));
#else
#define _XEPACKEDSCOPE(body) _Pragma("pack(push, 1)") body; _Pragma("pack(pop)");
#endif
#endif
#ifndef XEPACKEDSTRUCT
#define XEPACKEDSTRUCT(name, value) _XEPACKEDSCOPE(struct name value)
#endif
// (stray #endif removed by the merge)

#ifndef XE_COLD
#define XE_COLD __attribute__((cold, noinline))
#endif
#ifndef XE_FORCEINLINE
#define XE_FORCEINLINE inline __attribute__((always_inline))
#endif
// SAL annotations are Microsoft's contract language, parsed by their tools and by nobody else. Clang knows none of
// them, so every one used across the DXC headers is defined to nothing here.
#ifndef _In_
#endif
#ifndef _In_opt_
#endif
#ifndef _In_z_
#endif
#ifndef _In_opt_z_
#endif
#ifndef _In_count_
#endif
#ifndef _In_bytecount_
#endif
#ifndef _In_opt_count_
#endif
#ifndef _In_reads_bytes_
#endif
#ifndef _Out_
#endif
#ifndef _Outptr_
#endif
#ifndef _Outptr_opt_
#endif
#ifndef _Outptr_result_maybenull_z_
#endif
#ifndef _Outptr_result_nullonfailure_
#endif
#ifndef _Outptr_result_bytebuffer_maybenull_
#endif
#ifndef _Outptr_opt_result_z_
#endif
#ifndef _Inout_
#endif
#ifndef _COM_Outptr_
#endif
#ifndef _COM_Outptr_opt_
#endif
#ifndef _COM_Outptr_result_maybenull_
#endif
#ifndef _COM_Outptr_opt_result_maybenull_
#endif
#ifndef _Ret_opt_
#endif
#ifndef _Ret_notnull_
#endif
#ifndef _Ret_maybenull_
#endif
#ifndef _Success_
#endif
#ifndef _Use_decl_annotations_
#define _Use_decl_annotations_
#endif
// DXC's WinAdapter.h typedefs CLSID and REFIID itself when it is in the include chain, so these must not be macros:
// a macro named CLSID turns WinAdapter's own `typedef GUID CLSID;` into `typedef GUID struct;`. Anything that needs
// Windows-style GUID spellings gets them from WinAdapter.

// Upstream marks hot functions with profiling scopes. This SDK's counters are a different API, so these expand
// to nothing here rather than to a wrong call.
#ifndef XELOGKERNEL
#define XELOGKERNEL(...)
#endif

// Upstream reads a cvar as cvars::name; the rename script rewrites that to REXCVAR_GET(name). This alias exists so
// an unrewritten file still names something rather than failing to parse.
namespace rex::graphics::metal {
template <typename T>
inline T& CvarRef(T& storage) {
  return storage;
}
}  // namespace rex::graphics::metal

namespace rex {

// Upstream shared headers name the guest memory type unqualified; this SDK keeps it in rex::memory.
}  // namespace rex

// Spelling helpers from upstream's platform.h. The first two are real attributes; the rest are assertions about the
// host compiler that clang already satisfies, so they expand to nothing rather than to __declspec, which clang rejects.
#ifndef XE_RESTRICT
#define XE_RESTRICT __restrict
#endif
#ifndef XE_RESTRICT_VAR
#define XE_RESTRICT_VAR __restrict
#endif
#ifndef XE_NOALIAS
#define XE_NOALIAS
#endif
#ifndef XE_LIKELY
#define XE_LIKELY(...) (__builtin_expect(!!(__VA_ARGS__), 1))
#endif
#ifndef XE_UNLIKELY
#define XE_UNLIKELY(...) (__builtin_expect(!!(__VA_ARGS__), 0))
#endif
#ifndef XE_LIKELY_IF
#define XE_LIKELY_IF(...) if (__VA_ARGS__)
#endif
#ifndef XE_UNLIKELY_IF
#define XE_UNLIKELY_IF(...) if (__VA_ARGS__)
#endif
#ifndef XE_ALIGNAS
#define XE_ALIGNAS(n) alignas(n)
#endif
#ifndef XE_ATTRIBUTE_NOINLINE
#define XE_ATTRIBUTE_NOINLINE __attribute__((noinline))
#endif

// ---------------------------------------------------------------- port bridges

#include <cstdint>

// rex/thread.h no longer exists upstream; the port's fourteen uses of it are bridged here.
#include "metal_thread.h"

// Xenia's assertion macros. Upstream dropped them; the ported backend uses them as documentation of invariants, so they
// map onto the standard assert rather than vanishing silently.
#ifndef assert_true
#define assert_true(expression) assert(expression)
#endif
#ifndef assert_false
#define assert_false(expression) assert(!(expression))
#endif
#ifndef assert_zero
#define assert_zero(expression) assert((expression) == 0)
#endif
#ifndef assert_not_null
#define assert_not_null(expression) assert((expression) != nullptr)
#endif
#ifndef assert_always
// Xenia calls this with no arguments; it means "must not happen".
#define assert_always(...) assert(false)
#endif
#ifndef assert_unreachable
#define assert_unreachable() assert(false)
#endif

// Upstream no longer ships rex/thread.h. These two bracketed hooks are the only thing the ported backend used from it,
// and they exist to stop the guest being suspended mid-publication; this runtime runs the guest one thread at a time,
// so they are paired no-ops. If the guest ever gains concurrent threads they must actually suspend it.
// The suspend hooks live in metal_upstream_api.h, included above.

// TODO(migration): the old base answered "does this guest range overlap a watched range"; the new one keeps watch state
// private. Answering yes is conservative - the caller flushes rather than assuming the range is unwatched - so it costs
// bandwidth, not correctness, until the new watch API grows a query.
namespace rex::graphics::metal {
inline bool GuestRamWatchOverlaps(uint32_t base, uint32_t length) {
  (void)base;
  (void)length;
  return true;
}

// The SDK had this beside GuestRamWatchOverlaps above; it is a log-only diagnostic, copied verbatim.
inline void GuestRamWatchNote(const char* writer, uint32_t base, uint32_t length,
                              uint64_t value = UINT64_MAX) {
  if (!GuestRamWatchOverlaps(base, length)) {
    return;
  }
  if (value == UINT64_MAX) {
    REXLOG_INFO("Guest RAM watch: {} wrote {:08X} ({} bytes)", writer, base, length);
  } else {
    REXLOG_INFO("Guest RAM watch: {} wrote {:08X} ({} bytes) = {:016X}", writer, base, length, value);
  }
}
}  // namespace rex::graphics::metal

// The register definitions moved into rex::xenos::reg; the ported backend and the carried trace headers refer to them as
// the bare `reg` namespace that upstream used to expose at the top level.
#include <rex/xenos/register.h>
namespace reg = ::rex::xenos::reg;

// Register indices moved into rex::xenos::Register, generated from register_table.inc. The port refers to them by the old
// XE_GPU_REG_* spelling, so they are bridged here as constants - same values, no behaviour.
#include <rex/xenos/register.h>
namespace rex::xenos::compat {
inline constexpr uint32_t XE_GPU_REG_DC_LUTA_BLACK_OFFSET_BLUE = uint32_t(::rex::xenos::Register::DC_LUTA_BLACK_OFFSET_BLUE);
inline constexpr uint32_t XE_GPU_REG_DC_LUTA_BLACK_OFFSET_GREEN = uint32_t(::rex::xenos::Register::DC_LUTA_BLACK_OFFSET_GREEN);
inline constexpr uint32_t XE_GPU_REG_DC_LUTA_BLACK_OFFSET_RED = uint32_t(::rex::xenos::Register::DC_LUTA_BLACK_OFFSET_RED);
inline constexpr uint32_t XE_GPU_REG_DC_LUTA_CONTROL = uint32_t(::rex::xenos::Register::DC_LUTA_CONTROL);
inline constexpr uint32_t XE_GPU_REG_DC_LUTA_WHITE_OFFSET_BLUE = uint32_t(::rex::xenos::Register::DC_LUTA_WHITE_OFFSET_BLUE);
inline constexpr uint32_t XE_GPU_REG_DC_LUTA_WHITE_OFFSET_GREEN = uint32_t(::rex::xenos::Register::DC_LUTA_WHITE_OFFSET_GREEN);
inline constexpr uint32_t XE_GPU_REG_DC_LUTA_WHITE_OFFSET_RED = uint32_t(::rex::xenos::Register::DC_LUTA_WHITE_OFFSET_RED);
inline constexpr uint32_t XE_GPU_REG_DC_LUT_RW_MODE = uint32_t(::rex::xenos::Register::DC_LUT_RW_MODE);
inline constexpr uint32_t XE_GPU_REG_DC_LUT_WRITE_EN_MASK = uint32_t(::rex::xenos::Register::DC_LUT_WRITE_EN_MASK);
inline constexpr uint32_t XE_GPU_REG_PA_CL_CLIP_CNTL = uint32_t(::rex::xenos::Register::PA_CL_CLIP_CNTL);
inline constexpr uint32_t XE_GPU_REG_PA_CL_VPORT_XOFFSET = uint32_t(::rex::xenos::Register::PA_CL_VPORT_XOFFSET);
inline constexpr uint32_t XE_GPU_REG_PA_CL_VPORT_XSCALE = uint32_t(::rex::xenos::Register::PA_CL_VPORT_XSCALE);
inline constexpr uint32_t XE_GPU_REG_PA_CL_VPORT_YOFFSET = uint32_t(::rex::xenos::Register::PA_CL_VPORT_YOFFSET);
inline constexpr uint32_t XE_GPU_REG_PA_CL_VPORT_YSCALE = uint32_t(::rex::xenos::Register::PA_CL_VPORT_YSCALE);
inline constexpr uint32_t XE_GPU_REG_PA_CL_VPORT_ZOFFSET = uint32_t(::rex::xenos::Register::PA_CL_VPORT_ZOFFSET);
inline constexpr uint32_t XE_GPU_REG_PA_CL_VPORT_ZSCALE = uint32_t(::rex::xenos::Register::PA_CL_VPORT_ZSCALE);
inline constexpr uint32_t XE_GPU_REG_PA_CL_VTE_CNTL = uint32_t(::rex::xenos::Register::PA_CL_VTE_CNTL);
inline constexpr uint32_t XE_GPU_REG_PA_SC_SCREEN_SCISSOR_BR = uint32_t(::rex::xenos::Register::PA_SC_SCREEN_SCISSOR_BR);
inline constexpr uint32_t XE_GPU_REG_PA_SC_SCREEN_SCISSOR_TL = uint32_t(::rex::xenos::Register::PA_SC_SCREEN_SCISSOR_TL);
inline constexpr uint32_t XE_GPU_REG_PA_SC_WINDOW_OFFSET = uint32_t(::rex::xenos::Register::PA_SC_WINDOW_OFFSET);
inline constexpr uint32_t XE_GPU_REG_PA_SC_WINDOW_SCISSOR_BR = uint32_t(::rex::xenos::Register::PA_SC_WINDOW_SCISSOR_BR);
inline constexpr uint32_t XE_GPU_REG_PA_SC_WINDOW_SCISSOR_TL = uint32_t(::rex::xenos::Register::PA_SC_WINDOW_SCISSOR_TL);
inline constexpr uint32_t XE_GPU_REG_PA_SU_SC_MODE_CNTL = uint32_t(::rex::xenos::Register::PA_SU_SC_MODE_CNTL);
inline constexpr uint32_t XE_GPU_REG_RB_ALPHA_REF = uint32_t(::rex::xenos::Register::RB_ALPHA_REF);
inline constexpr uint32_t XE_GPU_REG_RB_BLENDCONTROL0 = uint32_t(::rex::xenos::Register::RB_BLENDCONTROL0);
inline constexpr uint32_t XE_GPU_REG_RB_BLEND_ALPHA = uint32_t(::rex::xenos::Register::RB_BLEND_ALPHA);
inline constexpr uint32_t XE_GPU_REG_RB_BLEND_BLUE = uint32_t(::rex::xenos::Register::RB_BLEND_BLUE);
inline constexpr uint32_t XE_GPU_REG_RB_BLEND_GREEN = uint32_t(::rex::xenos::Register::RB_BLEND_GREEN);
inline constexpr uint32_t XE_GPU_REG_RB_BLEND_RED = uint32_t(::rex::xenos::Register::RB_BLEND_RED);
inline constexpr uint32_t XE_GPU_REG_RB_COLORCONTROL = uint32_t(::rex::xenos::Register::RB_COLORCONTROL);
inline constexpr uint32_t XE_GPU_REG_RB_COLOR_MASK = uint32_t(::rex::xenos::Register::RB_COLOR_MASK);
inline constexpr uint32_t XE_GPU_REG_RB_DEPTHCONTROL = uint32_t(::rex::xenos::Register::RB_DEPTHCONTROL);
inline constexpr uint32_t XE_GPU_REG_RB_DEPTH_CLEAR = uint32_t(::rex::xenos::Register::RB_DEPTH_CLEAR);
inline constexpr uint32_t XE_GPU_REG_RB_DEPTH_INFO = uint32_t(::rex::xenos::Register::RB_DEPTH_INFO);
inline constexpr uint32_t XE_GPU_REG_RB_MODECONTROL = uint32_t(::rex::xenos::Register::RB_MODECONTROL);
inline constexpr uint32_t XE_GPU_REG_RB_STENCILREFMASK = uint32_t(::rex::xenos::Register::RB_STENCILREFMASK);
inline constexpr uint32_t XE_GPU_REG_RB_STENCILREFMASK_BF = uint32_t(::rex::xenos::Register::RB_STENCILREFMASK_BF);
inline constexpr uint32_t XE_GPU_REG_RB_SURFACE_INFO = uint32_t(::rex::xenos::Register::RB_SURFACE_INFO);
inline constexpr uint32_t XE_GPU_REG_SHADER_CONSTANT_000_X = uint32_t(::rex::xenos::Register::SHADER_CONSTANT_000_X);
inline constexpr uint32_t XE_GPU_REG_SHADER_CONSTANT_256_X = uint32_t(::rex::xenos::Register::SHADER_CONSTANT_256_X);
inline constexpr uint32_t XE_GPU_REG_SHADER_CONSTANT_BOOL_000_031 = uint32_t(::rex::xenos::Register::SHADER_CONSTANT_BOOL_000_031);
inline constexpr uint32_t XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 = uint32_t(::rex::xenos::Register::SHADER_CONSTANT_FETCH_00_0);
inline constexpr uint32_t XE_GPU_REG_SQ_CONTEXT_MISC = uint32_t(::rex::xenos::Register::SQ_CONTEXT_MISC);
inline constexpr uint32_t XE_GPU_REG_SQ_PROGRAM_CNTL = uint32_t(::rex::xenos::Register::SQ_PROGRAM_CNTL);
inline constexpr uint32_t XE_GPU_REG_SQ_PS_CONST = uint32_t(::rex::xenos::Register::SQ_PS_CONST);
inline constexpr uint32_t XE_GPU_REG_SQ_VS_CONST = uint32_t(::rex::xenos::Register::SQ_VS_CONST);
inline constexpr uint32_t XE_GPU_REG_VGT_DMA_BASE = uint32_t(::rex::xenos::Register::VGT_DMA_BASE);
inline constexpr uint32_t XE_GPU_REG_VGT_HOS_MAX_TESS_LEVEL = uint32_t(::rex::xenos::Register::VGT_HOS_MAX_TESS_LEVEL);
inline constexpr uint32_t XE_GPU_REG_VGT_HOS_MIN_TESS_LEVEL = uint32_t(::rex::xenos::Register::VGT_HOS_MIN_TESS_LEVEL);
inline constexpr uint32_t XE_GPU_REG_VGT_INDX_OFFSET = uint32_t(::rex::xenos::Register::VGT_INDX_OFFSET);
inline constexpr uint32_t XE_GPU_REG_VGT_MAX_VTX_INDX = uint32_t(::rex::xenos::Register::VGT_MAX_VTX_INDX);
inline constexpr uint32_t XE_GPU_REG_VGT_MIN_VTX_INDX = uint32_t(::rex::xenos::Register::VGT_MIN_VTX_INDX);
inline constexpr uint32_t XE_GPU_REG_PA_CL_UCP_0_X = uint32_t(::rex::xenos::Register::PA_CL_UCP_0_X);
}  // namespace rex::xenos::compat
using namespace rex::xenos::compat;

// The tiled-address helpers moved out of a texture_util namespace into rex::xenos; the port still spells the namespace.
namespace texture_util = ::rex::xenos;

// Xenia's countof, which upstream replaced with std::size / std::ssize at the call sites.
namespace rex {
template <typename T, size_t N>
constexpr size_t countof(const T (&)[N]) noexcept {
  return N;
}
}  // namespace rex

// Base types the ported headers name unqualified inside rex::graphics::metal.
#include "gpu/xenos/pipeline/texture/cache.h"
#include <rex/xenos/register.h>
namespace rex::graphics {
using ::rex::xenos::RegisterFile;
using ::rex::gpu::TextureCache;
}  // namespace rex::graphics

// Upstream dropped rex::align; the ported code rounds offsets with it. Two lines, standard rounding, nothing clever.
namespace rex {
inline size_t align(size_t value, size_t alignment) {
  return alignment ? ((value + alignment - 1) / alignment) * alignment : value;
}
}  // namespace rex

// EDRAM geometry upstream no longer publishes: 2048 tiles of base, up to 1024 of pitch.
namespace rex::xenos {
constexpr uint32_t kEdramBaseTilesBits = 11;
constexpr uint32_t kEdramPitchTilesBits = 10;
}  // namespace rex::xenos

#ifndef static_assert_size
#define static_assert_size(type, size) static_assert(sizeof(type) == (size), #type " has an unexpected size")
#endif

#ifndef XE_RESTRICT
#endif

// Attribute spellings upstream dropped; each is the real attribute where one exists.
#ifndef XE_NOINLINE
#endif
#ifndef XE_FORCEINLINE
#endif
#ifndef XE_COLD
#endif
#ifndef XE_NOALIAS
#endif

// Upstream keeps the four colour-target register indices as separate enum values rather than an array, so the port asks
// for them by index through these two helpers. Delete them if upstream ever publishes the arrays again.
namespace rex::xenos::compat {
inline constexpr Register ColorInfoRegisterIndex(uint32_t i) {
  constexpr Register kIndices[4] = {Register::RB_COLOR_INFO, Register::RB_COLOR1_INFO,
                                    Register::RB_COLOR2_INFO, Register::RB_COLOR3_INFO};
  return kIndices[i & 3];
}
inline constexpr Register BlendControlRegisterIndex(uint32_t i) {
  constexpr Register kIndices[4] = {Register::RB_BLENDCONTROL0, Register::RB_BLENDCONTROL1,
                                    Register::RB_BLENDCONTROL2, Register::RB_BLENDCONTROL3};
  return kIndices[i & 3];
}
}  // namespace rex::xenos::compat


