#include "fixture.h"
#include <cstring>
#include <atomic>
#include <mutex>
#include <windows.h>

namespace rex::graphics::d3d12::fixture {
namespace {
struct Controller {
  Report report{};
  bool configured = false, retired = false, armed = false;
  Phase phase = Phase::kWarmup;
  const void* cp = nullptr;
  const void* cache = nullptr;
  DWORD thread = 0;
  uint64_t event = 0, frame = 0, submission = 0, epoch = 0, success_epoch = 0;
  uint64_t view_epoch = 0, bound_epoch = 0;
  TextureValue latest{}, current{}, view{};
  bool real_view = false;
} state;
// The memory callback only enters this recorder. It never touches Controller,
// follows borrowed objects, or calls code that can acquire the memory lock.
struct TraceState {
  std::mutex mutex;
  std::atomic<bool> enabled{false};
  TraceReport report{};
  uint64_t cp = 0, cache = 0, shared = 0;
  uint64_t occurrence = 0, next_occurrence = 0, owned = 0, frame = 0, submission = 0;
  Phase phase = Phase::kWarmup;
} trace;
uint64_t Identity(const void* p) noexcept {
  return reinterpret_cast<uintptr_t>(p);
}
TraceRow Row(TraceKind kind) noexcept {
  TraceRow row;
  row.kind = kind;
  row.phase = trace.phase;
  row.cp = trace.cp;
  row.cache = trace.cache;
  row.shared = trace.shared;
  row.occurrence = trace.occurrence;
  row.owned = trace.owned;
  row.frame = trace.frame;
  row.submission = trace.submission;
  return row;
}
template <typename Buffer>
constexpr void AppendTo(Buffer& buffer, TraceRow row) noexcept {
  row.sequence = uint64_t(buffer.count) + buffer.lost + 1;
  if (buffer.count == buffer.rows.size()) {
    if (!buffer.lost)
      buffer.first_lost = row.sequence;
    ++buffer.lost;
    buffer.incomplete = 1;
    return;
  }
  buffer.rows[buffer.count++] = row;
}
static_assert([] {
  struct SmallBuffer {
    uint32_t count = 0, incomplete = 0;
    uint64_t lost = 0, first_lost = 0;
    std::array<TraceRow, 2> rows{};
  } buffer;
  AppendTo(buffer, {});
  AppendTo(buffer, {});
  AppendTo(buffer, {});
  TraceRow terminal;
  terminal.kind = TraceKind::kTerminal;
  AppendTo(buffer, terminal);
  return buffer.count == 2 && buffer.lost == 2 && buffer.first_lost == 3 && buffer.incomplete &&
         buffer.rows[0].sequence == 1 && buffer.rows[1].sequence == 2;
}());
void Append(TraceRow row) noexcept {
  AppendTo(trace.report, row);
}
void TracePhase(Phase phase) noexcept {
  std::lock_guard lock(trace.mutex);
  trace.phase = phase;
}
bool Same(const TextureValue& a, const TextureValue& b) noexcept {
  return a.instance == b.instance && a.resource == b.resource && a.key_size == b.key_size &&
         a.key_size && a.key_size <= a.key.size() &&
         !std::memcmp(a.key.data(), b.key.data(), a.key_size);
}
void Add(Kind kind, const TextureValue& value = {}, uint64_t a = 0, uint64_t b = 0, uint64_t c = 0,
         uint64_t d = 0) noexcept {
  if (state.report.count == state.report.receipts.size()) {
    Error(1);
    return;
  }
  auto& r = state.report.receipts[state.report.count++];
  r = {kind,
       state.phase,
       state.report.count,
       state.event,
       state.frame,
       state.submission,
       value,
       a,
       b,
       c,
       d};
}
bool Thread() noexcept {
  if (state.thread && state.thread != GetCurrentThreadId()) {
    Error(2);
    return false;
  }
  if (!state.thread)
    state.thread = GetCurrentThreadId();
  return true;
}
}  // namespace
bool TraceEnabled() noexcept {
  return trace.enabled.load(std::memory_order_acquire);
}
void TraceBind(const void* cp, const void* cache, const void* shared) noexcept {
  if (!TraceEnabled())
    return;
  std::lock_guard lock(trace.mutex);
  if (!TraceEnabled())
    return;
  if (!trace.cp) {
    trace.cp = Identity(cp);
    trace.cache = Identity(cache);
    trace.shared = Identity(shared);
    Append(Row(TraceKind::kOwner));
  } else if (trace.cp != Identity(cp) || trace.cache != Identity(cache) ||
             trace.shared != Identity(shared)) {
    trace.report.incomplete = 1;
  }
}
void TraceContext(const void* cp, uint64_t owned, uint64_t frame, uint64_t submission) noexcept {
  if (!TraceEnabled())
    return;
  std::lock_guard lock(trace.mutex);
  if (!TraceEnabled() || trace.cp != Identity(cp))
    return;
  trace.owned = owned;
  trace.frame = frame;
  trace.submission = submission;
  Append(Row(TraceKind::kDrawContext));
}
void TraceLoad(const void* cache, TraceKind kind, const TextureValue& texture,
               uint64_t generation_after, bool base, bool mips, uint64_t entry_generation,
               uint64_t flags) noexcept {
  if (!TraceEnabled())
    return;
  std::lock_guard lock(trace.mutex);
  if (!TraceEnabled() || trace.cache != Identity(cache))
    return;
  auto row = Row(kind);
  row.texture = texture;
  row.generation_after = generation_after;
  row.entry_generation = entry_generation;
  row.a = base;
  row.b = mips;
  row.c = texture.generation == generation_after;
  row.d = flags;
  Append(row);
}
void TraceDeclaration(const void* cache, uint32_t address, uint32_t length, bool scaled) noexcept {
  if (!TraceEnabled())
    return;
  std::lock_guard lock(trace.mutex);
  if (!TraceEnabled() || trace.cache != Identity(cache))
    return;
  auto row = Row(TraceKind::kDeclaration);
  row.a = address;
  row.b = length;
  row.c = scaled;
  Append(row);
}
void TraceInvalidation(const void* shared, uint32_t first, uint32_t last, bool gpu) noexcept {
  if (!TraceEnabled())
    return;
  std::lock_guard lock(trace.mutex);
  if (!TraceEnabled() || trace.shared != Identity(shared))
    return;
  auto row = Row(TraceKind::kInvalidation);
  row.a = first;
  row.b = last;
  row.c = gpu;
  // CPU callback ordering is only recorder serialization, not GPU-read ordering.
  row.d = !gpu;
  if (!gpu)
    row.occurrence = row.owned = row.frame = row.submission = 0;
  Append(row);
}
void TraceLifecycle(const void* owner, uint32_t cause) noexcept {
  if (!TraceEnabled())
    return;
  std::lock_guard lock(trace.mutex);
  if (!TraceEnabled() || (trace.cp != Identity(owner) && trace.cache != Identity(owner) &&
                          trace.shared != Identity(owner)))
    return;
  auto row = Row(TraceKind::kLifecycle);
  row.a = cause;
  Append(row);
  trace.report.incomplete = 1;
  if (cause >= 3)
    trace.enabled.store(false, std::memory_order_release);
}
void TraceInterference(const void* cp, uint32_t address, uint32_t length, bool unknown) noexcept {
  if (!TraceEnabled())
    return;
  std::lock_guard lock(trace.mutex);
  if (!TraceEnabled() || trace.cp != Identity(cp))
    return;
  auto row = Row(TraceKind::kInterference);
  row.a = address;
  row.b = length;
  row.c = unknown;
  Append(row);
}
void TraceCopyBegin(const void* cp, uint64_t owned, uint64_t frame, uint64_t submission) noexcept {
  if (!TraceEnabled())
    return;
  std::lock_guard lock(trace.mutex);
  if (!TraceEnabled() || trace.cp != Identity(cp))
    return;
  if (trace.occurrence)
    trace.report.incomplete = 1;
  trace.occurrence = ++trace.next_occurrence;
  trace.owned = owned;
  trace.frame = frame;
  trace.submission = submission;
  Append(Row(TraceKind::kCopyBegin));
}
void TraceCopyResolve(const void* cp, uint64_t frame, uint64_t submission) noexcept {
  if (!TraceEnabled())
    return;
  std::lock_guard lock(trace.mutex);
  if (!TraceEnabled() || trace.cp != Identity(cp))
    return;
  trace.frame = frame;
  trace.submission = submission;
  Append(Row(TraceKind::kCopyResolve));
}
void TraceCopyEnd(const void* cp, bool result, uint64_t frame, uint64_t submission,
                  uint32_t address, uint32_t length) noexcept {
  if (!TraceEnabled())
    return;
  std::lock_guard lock(trace.mutex);
  if (!TraceEnabled() || trace.cp != Identity(cp))
    return;
  auto row = Row(TraceKind::kCopyEnd);
  row.frame = frame;
  row.submission = submission;
  row.a = result;
  row.b = address;
  row.c = length;
  Append(row);
  trace.occurrence = 0;
  trace.owned = 0;
}
void TraceObserver(const void* cp, bool copy, bool prepared, uint64_t observer, uint64_t token,
                   uint64_t frame, uint64_t submission, bool accepted, bool result,
                   uint32_t address, uint32_t length) noexcept {
  if (!TraceEnabled())
    return;
  std::lock_guard lock(trace.mutex);
  if (!TraceEnabled() || trace.cp != Identity(cp))
    return;
  auto row = Row(copy ? TraceKind::kCopyObserver : TraceKind::kDrawObserver);
  row.observer = observer;
  row.token = token;
  row.frame = frame;
  row.submission = submission;
  row.a = prepared;
  row.b = accepted;
  row.c = result;
  row.d = uint64_t(address) | (uint64_t(length) << 32);
  Append(row);
}
bool Enabled() noexcept {
  return state.configured && !state.retired;
}
const Config& GetConfig() noexcept {
  return state.report.config;
}
const TextureValue* Seed() noexcept {
  return state.report.sealed ? &state.report.seed : nullptr;
}
void Error(uint32_t code) noexcept {
  if (!state.report.error)
    state.report.error = code;
}
bool Owner(const void* cp, const void* cache) noexcept {
  if (!Enabled() || !Thread())
    return false;
  if (!state.cp) {
    state.cp = cp;
    state.cache = cache;
  }
  if (state.cp != cp || state.cache != cache) {
    Error(3);
    return false;
  }
  return true;
}
void TextureAudit(Kind kind, const TextureValue& value, uint64_t a, uint64_t b) noexcept {
  if (!Enabled() || !Thread())
    return;
  if (!value.instance || !value.resource || !value.key_size || value.key_size > value.key.size())
    Error(4);
  if (state.report.sealed && !Same(value, state.report.seed))
    Error(5);
  if (kind == Kind::kEntry && state.report.sealed)
    ++state.report.post_seed_attempts;
  if (kind == Kind::kSuccess) {
    state.latest = value;
    state.success_epoch = state.epoch;
  }
  if (kind == Kind::kIdentity || kind == Kind::kSchedule)
    state.current = value;
  Add(kind, value, a, b);
}
bool Fail(const TextureValue& value, bool base, bool mips) noexcept {
  TextureAudit(Kind::kSeam, value, base, mips);
  if (!Enabled() || !state.report.config.fail || state.phase != Phase::kReloadB || !state.armed)
    return false;
  if (!Same(value, state.report.seed) || !base || mips || value.dirty ||
      value.generation != state.report.seed.generation + 1) {
    Error(6);
    return false;
  }
  ++state.report.ordinal;
  if (state.report.ordinal != 1 || state.report.injected) {
    Error(7);
    return false;
  }
  ++state.report.injected;
  state.armed = false;
  return true;
}
void View(const TextureValue& value, uint32_t fetch, bool sign, bool real, uint64_t descriptor,
          uint64_t destination) noexcept {
  if (!Enabled() || fetch || sign)
    return;
  state.view = value;
  state.real_view = real;
  ++state.view_epoch;
  Add(Kind::kView, value, state.view_epoch, real, descriptor, destination);
  TraceLoad(state.cache, TraceKind::kView, value, value.generation, real, false, descriptor,
            destination);
}
void Bindings(const void* cp) noexcept {
  if (!Enabled() || state.cp != cp || !Thread())
    return;
  state.bound_epoch = state.view_epoch;
  Add(Kind::kBindings, state.view, state.bound_epoch, state.real_view);
  TraceLoad(state.cache, TraceKind::kBinding, state.view, state.view.generation, state.real_view,
            false, state.bound_epoch);
}
void DrawContext(const void* cp, uint64_t event, uint64_t frame, uint64_t submission) noexcept {
  if (!Enabled() || (state.cp && state.cp != cp) || !Thread())
    return;
  state.event = event;
  state.frame = frame;
  state.submission = submission;
}
bool Select(const void* cp, uint64_t event, uint32_t stage, uint32_t binding, uint32_t fetch,
            bool sign, uint64_t instance, uint64_t generation) noexcept {
  if (!Enabled() || state.cp != cp || stage != 1 || binding || fetch || sign ||
      !state.report.sealed || instance != state.report.seed.instance ||
      (state.phase != Phase::kReloadB && state.phase != Phase::kRepeat &&
       state.phase != Phase::kRetryB))
    return false;
  if (event != state.event || !state.bound_epoch || !state.real_view ||
      !Same(state.view, state.report.seed))
    Error(8);
  Add(Kind::kUse, state.current, state.bound_epoch, generation, stage, binding);
  TraceLoad(state.cache, TraceKind::kUse, state.current, generation, true, false,
            state.bound_epoch);
  ++state.report.selected;
  return state.report.config.independent != 0;
}
void Use(uint64_t event, uint64_t index, uint64_t buffer, uint64_t bytes, bool reused) noexcept {
  if (!Enabled() || !state.report.sealed || state.phase == Phase::kC ||
      state.phase == Phase::kTerminal)
    return;
  if (event != state.event)
    Error(9);
  Add(Kind::kSchedule, state.current, index, buffer, bytes, reused);
}
void Rejected(uint64_t event, uint64_t map_size, uint64_t captures, uint64_t bytes) noexcept {
  if (!Enabled() || !state.report.sealed || !state.current.instance)
    return;
  if (event != state.event || !state.bound_epoch || !state.real_view ||
      !Same(state.view, state.report.seed))
    Error(26);
  Add(Kind::kRejected, state.current, map_size, captures, bytes, state.bound_epoch);
}
}  // namespace rex::graphics::d3d12::fixture

using namespace rex::graphics::d3d12::fixture;
extern "C" bool RexTextureFixtureConfigure(const Config* config) noexcept {
  if (!config || config->version != 2 || !config->run || state.configured || config->fail > 1 ||
      config->independent > 1 || config->receipt > 1)
    return false;
  state.report.config = *config;
  state.configured = true;
  trace.report.enabled = config->receipt;
  trace.enabled.store(config->receipt != 0, std::memory_order_release);
  return true;
}
extern "C" bool RexTextureFixtureAdvance(uint32_t command, uint64_t event, uint64_t frame,
                                         uint64_t submission) noexcept {
  if (!Enabled() || !Thread())
    return false;
  state.event = event;
  state.frame = frame;
  state.submission = submission;
  switch (command) {
    case 1:  // Actual A resolve completed. Earlier swap loads are outside this epoch.
      if (state.phase != Phase::kWarmup) {
        Error(10);
        break;
      }
      ++state.epoch;
      state.phase = Phase::kSeedA;
      Add(Kind::kCopy);
      break;
    case 2:  // Designated issued seed consumer.
      if (state.phase != Phase::kSeedA || state.success_epoch != state.epoch ||
          !Same(state.latest, state.current) || state.current.dirty ||
          state.latest.generation != state.current.generation || !state.real_view ||
          !state.bound_epoch) {
        Error(11);
        break;
      }
      state.report.seed = state.current;
      state.report.sealed = 1;
      Add(Kind::kSeal, state.current);
      state.phase = Phase::kProducerB;
      break;
    case 3:  // Actual B resolve completed and its texture watch fired.
      if (state.phase != Phase::kProducerB || !state.report.sealed || state.armed) {
        Error(12);
        break;
      }
      state.phase = Phase::kReloadB;
      state.armed = state.report.config.fail != 0;
      Add(Kind::kArm, state.report.seed, 1);
      break;
    case 4:
      Add(Kind::kDraw, state.current);
      if (state.phase == Phase::kReloadB)
        state.phase = Phase::kRepeat;
      else if (state.phase == Phase::kRepeat)
        state.phase = Phase::kC;
      else if (state.phase == Phase::kC)
        state.phase = Phase::kRetryB;
      else if (state.phase == Phase::kRetryB)
        state.phase = Phase::kTerminal;
      else
        Error(13);
      break;
    default:
      Error(14);
  }
  TracePhase(state.phase);
  return !state.report.error;
}
extern "C" bool RexTextureFixtureCopyReport(Report* report) noexcept {
  if (!report || !Enabled() || !Thread())
    return false;
  *report = state.report;
  {
    std::lock_guard lock(trace.mutex);
    if (trace.report.enabled && !trace.report.closed) {
      auto row = Row(TraceKind::kTerminal);
      row.a = state.phase == Phase::kTerminal && !state.armed && !trace.occurrence;
      Append(row);
      trace.report.incomplete |= !row.a;
      trace.report.closed = 1;
      trace.enabled.store(false, std::memory_order_release);
    }
    report->trace = trace.report;
  }
  return true;
}
extern "C" bool RexTextureFixtureRetire() noexcept {
  if (!Enabled() || !Thread() || state.phase != Phase::kTerminal || state.armed)
    return false;
  {
    std::lock_guard lock(trace.mutex);
    if (trace.report.enabled && !trace.report.closed) {
      auto row = Row(TraceKind::kLifecycle);
      row.a = 7;
      Append(row);
      trace.report.incomplete = 1;
      trace.report.closed = 1;
    }
    trace.enabled.store(false, std::memory_order_release);
  }
  state.retired = true;
  state.phase = Phase::kRetired;
  return true;
}
