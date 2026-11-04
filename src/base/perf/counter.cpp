#include <rex/platform/perf.h>

#include <rex/logging/log.h>
#include <rex/string/text.h>
#include <rex/settings.h>

#include "base/core_settings.h"

#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <format>
#include <fstream>
#include <mutex>

namespace rex {

PerfSettings& GetPerfSettings() {
  static PerfSettings& settings = [] -> PerfSettings& {
    static PerfSettings owner;
    (void)REX_SETTINGS().Register(owner);
    return owner;
  }();
  return settings;
}

}  // namespace rex

namespace rex::platform {

namespace {

std::array<std::atomic<i64>, kMaxCounters> g_counters{};
std::array<std::atomic<i64>, kMaxCounters> g_snapshot{};

constexpr size_t kNameCapacity = 48;
using CounterName = std::array<char, kNameCapacity>;

constexpr CounterName MakeName(std::string_view text) {
  CounterName name{};
  for (size_t i = 0; i < text.size() && i + 1 < name.size(); ++i) {
    name[i] = text[i];
  }
  return name;
}

constinit std::array<CounterName, kMaxCounters> g_names = {MakeName("frame_time_us"),
                                                           MakeName("fps")};
constinit std::array<CounterType, kMaxCounters> g_types{};
constinit std::atomic<size_t> g_count{2};
constinit std::mutex g_register_mutex;

size_t CounterCount() {
  return g_count.load(std::memory_order_acquire);
}

bool IsGauge(size_t index) {
  return g_types[index] == CounterType::kGauge;
}

std::ofstream& CSVFile() {
  static std::ofstream file;
  return file;
}

int g_csv_frame_count = 0;

}  // namespace

Result<CounterID> RegisterCounter(const char* name, CounterType type) {
  std::lock_guard lock(g_register_mutex);
  const size_t count = CounterCount();
  for (size_t i = 0; i < count; ++i) {
    if (std::strcmp(g_names[i].data(), name) == 0) {
      return static_cast<CounterID>(i);
    }
  }
  if (count >= kMaxCounters) {
    return Err<CounterID>(ErrorCategory::kRuntime,
                          std::format("no free counter slot for {}", name));
  }
  if (std::strlen(name) >= kNameCapacity) {
    return Err<CounterID>(
        ErrorCategory::kRuntime,
        std::format("the counter name {} is longer than {} characters", name, kNameCapacity - 1));
  }
  g_names[count] = MakeName(name);
  g_types[count] = type;
  g_count.store(count + 1, std::memory_order_release);
  return static_cast<CounterID>(count);
}

std::optional<CounterID> FindCounter(std::string_view name) {
  const size_t count = CounterCount();
  for (size_t i = 0; i < count; ++i) {
    if (name == g_names[i].data()) {
      return static_cast<CounterID>(i);
    }
  }
  return std::nullopt;
}

const char* GetCounterName(CounterID id) {
  auto idx = static_cast<size_t>(id);
  if (idx < CounterCount())
    return g_names[idx].data();
  return "unknown";
}

void SetCounter(CounterID id, i64 value) {
  const size_t idx = static_cast<size_t>(id);
  if (idx < kMaxCounters)
    g_counters[idx].store(value, std::memory_order_relaxed);
}

void IncrementCounter(CounterID id, i64 delta) {
  const size_t idx = static_cast<size_t>(id);
  if (idx < kMaxCounters)
    g_counters[idx].fetch_add(delta, std::memory_order_relaxed);
}

i64 GetCounter(CounterID id) {
  const size_t idx = static_cast<size_t>(id);
  return idx < kMaxCounters ? g_counters[idx].load(std::memory_order_relaxed) : 0;
}

void ResetFrameCounters() {
  const size_t count = CounterCount();
  for (size_t i = 0; i < count; ++i) {
    if (IsGauge(i)) {
      g_snapshot[i].store(g_counters[i].load(std::memory_order_relaxed), std::memory_order_relaxed);
    } else {
      g_snapshot[i].store(g_counters[i].exchange(0, std::memory_order_relaxed),
                          std::memory_order_relaxed);
    }
  }
}

i64 GetSnapshotCounter(CounterID id) {
  const size_t idx = static_cast<size_t>(id);
  return idx < kMaxCounters ? g_snapshot[idx].load(std::memory_order_relaxed) : 0;
}

void InitCounters() {
  for (auto& c : g_counters)
    c.store(0, std::memory_order_relaxed);
  for (auto& s : g_snapshot)
    s.store(0, std::memory_order_relaxed);
}

void SetCSVLogPath(const std::string& path) {
  auto& csv = CSVFile();
  if (csv.is_open()) {
    csv.close();
  }
  g_csv_frame_count = 0;

  if (path.empty())
    return;

  csv.open(rex::string::to_path(path), std::ios::out | std::ios::trunc);
  if (!csv) {
    REXLOG_WARN("perf: failed to open CSV log at {}", path);
    return;
  }

  const size_t count = CounterCount();
  for (size_t i = 0; i < count; ++i) {
    if (i > 0)
      csv << ',';
    csv << g_names[i].data();
  }
  csv << '\n';
}

void WriteCSVFrame() {
  auto& csv = CSVFile();
  if (!csv.is_open())
    return;

  const size_t count = CounterCount();
  for (size_t i = 0; i < count; ++i) {
    if (i > 0)
      csv << ',';
    csv << std::format("{}", g_snapshot[i].load(std::memory_order_relaxed));
  }
  csv << '\n';

  if (++g_csv_frame_count % 60 == 0) {
    csv.flush();
  }
}

void CloseCSV() {
  if (CSVFile().is_open()) {
    CSVFile().close();
  }
}

}  // namespace rex::platform
