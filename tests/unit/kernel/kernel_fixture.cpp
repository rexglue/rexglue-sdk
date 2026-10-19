#include "kernel_fixture.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <exception>
#include <filesystem>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <thread>

#include <rex/platform/thread.h>
#include <rex/runtime/function_dispatcher.h>
#include <rex/runtime/thread_state.h>
#include <rex/runtime/xmemory.h>

#include "../test_memory.h"
#include "runtime/dispatcher.h"

namespace rex::test {

namespace {

constexpr u32 kCodeBase = 0x8F000000;
constexpr u32 kCodeSize = 0x10000;
constexpr u32 kImageSize = 0x100000;

std::mutex g_bodies_mutex;
std::unordered_map<u32, std::function<void(Caller&)>>& Bodies() {
  static std::unordered_map<u32, std::function<void(Caller&)>> bodies;
  return bodies;
}
u32 g_next_body = 1;

u32 PutBody(std::function<void(Caller&)> body) {
  std::lock_guard lock(g_bodies_mutex);
  const u32 id = g_next_body++;
  Bodies().emplace(id, std::move(body));
  return id;
}

void TestStart(PPCContext& ctx, u8* base) {
  std::function<void(Caller&)> body;
  {
    std::lock_guard lock(g_bodies_mutex);
    auto it = Bodies().find(ctx.r3.u32);
    body = std::move(it->second);
    Bodies().erase(it);
  }
  Caller caller(ctx, base, runtime::GetCurrent());
  body(caller);
  ctx.r3.u64 = 0;
}

codegen::ToolRuntime BuildTool(const std::filesystem::path& root,
                               const std::function<void(const std::filesystem::path&)>& populate,
                               std::span<const std::string> modules, ui::IUserInterface* ui) {
  if (populate) {
    populate(root);
  }
  auto tool = codegen::OpenToolRuntime(root, modules, ui);
  REQUIRE(tool.has_value());
  return std::move(*tool);
}

const runtime::ExportEntry* FindExport(runtime::ExportModule module, std::string_view name) {
  const auto table = runtime::GetExports(module);
  const auto it = std::ranges::find(table, name, &runtime::ExportEntry::name);
  return it == table.end() ? nullptr : &*it;
}

std::mutex g_failure_mutex;
std::vector<std::string> g_failures;

void AddFailure(std::string failure) {
  std::lock_guard lock(g_failure_mutex);
  g_failures.push_back(std::move(failure));
}

void CheckNoFailures() {
  std::vector<std::string> failures;
  {
    std::lock_guard lock(g_failure_mutex);
    failures.swap(g_failures);
  }
  for (const std::string& failure : failures) {
    FAIL_CHECK(failure);
  }
}

}  // namespace

u32 Caller::Call(runtime::ExportModule module, std::string_view name,
                 std::initializer_list<u64> args) {
  const auto* entry = FindExport(module, name);
  if (!entry || !entry->fn) {
    AddFailure("no export named " + std::string(name));
    return 0xFFFFFFFF;
  }
  PPCRegister* registers[] = {&ctx_.r3, &ctx_.r4, &ctx_.r5, &ctx_.r6,
                              &ctx_.r7, &ctx_.r8, &ctx_.r9, &ctx_.r10};
  const u64 saved_r1 = ctx_.r1.u64;
  if (args.size() > 8) {
    ctx_.r1.u64 = Alloc(0x1000) + 0x800;
  }
  size_t index = 0;
  for (const u64 value : args) {
    if (index < 8) {
      registers[index]->u64 = value;
    } else {
      Store64(ctx_.r1.u32 + 0x50 + static_cast<u32>((index - 8) * 8), value);
    }
    ++index;
  }
  entry->fn(ctx_, base_);
  ctx_.r1.u64 = saved_r1;
  return ctx_.r3.u32;
}

u32 Caller::Kernel(std::string_view name, std::initializer_list<u64> args) {
  return Call(runtime::ExportModule::kXboxkrnl, name, args);
}

u32 Caller::XAM(std::string_view name, std::initializer_list<u64> args) {
  return Call(runtime::ExportModule::kXAM, name, args);
}

u32 Caller::Alloc(u32 size) {
  auto* memory = runtime_.GetMemory();
  const u32 address = memory->SystemHeapAlloc(size);
  std::memset(memory->TranslateVirtual(address), 0, size);
  return address;
}

u32 Caller::Load32(u32 address) const {
  return *runtime_.GetMemory()->TranslateVirtual<const be_u32*>(address);
}

u32 Caller::LoadAcquire32(u32 address) const {
  return runtime_.GetMemory()->TranslateVirtual<const be_u32*>(address)->Load(
      std::memory_order_acquire);
}

u8 Caller::Load8(u32 address) const {
  return *runtime_.GetMemory()->TranslateVirtual<u8*>(address);
}

void Caller::Store32(u32 address, u32 value) {
  *runtime_.GetMemory()->TranslateVirtual<be_u32*>(address) = value;
}

u32 Caller::String(std::string_view text) {
  const u32 address = Alloc(static_cast<u32>(text.size()) + 1);
  std::memcpy(runtime_.GetMemory()->TranslateVirtual(address), text.data(), text.size());
  return address;
}

void Caller::Store64(u32 address, u64 value) {
  *runtime_.GetMemory()->TranslateVirtual<be_u64*>(address) = value;
}

u32 Caller::Quad(i64 value) {
  const u32 address = Alloc(8);
  Store64(address, static_cast<u64>(value));
  return address;
}

u32 Caller::Attributes(std::string_view name) {
  const u32 text = String(name);
  const u32 ansi = Alloc(8);
  Store32(ansi, (static_cast<u32>(name.size()) << 16) | static_cast<u32>(name.size() + 1));
  Store32(ansi + 4, text);
  const u32 attributes = Alloc(16);
  Store32(attributes + 4, ansi);
  return attributes;
}

u32 Caller::RegisterBody(std::function<void(Caller&)> body) {
  return PutBody(std::move(body));
}

u32 Caller::Spawn(std::function<void(Caller&)> body, u32 creation_flags) {
  const u32 handle_ptr = Alloc(4);
  const u32 id = PutBody(std::move(body));
  const u32 status =
      Kernel("ExCreateThread", {handle_ptr, 0x10000, 0, 0, start_thunk, id, creation_flags});
  if (status != 0) {
    AddFailure("ExCreateThread returned " + std::to_string(status));
    return 0;
  }
  return Load32(handle_ptr);
}

KernelFixture::ThreadDrain::ThreadDrain() : baseline(platform::detail::GetThreadCount()) {}

KernelFixture::ThreadDrain::~ThreadDrain() {
  platform::detail::ShutdownTimerQueue();
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (platform::detail::GetThreadCount() > baseline &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
}

KernelFixture::KernelFixture(const std::function<void(const std::filesystem::path&)>& populate,
                             std::span<const std::string> modules, ui::IUserInterface* ui)
    : tool_((ReleaseTestMemory(), BuildTool(root_.path, populate, modules, ui))) {
  auto* dispatcher = GetRuntime().GetFunctionDispatcher();
  REQUIRE(dispatcher->InitializeFunctionTable(kCodeBase, kCodeSize, kCodeBase, kImageSize,
                                              /*is_entrypoint=*/true));
  Caller::start_thunk = dispatcher->AllocateThunk(&TestStart, 0);
  REQUIRE(Caller::start_thunk != 0);
  static_cast<runtime::Dispatcher&>(GetRuntime().GetDispatcher()).Start();
}

KernelFixture::~KernelFixture() {
  try {
    CheckNoFailures();
  } catch (const std::exception& error) {
    std::fprintf(stderr, "KernelFixture teardown failed with %s\n", error.what());
  }
}

void KernelFixture::RunOnGuestThread(const std::function<void(Caller&)>& body) {
  auto done = std::make_shared<std::promise<void>>();
  auto finished = done->get_future();

  runtime::ThreadState host_state(0, 0, 0, GetRuntime().GetMemory(), &GetRuntime());
  runtime::ThreadState* previous = runtime::ThreadState::Get();
  runtime::ThreadState::Bind(&host_state);

  u8* base = GetRuntime().GetMemory()->GetVirtualMembase();
  Caller main_caller(*host_state.GetContext(), base, GetRuntime());
  const u32 handle = main_caller.Spawn([body, done](Caller& caller) {
    body(caller);
    done->set_value();
  });

  const bool finished_in_time =
      finished.wait_for(std::chrono::seconds(20)) == std::future_status::ready;
  CHECK(finished_in_time);
  CheckNoFailures();
  if (finished_in_time) {
    main_caller.Kernel("NtClose", {handle});
  }
  runtime::ThreadState::Bind(previous);
}

void KernelFixture::RunUntilGuestThreadEnds(const std::function<void(Caller&)>& body) {
  runtime::ThreadState host_state(0, 0, 0, GetRuntime().GetMemory(), &GetRuntime());
  runtime::ThreadState* previous = runtime::ThreadState::Get();
  runtime::ThreadState::Bind(&host_state);

  u8* base = GetRuntime().GetMemory()->GetVirtualMembase();
  Caller main_caller(*host_state.GetContext(), base, GetRuntime());
  const u32 handle = main_caller.Spawn(body);

  constexpr i64 kTwentySeconds = 200000000;
  const bool ended = main_caller.Kernel("NtWaitForSingleObjectEx",
                                        {handle, 1, 0, main_caller.Quad(-kTwentySeconds)}) == 0;
  CHECK(ended);
  CheckNoFailures();
  if (ended) {
    main_caller.Kernel("NtClose", {handle});
  }
  runtime::ThreadState::Bind(previous);
}

u32 CopyToGuest(Caller& c, std::span<const u8> bytes) {
  const u32 address = c.Alloc(static_cast<u32>(bytes.size()) + 8);
  for (u32 offset = 0; offset < bytes.size(); offset += 4) {
    u8 word[4]{};
    std::memcpy(word, bytes.data() + offset, std::min<size_t>(4, bytes.size() - offset));
    c.Store32(address + offset,
              (u32{word[0]} << 24) | (u32{word[1]} << 16) | (u32{word[2]} << 8) | word[3]);
  }
  return address;
}

u32 PutFilled(Caller& c, u32 size) {
  std::vector<u8> bytes(size);
  for (u32 i = 0; i < size; ++i) {
    bytes[i] = std::array<u8, 4>{0xDE, 0xAD, 0xBE, 0xEF}[i % 4];
  }
  return CopyToGuest(c, bytes);
}

std::vector<u8> ReadBytes(Caller& c, u32 address, u32 size) {
  std::vector<u8> bytes(size);
  for (u32 i = 0; i < size; ++i) {
    bytes[i] = c.Load8(address + i);
  }
  return bytes;
}

std::vector<u8> Hex(std::string_view text) {
  std::vector<u8> bytes(text.size() / 2);
  for (size_t i = 0; i < bytes.size(); ++i) {
    bytes[i] = static_cast<u8>(std::stoi(std::string(text.substr(i * 2, 2)), nullptr, 16));
  }
  return bytes;
}

std::string ToHex(std::span<const u8> bytes) {
  static constexpr char kDigits[] = "0123456789abcdef";
  std::string text;
  for (const u8 byte : bytes) {
    text.push_back(kDigits[byte >> 4]);
    text.push_back(kDigits[byte & 15]);
  }
  return text;
}

void Join(Caller& c, u32 handle) {
  CHECK(c.Kernel("NtWaitForSingleObjectEx", {handle, 1, 0, c.Quad(-50000000)}) == 0);
  c.Kernel("NtClose", {handle});
}

u64 SystemTime(Caller& c) {
  const u32 time = c.Alloc(8);
  c.Kernel("KeQuerySystemTime", {time});
  return (static_cast<u64>(c.Load32(time)) << 32) | c.Load32(time + 4);
}

}  // namespace rex::test
