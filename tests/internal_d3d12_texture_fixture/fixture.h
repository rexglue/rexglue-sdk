#pragma once

// Private test protocol. This header is never installed or part of the plugin ABI.
#include <array>
#include <cstdint>

namespace rex::graphics::d3d12::fixture {
enum class Phase : uint32_t {
  kWarmup,
  kSeedA,
  kProducerB,
  kReloadB,
  kRepeat,
  kC,
  kRetryB,
  kTerminal,
  kRetired
};
enum class Kind : uint32_t {
  kEntry,
  kSeam,
  kSuccess,
  kIdentity,
  kSchedule,
  kView,
  kBindings,
  kUse,
  kSeal,
  kArm,
  kDraw,
  kCopy,
  kRejected
};
struct Config {
  uint32_t version = 2;
  uint32_t fetch[6] = {};
  uint64_t run = 0;
  uint32_t fail = 0;
  uint32_t independent = 0;
  uint32_t receipt = 0;
};
struct TextureValue {
  uint64_t instance = 0, generation = 0, resource = 0;
  uint32_t dirty = 0, key_size = 0;
  std::array<uint8_t, 32> key{};
  uint32_t base_address = 0, base_size = 0, mip_address = 0, mip_size = 0;
  uint32_t host_width = 0, host_height = 0, host_depth = 0, host_mips = 0, host_format = 0;
};
struct Receipt {
  Kind kind{};
  Phase phase{};
  uint64_t sequence = 0, event = 0, frame = 0, submission = 0;
  TextureValue texture{};
  uint64_t a = 0, b = 0, c = 0, d = 0;
};
enum class TraceKind : uint32_t {
  kOwner = 1,
  kCopyBegin,
  kCopyResolve,
  kCopyEnd,
  kCopyObserver,
  kDeclaration,
  kPrepare,
  kNoop,
  kBackendAttempt,
  kBackendFailure,
  kLoadSuccess,
  kBatchRequestFailure,
  kDirectRequestFailure,
  kInvalidation,
  kLifecycle,
  kInterference,
  kDrawContext,
  kDrawObserver,
  kView,
  kBinding,
  kUse,
  kTerminal,
  kResidencyFailure,
  kPrepareBegin
};
struct TraceRow {
  TraceKind kind{};
  Phase phase{};
  uint64_t sequence = 0, cp = 0, cache = 0, shared = 0;
  uint64_t occurrence = 0, owned = 0, observer = 0, token = 0;
  uint64_t frame = 0, submission = 0;
  TextureValue texture{};
  uint64_t generation_after = 0, entry_generation = 0;
  uint64_t a = 0, b = 0, c = 0, d = 0;
};
struct TraceReport {
  uint32_t enabled = 0, closed = 0, incomplete = 0, count = 0;
  uint64_t lost = 0, first_lost = 0;
  std::array<TraceRow, 2048> rows{};
};
struct Report {
  Config config{};
  uint32_t error = 0, count = 0, post_seed_attempts = 0, injected = 0, ordinal = 0;
  uint32_t selected = 0, sealed = 0;
  TextureValue seed{};
  std::array<Receipt, 256> receipts{};
  TraceReport trace{};
};
bool TraceEnabled() noexcept;
void TraceBind(const void* cp, const void* cache, const void* shared) noexcept;
void TraceContext(const void* cp, uint64_t owned, uint64_t frame, uint64_t submission) noexcept;
void TraceLoad(const void* cache, TraceKind kind, const TextureValue& texture,
               uint64_t generation_after, bool base, bool mips, uint64_t entry_generation = 0,
               uint64_t flags = 0) noexcept;
void TraceDeclaration(const void* cache, uint32_t address, uint32_t length, bool scaled) noexcept;
void TraceInvalidation(const void* shared, uint32_t first, uint32_t last, bool gpu) noexcept;
void TraceLifecycle(const void* owner, uint32_t cause) noexcept;
void TraceInterference(const void* cp, uint32_t address, uint32_t length, bool unknown) noexcept;
void TraceCopyBegin(const void* cp, uint64_t owned, uint64_t frame, uint64_t submission) noexcept;
void TraceCopyResolve(const void* cp, uint64_t frame, uint64_t submission) noexcept;
void TraceCopyEnd(const void* cp, bool result, uint64_t frame, uint64_t submission,
                  uint32_t address, uint32_t length) noexcept;
void TraceObserver(const void* cp, bool copy, bool prepared, uint64_t observer, uint64_t token,
                   uint64_t frame, uint64_t submission, bool accepted, bool result,
                   uint32_t address = 0, uint32_t length = 0) noexcept;
// All implementation and mutable storage live in the fixture module.
bool Enabled() noexcept;
const Config& GetConfig() noexcept;
const TextureValue* Seed() noexcept;
void Error(uint32_t code) noexcept;
bool Owner(const void* cp, const void* cache) noexcept;
void TextureAudit(Kind kind, const TextureValue& value, uint64_t a = 0, uint64_t b = 0) noexcept;
bool Fail(const TextureValue& value, bool base, bool mips) noexcept;
void View(const TextureValue& value, uint32_t fetch, bool sign, bool real, uint64_t descriptor,
          uint64_t destination) noexcept;
void Bindings(const void* cp) noexcept;
bool Select(const void* cp, uint64_t event, uint32_t stage, uint32_t binding, uint32_t fetch,
            bool sign, uint64_t instance, uint64_t generation) noexcept;
void Use(uint64_t event, uint64_t index, uint64_t buffer, uint64_t bytes, bool reused) noexcept;
void Rejected(uint64_t event, uint64_t map_size, uint64_t captures, uint64_t bytes) noexcept;
void DrawContext(const void* cp, uint64_t event, uint64_t frame, uint64_t submission) noexcept;
}  // namespace rex::graphics::d3d12::fixture

#if defined(REX_INTERNAL_D3D12_TEXTURE_FIXTURE_MODULE)
#define REX_TEXTURE_FIXTURE_EXPORT __declspec(dllexport)
#else
#define REX_TEXTURE_FIXTURE_EXPORT
#endif
extern "C" {
// Resolved dynamically from the explicitly staged fixture DLL before publication.
REX_TEXTURE_FIXTURE_EXPORT bool RexTextureFixtureConfigure(
    const rex::graphics::d3d12::fixture::Config*) noexcept;
REX_TEXTURE_FIXTURE_EXPORT bool RexTextureFixtureAdvance(uint32_t command, uint64_t event,
                                                         uint64_t frame,
                                                         uint64_t submission) noexcept;
REX_TEXTURE_FIXTURE_EXPORT bool RexTextureFixtureCopyReport(
    rex::graphics::d3d12::fixture::Report*) noexcept;
REX_TEXTURE_FIXTURE_EXPORT bool RexTextureFixtureRetire() noexcept;
}
