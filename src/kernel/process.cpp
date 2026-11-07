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

#include <rex/kernel/types/list.h>
#include <rex/kernel/types/status.h>
#include <rex/kernel/types/thread.h>
#include <rex/kernel/title_runtime.h>
#include <rex/kernel/runtime_settings.h>
#include <rex/kernel/kernel_settings.h>
#include <rex/kernel/title_exit.h>
#include <rex/logging/log_category.h>
#include <rex/math.h>
#include <rex/ppc/call.h>
#include <rex/kernel/function_dispatcher.h>
#include <rex/kernel/thread_state.h>
#include <rex/kernel/guest_allocator.h>
#include <rex/kernel/object/module_object.h>
#include <rex/kernel/object/kernel_thread.h>
#include <rex/string/text.h>

#include "kernel/xboxkrnl/loaded_command_line.h"
#include "kernel/apu.h"
#include "kernel/dispatcher.h"
#include "kernel/module_registry.h"
#include "kernel/notification_broadcaster.h"
#include "kernel/object_table.h"
#include "kernel/thread_registry.h"
#include "kernel/thread_wait.h"
#include "kernel/guest_stack.h"
#include "kernel/settings_access.h"
#include "kernel/title_state.h"
#include <rex/kernel/types/xex.h>
#include <rex/xam/title_lifecycle.h>

namespace rex::kernel {

Result<void> kernel::TitleState::CreateSubsystems() {
  object_table_ = std::make_unique<kernel::ObjectTable>(memory_.get());
  object_table_->SetOwner(owner_);
  CreateWorkspace(WorkspaceHeap::ResolveSize(0));
  thread_registry_ = std::make_unique<kernel::ThreadRegistry>(owner_, *object_table_);
  module_registry_ = std::make_unique<kernel::ModuleRegistry>(owner_, *object_table_);
  notifier_ = std::make_unique<kernel::NotificationBroadcaster>(*object_table_);
  dispatcher_ = std::make_unique<kernel::Dispatcher>(owner_);
  BeginServing();
  REX_TRY_VOID(CreateXAM());

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
  // Dispatch header type 0 is a notification event and type 1 is a synchronization event.
  usbd_event->type = 1;
  usbd_event->signal_state = 1;

  for (u32 curve = 0; curve < std::size(globals->ecc_curves); ++curve) {
    globals->ecc_curves[curve] = 0x80000000 + curve;
  }

  u32 oddobject_offset = kernel_guest_globals_ + offsetof(KernelGuestGlobals, self_list_object);
  globals->self_list_object.unk_00 = 0x1000000;
  globals->self_list_object.unk_04 = 1;
  globals->self_list_object.points_to_self =
      oddobject_offset + offsetof(X_UNKNOWN_TYPE_REFED, points_to_self);
  globals->self_list_object.points_to_prior = globals->self_list_object.points_to_self;

  InitializeProcess(&globals->idle_process, kXProcTypeIdle, 0, 0, 0);
  globals->idle_process.quantum = 0x7F;

  InitializeProcess(&globals->system_process, kXProcTypeSystem, 2, 5, 9);
  SetProcessTLSVars(&globals->system_process, 32, 0, 0);

  InitializeProcess(&globals->title_process, kXProcTypeUser, 0, 0, 0);
  SetProcessTLSVars(&globals->title_process, 0, 0, 0);
  return {};
}

void kernel::TitleState::DestroySubsystems() {
  terminating_title_.store(true, std::memory_order_release);
  ReleaseXAMApps();
  if (dispatcher_) {
    dispatcher_->Stop(std::chrono::milliseconds(GetRuntimeSettings().terminate_timeout_ms.Get()));
  }
  if (thread_registry_) {
    std::vector<ThreadPin> threads = thread_registry_->All();
    for (ThreadPin& thread : threads) {
      thread->RequestStop();
      while (thread->GetSuspendCount() > 0 && thread->Resume() == X_STATUS_SUCCESS) {}
    }
    if (xam_) {
      xam_->ShutdownSockets();
    }
    const auto interval =
        std::chrono::milliseconds(GetRuntimeSettings().terminate_timeout_ms.Get());
    WaitForThreadsToStop(threads, interval, [interval](const ThreadPin& thread) {
      REXSYS_WARN("Thread {:08X} '{}' still running after {} ms", thread->GetThreadID(),
                  thread->GetName(), interval.count());
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
  module_registry_.reset();
  notifier_.reset();
  object_table_.reset();
  DestroyXAM();
  workspace_.reset();
  thread_registry_.reset();
  {
    std::lock_guard lock(terminate_mutex_);
    terminate_registrations_.clear();
  }
  terminating_title_.store(false, std::memory_order_release);
  terminate_claimed_.store(false, std::memory_order_release);
}

void kernel::TitleState::CreateWorkspace(u32 size) {
  Result<std::unique_ptr<WorkspaceHeap>> workspace = WorkspaceHeap::Create(*memory_, size);
  if (!workspace) {
    REXSYS_ERROR("Cannot commit a {:#x} byte title workspace, keeping {:#x} bytes", size,
                 workspace_ ? workspace_->GetSize() : 0u);
    return;
  }
  workspace_ = std::move(*workspace);
  if (object_table_) {
    object_table_->SetWorkspace(workspace_.get());
  }
}

void kernel::TitleState::InitializeProcess(X_KPROCESS* process, u32 process_type, u8 unk_18,
                                           u8 unk_19, u8 unk_1a) {
  process->unk_18 = unk_18;
  process->unk_19 = unk_19;
  process->unk_1a = unk_1a;
  process->unk_1b = 0x06;
  process->quantum = 60;
  process->clrdataa_masked_ptr = 0;
  process->thread_count = 0;
  process->kernel_stack_size = kDefaultGuestStackSize;
  process->tls_slot_size = 0x80;
  process->process_type = static_cast<u8>(process_type);
  kernel::InitializeListHead(&process->thread_list, memory_.get());
  kernel::InitializeListHead(&process->unk_54, memory_.get());
}

void kernel::TitleState::SetProcessTLSVars(X_KPROCESS* process, u32 num_slots, u32 tls_data_size,
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

u32 kernel::TitleState::GetProcessType() const {
  auto globals = memory_->TranslateVirtual<const KernelGuestGlobals*>(kernel_guest_globals_);
  return globals->title_process.process_type;
}

void kernel::TitleState::SetProcessType(u32 value) {
  auto globals = memory_->TranslateVirtual<KernelGuestGlobals*>(kernel_guest_globals_);
  globals->title_process.process_type = static_cast<u8>(value);
}

u32 kernel::TitleState::GetObjectTypeAddress(ObjectType type) const {
  if (!kernel_guest_globals_) {
    return 0;
  }
  switch (type) {
    case ObjectType::kEvent:
      return kernel_guest_globals_ + offsetof(KernelGuestGlobals, ExEventObjectType);
    case ObjectType::kSemaphore:
      return kernel_guest_globals_ + offsetof(KernelGuestGlobals, ExSemaphoreObjectType);
    case ObjectType::kMutant:
      return kernel_guest_globals_ + offsetof(KernelGuestGlobals, ExMutantObjectType);
    case ObjectType::kTimer:
      return kernel_guest_globals_ + offsetof(KernelGuestGlobals, ExTimerObjectType);
    case ObjectType::kThread:
      return kernel_guest_globals_ + offsetof(KernelGuestGlobals, ExThreadObjectType);
    case ObjectType::kFile:
      return kernel_guest_globals_ + offsetof(KernelGuestGlobals, IoFileObjectType);
    case ObjectType::kIOCompletion:
      return kernel_guest_globals_ + offsetof(KernelGuestGlobals, IoCompletionObjectType);
    case ObjectType::kSymbolicLink:
      return kernel_guest_globals_ + offsetof(KernelGuestGlobals, ObSymbolicLinkObjectType);
    case ObjectType::kNotifyListener:
    case ObjectType::kModule:
    case ObjectType::kXAM:
      return 0;
  }
  return 0;
}

void kernel::TitleState::RegisterTitleTerminateNotification(u32 registration) {
  std::lock_guard lock(terminate_mutex_);
  if (std::ranges::find(terminate_registrations_, registration) == terminate_registrations_.end()) {
    terminate_registrations_.push_back(registration);
  }
}

void kernel::TitleState::RemoveTitleTerminateNotification(u32 registration) {
  std::lock_guard lock(terminate_mutex_);
  std::erase(terminate_registrations_, registration);
}

bool kernel::TitleState::RunTitleTerminateNotifications(std::chrono::milliseconds timeout) {
  std::vector<u32> registrations;
  {
    std::lock_guard lock(terminate_mutex_);
    registrations = std::exchange(terminate_registrations_, {});
  }
  if (registrations.empty() || !dispatcher_ || !dispatcher_->IsRunning()) {
    return false;
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
    kernel::ThreadState* state = kernel::ThreadState::Get();
    for (const u32 registration : registrations) {
      const u32 routine = memory->TranslateVirtual<X_EX_TITLE_TERMINATE_REGISTRATION*>(registration)
                              ->notification_routine;
      progress->routine = routine;
      REXSYS_INFO("Title terminate notification {:08X} with registration {:08X}", routine,
                  registration);
      u64 args[] = {registration};
      functions->ExecuteTrap(state, routine, args, rex::CountOf(args));
    }
    progress->routine = 0;
    progress->done.set_value();
  };
  if (dispatcher_->IsDispatchThread()) {
    run();
    return false;
  }
  dispatcher_->Post(std::move(run));
  if (done.wait_for(timeout) == std::future_status::timeout) {
    REXSYS_WARN("Title terminate notification {:08X} still running after {} ms",
                progress->routine.load(), timeout.count());
    dispatcher_->RequestStop();
    return true;
  }
  return false;
}

void kernel::TitleState::SetExecutableModule(X_HANDLE module_handle) {
  auto module = object_table_->Get<ModuleObject>(module_handle);
  if (auto current =
          object_table_->Get<ModuleObject>(module_registry_->GetExecutable().value_or(0));
      current && module && &*current == &*module) {
    return;
  }
  module_registry_->SetExecutable(module_handle);
  if (!module) {
    return;
  }

  auto globals = memory_->TranslateVirtual<KernelGuestGlobals*>(kernel_guest_globals_);
  auto* process = &globals->title_process;
  process->unk_18 = 10;
  process->unk_19 = 13;
  process->unk_1a = 17;

  u32 default_stack_size = 0;
  module->OptHeader(kXEXHeaderDefaultStackSize, &default_stack_size);
  if (default_stack_size) {
    process->kernel_stack_size =
        ResolveGuestStackSize(default_stack_size, 0, kMinimumThreadStackSize);
  }

  XEXOptTLSInfo* tls_header = nullptr;
  module->OptHeader(kXEXHeaderTLSInfo, &tls_header);
  if (tls_header) {
    process->tls_raw_data_size = tls_header->raw_data_size;
    SetProcessTLSVars(process, tls_header->slot_count, tls_header->data_size,
                      tls_header->raw_data_address);
  }
  u32 workspace_size_address = 0;
  module->GetOptHeaderGuestAddress(kXEXHeaderTitleWorkspaceSize, &workspace_size_address);
  const u32 workspace_size = WorkspaceHeap::ResolveSize(
      workspace_size_address
          ? u32{*memory_->TranslateVirtual<const be_u32*>(workspace_size_address)}
          : 0);
  if (!workspace_ || workspace_->GetSize() != workspace_size) {
    CreateWorkspace(workspace_size);
  }

  REX_ASSERT(module_registry_->IsKernelModule("xboxkrnl.exe"));
  auto xboxkrnl = object_table_->Get<ModuleObject>(
      module_registry_->FindKernelModule("xboxkrnl.exe").value_or(0));
  if (xboxkrnl) {
    if (const u32 address = xboxkrnl->GetVariable(0x0193 /* executable module handle */)) {
      auto variable_ptr = memory_->TranslateVirtual<be_u32*>(address);
      *variable_ptr = module->hmodule_ptr;
    }
    if (const u32 address = xboxkrnl->GetVariable(0x01AF /* ExLoadedImageName */)) {
      char* variable_ptr = memory_->TranslateVirtual<char*>(address);
      rex::string::CopyTruncating(variable_ptr, module->path, kExLoadedImageNameSize);
    }
    if (const u32 address = xboxkrnl->GetVariable(0x01AE /* ExLoadedCommandLine */)) {
      const std::string arguments = GetKernelSettings().command_line.Get();
      rex::string::CopyTruncating(memory_->TranslateVirtual<char*>(address),
                                  LoadedCommandLine(module->path, arguments),
                                  LoadedCommandLineCapacity(arguments));
    }
  }

  dispatcher_->Start();

  LoadAchievements();
}

void kernel::TitleState::TerminateTitle() {
  REXSYS_DEBUG("TitleRuntime::TerminateTitle");
  if (KernelThread::IsInThread() && terminate_claimed_.exchange(true, std::memory_order_acq_rel)) {
    KernelThread::GetCallingThread()->Exit(0);
  }

  const auto timeout = std::chrono::milliseconds(GetRuntimeSettings().terminate_timeout_ms.Get());
  const bool notification_overran = RunTitleTerminateNotifications(timeout);
  StopGuestThreads(timeout, notification_overran);

  if (KernelThread::IsInThread()) {
    KernelThread::GetCallingThread()->Exit(0);
  }
}

void kernel::TitleState::StopGuestThreads(std::chrono::milliseconds timeout,
                                          bool include_dispatch) {
  terminating_title_.store(true, std::memory_order_release);
  if (!thread_registry_) {
    return;
  }

  std::vector<ThreadPin> target_threads;
  for (ThreadPin& thread : thread_registry_->All()) {
    if (!KernelThread::IsInThread(thread.GetThread()) && thread->IsGuestThread()) {
      target_threads.push_back(std::move(thread));
    }
  }

  if (include_dispatch && dispatcher_) {
    if (ThreadPin dispatch = dispatcher_->FindThread()) {
      target_threads.push_back(std::move(dispatch));
    }
  }

  for (auto& thread : target_threads) {
    thread->RequestStop();
    while (thread->GetSuspendCount() > 0 && thread->Resume() == X_STATUS_SUCCESS) {}
  }
  if (xam_) {
    xam_->ShutdownSockets();
  }

  WaitForThreadsToStop(target_threads, timeout, [timeout](const ThreadPin& thread) {
    REXSYS_WARN("Guest thread {:08X} '{}' entry {:08X} still running after {} ms",
                thread->GetThreadID(), thread->GetName(),
                thread->GetCreationParams()->start_address, timeout.count());
  });
}

void kernel::TitleState::StopThreads() {
  const auto timeout = std::chrono::milliseconds(GetRuntimeSettings().terminate_timeout_ms.Get());
  StopGuestThreads(timeout);
  if (dispatcher_) {
    dispatcher_->Stop(timeout);
  }
  if (apu_) {
    apu_->StopWorker();
  }
}

void kernel::TitleState::ExitTitle(TitleExitReason reason) {
  ReportTitleExit(reason);
  TerminateTitle();
}

}  // namespace rex::kernel
