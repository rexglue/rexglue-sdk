/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <functional>
#include <future>
#include <type_traits>
#include <variant>
#include <vector>

#include <format>

#include <rex/api.h>
#include <rex/logging.h>
#include <rex/math.h>
#include <rex/ppc/function.h>
#include <rex/runtime.h>
#include <rex/runtime/function_dispatcher.h>
#include <rex/runtime/kernel_module.h>
#include <rex/runtime/thread_state.h>
#include <rex/runtime/user_module.h>
#include <rex/runtime/workspace_heap.h>
#include <rex/runtime/kernel_list.h>
#include <rex/runtime/xam/app_manager.h>
#include <rex/runtime/xam/user_profile.h>
#include <rex/runtime/event_object.h>
#include <rex/runtime/xexception.h>
#include <rex/runtime/module_object.h>
#include <rex/runtime/mutant_object.h>
#include <rex/runtime/semaphore_object.h>
#include <rex/runtime/xthread.h>
#include <rex/runtime/timer_object.h>
#include <rex/string.h>

#include "runtime/achievement_manager.h"
#include "runtime/apu.h"
#include "runtime/dispatcher.h"
#include "runtime/module_registry.h"
#include "runtime/notifier.h"
#include "runtime/object_table.h"
#include "runtime/thread_registry.h"
#include "runtime/thread_wait.h"
#include "runtime/guest_stack.h"
#include "runtime/xam/content_manager.h"

namespace rex {

using namespace rex::runtime;

namespace {

void ShutDownSockets(IObjectTable& table) {
  for (auto& sock : table.All<SocketObject>()) {
    SocketShutdown(*sock, 2);
  }
}

}  // namespace

void Runtime::CreateSubsystems() {
  app_manager_ = std::make_unique<runtime::AppManager>();
  user_profile_ = std::make_unique<runtime::UserProfile>(*this);
  object_table_ = std::make_unique<runtime::ObjectTable>(memory_.get());
  CreateWorkspace(WorkspaceHeap::ResolveSize(0));
  thread_registry_ = std::make_unique<runtime::ThreadRegistry>(*this, *object_table_);
  module_registry_ = std::make_unique<runtime::ModuleRegistry>(*this, *object_table_);
  notifier_ = std::make_unique<runtime::Notifier>(*object_table_);
  std::filesystem::path user_data_root = this->user_data_root();
  if (!user_data_root.empty()) {
    user_data_root = std::filesystem::absolute(user_data_root);
  }
  content_manager_ = std::make_unique<runtime::ContentManager>(*this, user_data_root, *app_manager_,
                                                               *user_profile_);
  achievement_manager_ = std::make_unique<runtime::AchievementManager>();
  dispatcher_ = std::make_unique<runtime::Dispatcher>(*this);

  kernel_guest_globals_ = memory_->SystemHeapAlloc(sizeof(KernelGuestGlobals));
  auto globals = memory_->TranslateVirtual<KernelGuestGlobals*>(kernel_guest_globals_);
  std::memset(globals, 0, sizeof(KernelGuestGlobals));

  globals->ExThreadObjectType.pool_tag = memory::make_fourcc('T', 'h', 'r', 'd');
  globals->ExEventObjectType.pool_tag = memory::make_fourcc('E', 'v', 'n', 't');
  globals->ExMutantObjectType.pool_tag = memory::make_fourcc('M', 'u', 't', 'a');
  globals->ExSemaphoreObjectType.pool_tag = memory::make_fourcc('S', 'e', 'm', 'a');
  globals->ExTimerObjectType.pool_tag = memory::make_fourcc('T', 'i', 'm', 'r');
  globals->IoCompletionObjectType.pool_tag = memory::make_fourcc('I', 'o', 'C', 'p');
  globals->IoDeviceObjectType.pool_tag = memory::make_fourcc('I', 'o', 'D', 'v');
  globals->IoFileObjectType.pool_tag = memory::make_fourcc('I', 'o', 'F', 'l');
  globals->ObDirectoryObjectType.pool_tag = memory::make_fourcc('O', 'b', 'D', 'r');
  globals->ObSymbolicLinkObjectType.pool_tag = memory::make_fourcc('O', 'b', 'S', 'l');

  auto* usbd_event = reinterpret_cast<X_DISPATCH_HEADER*>(&globals->UsbdBootEnumerationDoneEvent);
  // Dispatch header type 1 is a notification event.
  usbd_event->type = 1;
  usbd_event->signal_state = 1;

  u32 oddobject_offset = kernel_guest_globals_ + offsetof(KernelGuestGlobals, OddObj);
  globals->OddObj.field0 = 0x1000000;
  globals->OddObj.field4 = 1;
  globals->OddObj.points_to_self =
      oddobject_offset + offsetof(X_UNKNOWN_TYPE_REFED, points_to_self);
  globals->OddObj.points_to_prior = globals->OddObj.points_to_self;

  InitializeProcess(&globals->idle_process, X_PROCTYPE_IDLE, 0, 0, 0);
  globals->idle_process.quantum = 0x7F;

  InitializeProcess(&globals->system_process, X_PROCTYPE_SYSTEM, 2, 5, 9);
  SetProcessTLSVars(&globals->system_process, 32, 0, 0);

  InitializeProcess(&globals->title_process, X_PROCTYPE_USER, 0, 0, 0);
  SetProcessTLSVars(&globals->title_process, 0, 0, 0);
}

void Runtime::DestroySubsystems() {
  terminating_title_.store(true, std::memory_order_release);
  app_manager_.reset();
  if (dispatcher_) {
    dispatcher_->Stop(config_.runtime.terminate_timeout);
  }
  if (thread_registry_) {
    std::vector<ThreadPin> threads = thread_registry_->All();
    for (ThreadPin& thread : threads) {
      thread->RequestStop();
      while (thread->suspend_count() > 0 && thread->Resume() == X_STATUS_SUCCESS) {}
    }
    if (object_table_) {
      ShutDownSockets(*object_table_);
    }
    const auto interval = config_.runtime.terminate_timeout;
    WaitForThreadsToStop(threads, interval, [interval](const ThreadPin& thread) {
      REXLOG_WARN(system, "Thread {:08X} '{}' still running after {} ms", thread->thread_id(),
                  thread->name(), interval.count());
    });
  }
  if (module_registry_) {
    module_registry_->UnloadAll();
  }
  if (notifier_) {
    notifier_->Shutdown();
  }
  if (object_table_) {
    object_table_->Reset();
  }
  if (thread_registry_) {
    thread_registry_->DestroyFibers();
  }
  if (kernel_guest_globals_) {
    memory_->SystemHeapFree(kernel_guest_globals_);
    kernel_guest_globals_ = 0;
  }
  if (module_registry_) {
    module_registry_->DrainLibraries();
  }
  dispatcher_.reset();
  achievement_manager_.reset();
  module_registry_.reset();
  notifier_.reset();
  object_table_.reset();
  workspace_.reset();
  thread_registry_.reset();
  user_profile_.reset();
  content_manager_.reset();
  {
    std::lock_guard lock(terminate_mutex_);
    terminate_registrations_.clear();
  }
  terminating_title_.store(false, std::memory_order_release);
  terminate_claimed_.store(false, std::memory_order_release);
}

void Runtime::CreateWorkspace(u32 size) {
  Result<std::unique_ptr<WorkspaceHeap>> workspace = WorkspaceHeap::Create(*memory_, size);
  if (!workspace) {
    REXLOG_ERROR(system, "Cannot commit a {:#x} byte title workspace, keeping {:#x} bytes", size,
                 workspace_ ? workspace_->size() : 0u);
    return;
  }
  workspace_ = std::move(*workspace);
}

void Runtime::InitializeProcess(X_KPROCESS* process, u32 process_type, u8 unk_18, u8 unk_19,
                                u8 unk_1A) {
  process->unk_18 = unk_18;
  process->unk_19 = unk_19;
  process->unk_1A = unk_1A;
  process->unk_1B = 0x06;
  process->quantum = 60;
  process->clrdataa_masked_ptr = 0;
  process->thread_count = 0;
  process->kernel_stack_size = kDefaultGuestStackSize;
  process->tls_slot_size = 0x80;
  process->process_type = static_cast<u8>(process_type);
  runtime::InitializeListHead(&process->thread_list, memory_.get());
  runtime::InitializeListHead(&process->unk_54, memory_.get());
}

void Runtime::SetProcessTLSVars(X_KPROCESS* process, u32 num_slots, u32 tls_data_size,
                                u32 tls_raw_data_address) {
  const u32 slot_count = ResolveTLSSlotCount(num_slots);
  process->tls_slot_size = static_cast<u16>(4 * slot_count);
  process->tls_data_size = tls_data_size;
  process->tls_static_data_address = tls_raw_data_address;
  for (u32 i = 0; i < 8; ++i) {
    const u32 bits = std::min(slot_count - std::min(slot_count, i * 32), 32u);
    process->tls_slot_bitmap[i] = bits ? ~0u << (32 - bits) : 0u;
  }
}

void Runtime::LoadAchievementsData() {
  std::vector<AchievementInfo> achievements;

  const runtime::XDBFGameData db = content_manager_->title_xdbf();
  if (db.is_valid()) {
    const XLanguage language =
        db.GetExistingLanguage(ToXLanguage(config_.runtime.kernel.user_language));
    for (const auto& entry : db.GetAchievements()) {
      AchievementInfo info;
      info.id = entry.id;
      info.label = db.GetStringTableEntry(language, entry.label_id);
      info.description = db.GetStringTableEntry(language, entry.description_id);
      info.unachieved_description = db.GetStringTableEntry(language, entry.unachieved_id);
      info.image_id = entry.image_id;
      info.gamerscore = entry.gamerscore;
      info.flags = entry.flags;
      achievements.push_back(std::move(info));
    }
  }

  achievement_manager_->ReplaceAchievements(std::move(achievements));
  if (auto metadata_path = FindMetadataPath("achievements.toml")) {
    if (auto loaded = achievement_manager_->LoadMetadataFile(*metadata_path); !loaded) {
      REXLOG_WARN(system, "achievements metadata {} ignored, {}", rex::path_to_utf8(*metadata_path),
                  loaded.error().message);
    }
  }

  const auto user_root = user_data_root();
  if (!user_root.empty()) {
    achievement_manager_->SetUnlockSavePath(
        user_root / "achievements" / std::format("{:08X}.toml", content_manager_->title_id()));
    achievement_manager_->LoadUnlockState();
  }
}

u32 Runtime::process_type() const {
  auto globals = memory_->TranslateVirtual<const KernelGuestGlobals*>(kernel_guest_globals_);
  return globals->title_process.process_type;
}

void Runtime::set_process_type(u32 value) {
  auto globals = memory_->TranslateVirtual<KernelGuestGlobals*>(kernel_guest_globals_);
  globals->title_process.process_type = static_cast<u8>(value);
}

void Runtime::RegisterTitleTerminateNotification(u32 registration) {
  std::lock_guard lock(terminate_mutex_);
  if (std::ranges::find(terminate_registrations_, registration) == terminate_registrations_.end()) {
    terminate_registrations_.push_back(registration);
  }
}

void Runtime::RemoveTitleTerminateNotification(u32 registration) {
  std::lock_guard lock(terminate_mutex_);
  std::erase(terminate_registrations_, registration);
}

void Runtime::RunTerminateNotifications(std::chrono::milliseconds timeout) {
  std::vector<u32> registrations;
  {
    std::lock_guard lock(terminate_mutex_);
    registrations = std::exchange(terminate_registrations_, {});
  }
  if (registrations.empty() || !dispatcher_ || !dispatcher_->IsRunning()) {
    return;
  }
  auto* memory = memory_.get();
  std::ranges::stable_sort(registrations, std::greater<>{}, [memory](u32 registration) {
    return static_cast<i32>(static_cast<u32>(
        memory->TranslateVirtual<X_EX_TITLE_TERMINATE_REGISTRATION*>(registration)->priority));
  });
  struct Progress {
    std::atomic<u32> routine{0};
    std::promise<void> done;
  };
  auto progress = std::make_shared<Progress>();
  std::future<void> done = progress->done.get_future();
  auto run = [memory, functions = function_dispatcher_.get(), registrations, progress] {
    runtime::ThreadState* state = runtime::ThreadState::Get();
    for (const u32 registration : registrations) {
      const u32 routine = memory->TranslateVirtual<X_EX_TITLE_TERMINATE_REGISTRATION*>(registration)
                              ->notification_routine;
      progress->routine = routine;
      REXLOG_INFO(system, "Title terminate notification {:08X} with registration {:08X}", routine,
                  registration);
      u64 args[] = {registration};
      functions->ExecuteTrap(state, routine, args, rex::countof(args));
    }
    progress->routine = 0;
    progress->done.set_value();
  };
  if (dispatcher_->IsDispatchThread()) {
    run();
    return;
  }
  dispatcher_->Post(std::move(run));
  if (done.wait_for(timeout) == std::future_status::timeout) {
    REXLOG_WARN(system, "Title terminate notification {:08X} still running after {} ms",
                progress->routine.load(), timeout.count());
    dispatcher_->RequestStop();
  }
}

void Runtime::SetExecutableModule(X_HANDLE module_handle) {
  auto module = object_table_->Get<ModuleObject>(module_handle);
  if (auto current = object_table_->Get<ModuleObject>(module_registry_->executable().value_or(0));
      current && module && &*current == &*module) {
    return;
  }
  module_registry_->set_executable(module_handle);
  if (!module) {
    return;
  }

  auto globals = memory_->TranslateVirtual<KernelGuestGlobals*>(kernel_guest_globals_);
  auto* pib = &globals->title_process;
  pib->unk_18 = 10;
  pib->unk_19 = 13;
  pib->unk_1A = 17;

  u32 default_stack_size = 0;
  UserModuleGetOptHeader(*module, filesystem::XEX_HEADER_DEFAULT_STACK_SIZE, &default_stack_size);
  if (default_stack_size) {
    pib->kernel_stack_size = ResolveGuestStackSize(default_stack_size, 0, kMinimumThreadStackSize);
  }

  filesystem::XEXOptTLSInfo* tls_header = nullptr;
  UserModuleGetOptHeader(*module, filesystem::XEX_HEADER_TLS_INFO, &tls_header);
  if (tls_header) {
    pib->tls_raw_data_size = tls_header->raw_data_size;
    SetProcessTLSVars(pib, tls_header->slot_count, tls_header->data_size,
                      tls_header->raw_data_address);
  }
  u32 workspace_size_address = 0;
  UserModuleGetOptHeader(*module, filesystem::XEX_HEADER_TITLE_WORKSPACE_SIZE,
                         &workspace_size_address);
  const u32 workspace_size = WorkspaceHeap::ResolveSize(
      workspace_size_address
          ? u32{*memory_->TranslateVirtual<const be_u32*>(workspace_size_address)}
          : 0);
  if (!workspace_ || workspace_->size() != workspace_size) {
    CreateWorkspace(workspace_size);
  }

  REX_ASSERT(module_registry_->IsKernel("xboxkrnl.exe"));
  auto xboxkrnl =
      object_table_->Get<ModuleObject>(module_registry_->FindKernel("xboxkrnl.exe").value_or(0));
  if (xboxkrnl) {
    if (const u32 address =
            KernelModuleVariable(*xboxkrnl, 0x0193 /* executable module handle */)) {
      auto variable_ptr = memory_->TranslateVirtual<be_u32*>(address);
      *variable_ptr = module->hmodule_ptr;
    }
    if (const u32 address = KernelModuleVariable(*xboxkrnl, 0x01AF /* ExLoadedImageName */)) {
      char* variable_ptr = memory_->TranslateVirtual<char*>(address);
      rex::copy_truncating(variable_ptr, module->path, kExLoadedImageNameSize);
    }
  }

  dispatcher_->Start();

  LoadAchievementsData();
}

void Runtime::TerminateTitle() {
  REXLOG_DEBUG(system, "Runtime::TerminateTitle");
  if (KernelThread::IsInThread() && terminate_claimed_.exchange(true, std::memory_order_acq_rel)) {
    KernelThread::GetCurrentThread()->Exit(0);
  }

  const auto timeout = config_.runtime.terminate_timeout;
  RunTerminateNotifications(timeout);
  StopGuestThreads(timeout);

  if (KernelThread::IsInThread()) {
    KernelThread::GetCurrentThread()->Exit(0);
  }
}

void Runtime::StopGuestThreads(std::chrono::milliseconds timeout) {
  terminating_title_.store(true, std::memory_order_release);
  if (!thread_registry_) {
    return;
  }

  std::vector<ThreadPin> target_threads;
  for (ThreadPin& thread : thread_registry_->All()) {
    if (!KernelThread::IsInThread(thread.get()) && thread->is_guest_thread()) {
      target_threads.push_back(std::move(thread));
    }
  }

  for (auto& thread : target_threads) {
    thread->RequestStop();
    while (thread->suspend_count() > 0 && thread->Resume() == X_STATUS_SUCCESS) {}
  }
  if (object_table_) {
    ShutDownSockets(*object_table_);
  }

  WaitForThreadsToStop(target_threads, timeout, [timeout](const ThreadPin& thread) {
    REXLOG_WARN(system, "Guest thread {:08X} '{}' entry {:08X} still running after {} ms",
                thread->thread_id(), thread->name(), thread->creation_params()->start_address,
                timeout.count());
  });
}

void Runtime::StopThreads() {
  const auto timeout = config_.runtime.terminate_timeout;
  StopGuestThreads(timeout);
  if (dispatcher_) {
    dispatcher_->Stop(timeout);
  }
  if (apu_) {
    apu_->StopWorker();
  }
}

void Runtime::ExitTitle(runtime::TitleExitReason reason) {
  ReportTitleExit(reason);
  TerminateTitle();
}

}  // namespace rex
