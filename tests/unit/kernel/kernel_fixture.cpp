#include "kernel_fixture.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <future>
#include <memory>
#include <mutex>
#include <unordered_map>

#include <thread>

#include <rex/platform/thread.h>
#include <rex/runtime/function_dispatcher.h>
#include <rex/runtime/thread_state.h>
#include <rex/runtime/xmemory.h>

#include "../test_memory.h"
#include "runtime/dispatcher.h"

namespace rex::testing {

namespace {

constexpr u32 kCodeBase = 0x8F000000;
constexpr u32 kCodeSize = 0x10000;
constexpr u32 kImageSize = 0x100000;

std::mutex g_bodies_mutex;
std::unordered_map<u32, std::function<void(Caller&)>> g_bodies;
u32 g_next_body = 1;
std::atomic<u32> g_next_root{0};

u32 PutBody(std::function<void(Caller&)> body) {
  std::lock_guard lock(g_bodies_mutex);
  const u32 id = g_next_body++;
  g_bodies.emplace(id, std::move(body));
  return id;
}

void TestStart(PPCContext& ctx, u8* base) {
  std::function<void(Caller&)> body;
  {
    std::lock_guard lock(g_bodies_mutex);
    auto it = g_bodies.find(ctx.r3.u32);
    body = std::move(it->second);
    g_bodies.erase(it);
  }
  Caller caller(ctx, base, runtime::Current());
  body(caller);
  ctx.r3.u64 = 0;
}

codegen::ToolRuntime MakeTool(const std::filesystem::path& root,
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
  const auto table = runtime::Exports(module);
  const auto it = std::ranges::find(table, name, &runtime::ExportEntry::name);
  return it == table.end() ? nullptr : &*it;
}

}  // namespace

u32 Caller::Call(runtime::ExportModule module, std::string_view name,
                 std::initializer_list<u64> args) {
  const auto* entry = FindExport(module, name);
  REQUIRE(entry != nullptr);
  REQUIRE(entry->fn != nullptr);
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
      Store32(ctx_.r1.u32 + 0x54 + static_cast<u32>((index - 8) * 8), static_cast<u32>(value));
    }
    ++index;
  }
  entry->fn(ctx_, base_);
  ctx_.r1.u64 = saved_r1;
  return ctx_.r3.u32;
}

u32 Caller::Kernel(std::string_view name, std::initializer_list<u64> args) {
  return Call(runtime::ExportModule::Xboxkrnl, name, args);
}

u32 Caller::XAM(std::string_view name, std::initializer_list<u64> args) {
  return Call(runtime::ExportModule::XAM, name, args);
}

u32 Caller::Alloc(u32 size) {
  auto* memory = runtime_.memory();
  const u32 address = memory->SystemHeapAlloc(size);
  std::memset(memory->TranslateVirtual(address), 0, size);
  return address;
}

u32 Caller::Load32(u32 address) const {
  return memory::load_and_swap<u32>(runtime_.memory()->TranslateVirtual(address));
}

u8 Caller::Load8(u32 address) const {
  return *runtime_.memory()->TranslateVirtual<u8*>(address);
}

void Caller::Store32(u32 address, u32 value) {
  memory::store_and_swap<u32>(runtime_.memory()->TranslateVirtual(address), value);
}

u32 Caller::String(std::string_view text) {
  const u32 address = Alloc(static_cast<u32>(text.size()) + 1);
  std::memcpy(runtime_.memory()->TranslateVirtual(address), text.data(), text.size());
  return address;
}

void Caller::Store64(u32 address, u64 value) {
  memory::store_and_swap<u64>(runtime_.memory()->TranslateVirtual(address), value);
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

u32 Caller::PutBodyForTest(std::function<void(Caller&)> body) {
  return PutBody(std::move(body));
}

u32 Caller::Spawn(std::function<void(Caller&)> body, u32 creation_flags) {
  const u32 handle_ptr = Alloc(4);
  const u32 id = PutBody(std::move(body));
  const u32 status =
      Kernel("ExCreateThread", {handle_ptr, 0x10000, 0, 0, start_thunk, id, creation_flags});
  REQUIRE(status == 0);
  return Load32(handle_ptr);
}

KernelFixture::ThreadDrain::ThreadDrain() : baseline(platform::detail::LiveCount()) {}

KernelFixture::ThreadDrain::~ThreadDrain() {
  platform::detail::ShutdownTimerQueue();
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (platform::detail::LiveCount() > baseline && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
}

KernelFixture::TempRoot::TempRoot() {
  path = std::filesystem::temp_directory_path() /
         ("rexglue_kernel_fixture_" + std::to_string(g_next_root.fetch_add(1)) + "_" +
          std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  std::filesystem::create_directories(path);
}

KernelFixture::TempRoot::~TempRoot() {
  std::error_code error;
  std::filesystem::remove_all(path, error);
}

KernelFixture::KernelFixture(const std::function<void(const std::filesystem::path&)>& populate,
                             std::span<const std::string> modules, ui::IUserInterface* ui)
    : tool_((ReleaseTestMemory(), MakeTool(root_.path, populate, modules, ui))) {
  auto* dispatcher = runtime().function_dispatcher();
  REQUIRE(dispatcher->InitializeFunctionTable(kCodeBase, kCodeSize, kCodeBase, kImageSize,
                                              /*is_entrypoint=*/true));
  Caller::start_thunk = dispatcher->AllocateThunk(&TestStart, 0);
  REQUIRE(Caller::start_thunk != 0);
  static_cast<runtime::Dispatcher&>(runtime().dispatcher()).Start();
}

KernelFixture::~KernelFixture() = default;

void KernelFixture::RunOnGuestThread(const std::function<void(Caller&)>& body) {
  auto done = std::make_shared<std::promise<void>>();
  auto finished = done->get_future();

  runtime::ThreadState host_state(0, 0, 0, runtime().memory(), &runtime());
  runtime::ThreadState* previous = runtime::ThreadState::Get();
  runtime::ThreadState::Bind(&host_state);

  PPCContext ctx{};
  u8* base = runtime().memory()->virtual_membase();
  Caller main_caller(ctx, base, runtime());
  const u32 handle = main_caller.Spawn([body, done](Caller& caller) {
    body(caller);
    done->set_value();
  });

  REQUIRE(finished.wait_for(std::chrono::seconds(20)) == std::future_status::ready);
  main_caller.Kernel("NtClose", {handle});
  runtime::ThreadState::Bind(previous);
}

}  // namespace rex::testing
