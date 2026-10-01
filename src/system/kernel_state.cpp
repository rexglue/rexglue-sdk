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

#include <chrono>
#include <cstring>
#include <filesystem>
#include <ranges>
#include <string>
#include <type_traits>
#include <variant>

#include <fmt/format.h>
#include <rex/assert.h>
#include <rex/image_info.h>
#include <rex/logging.h>
#include <rex/math.h>
#include <rex/ppc/function.h>
#include <rex/runtime.h>
#include <rex/platform/atomic.h>
#include <rex/string.h>
#include <rex/kernel/xboxkrnl/threading.h>
#include <rex/system/kernel_module.h>
#include <rex/system/kernel_state.h>

#include <rex/api.h>
#include <rex/system/function_dispatcher.h>
#include <chrono>
#include <thread>

#include <rex/system/guest_path.h>
#include <rex/system/user_module.h>
#include <rex/system/xevent.h>
#include <rex/system/xmodule.h>
#include <rex/system/xmutant.h>
#include <rex/system/xnotifylistener.h>
#include <rex/system/xobject.h>
#include <rex/system/xsemaphore.h>
#include <rex/system/xthread.h>
#include <rex/system/xtimer.h>

#include "system/thread_wait.h"

namespace rex::system {

constexpr uint32_t kDeferredOverlappedDelayMillis = 100;

// This is a global object initialized with the XboxkrnlModule.
// It references the current kernel state object that all kernel methods should
// be using to stash their variables.
KernelState* shared_kernel_state_ = nullptr;

KernelState* kernel_state() {
  return shared_kernel_state_;
}

KernelState::KernelState(Runtime* emulator)
    : emulator_(emulator),
      config_(emulator->config().runtime.kernel),
      memory_(emulator->memory()),
      object_table_(memory_),
      dispatch_thread_running_(false),
      dpc_list_(emulator->memory()) {
  function_dispatcher_ = emulator->function_dispatcher();
  file_system_ = emulator->file_system();

  app_manager_ = std::make_unique<xam::AppManager>();
  user_profile_ = std::make_unique<xam::UserProfile>();
  user_profile_->set_kernel_state(this);

  auto user_data_root = emulator_->user_data_root();
  if (!user_data_root.empty()) {
    user_data_root = std::filesystem::absolute(user_data_root);
  }
  content_manager_ = std::make_unique<xam::ContentManager>(this, user_data_root);

  if (shared_kernel_state_ != nullptr) {
    REXLOG_ERROR(system, "KernelState constructed but shared_kernel_state_ already set");
    rex::FatalError("Double initialization of KernelState");
  }
  shared_kernel_state_ = this;

  // Allocate KernelGuestGlobals early so xboxkrnl module can wire exports.
  kernel_guest_globals_ = memory_->SystemHeapAlloc(sizeof(KernelGuestGlobals));
  auto globals = memory_->TranslateVirtual<KernelGuestGlobals*>(kernel_guest_globals_);
  std::memset(globals, 0, sizeof(KernelGuestGlobals));

  // Initialize object type pool tags
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

  // Initialize UsbdBootEnumerationDoneEvent as a signaled manual-reset event
  auto* usbd_event = reinterpret_cast<X_DISPATCH_HEADER*>(&globals->UsbdBootEnumerationDoneEvent);
  usbd_event->type = 1;  // NotificationEvent
  usbd_event->signal_state = 1;

  // Initialize OddObj self-referencing pointer
  uint32_t oddobject_offset = kernel_guest_globals_ + offsetof(KernelGuestGlobals, OddObj);
  globals->OddObj.field0 = 0x1000000;
  globals->OddObj.field4 = 1;
  globals->OddObj.points_to_self =
      oddobject_offset + offsetof(X_UNKNOWN_TYPE_REFED, points_to_self);
  globals->OddObj.points_to_prior = globals->OddObj.points_to_self;

  // Initialize process structs
  InitializeProcess(&globals->idle_process, X_PROCTYPE_IDLE, 0, 0, 0);
  globals->idle_process.quantum = 0x7F;

  InitializeProcess(&globals->system_process, X_PROCTYPE_SYSTEM, 2, 5, 9);
  SetProcessTLSVars(&globals->system_process, 32, 0, 0);

  // Title process needs minimal initialization here so threads created before
  // SetExecutableModule() (e.g. XMA decoder) can link into its thread_list.
  // SetExecutableModule() will re-initialize it fully with XEX header values.
  InitializeProcess(&globals->title_process, X_PROCTYPE_USER, 0, 0, 0);
}

void KernelState::InitializeProcess(X_KPROCESS* process, uint32_t process_type, uint8_t unk_18,
                                    uint8_t unk_19, uint8_t unk_1A) {
  process->unk_18 = unk_18;
  process->unk_19 = unk_19;
  process->unk_1A = unk_1A;
  process->unk_1B = 0x06;
  process->quantum = 60;
  process->clrdataa_masked_ptr = 0;
  process->thread_count = 0;
  process->kernel_stack_size = 16 * 1024;
  process->tls_slot_size = 0x80;
  process->process_type = static_cast<uint8_t>(process_type);
  util::XeInitializeListHead(&process->thread_list, memory_);
  util::XeInitializeListHead(&process->unk_54, memory_);
}

void KernelState::SetProcessTLSVars(X_KPROCESS* process, uint32_t num_slots, uint32_t tls_data_size,
                                    uint32_t tls_raw_data_address) {
  uint32_t slots_padded = (num_slots + 3) & ~uint32_t(3);
  process->tls_slot_size = static_cast<uint16_t>(4 * slots_padded);
  process->tls_static_data_address = tls_raw_data_address;

  // Initialize TLS bitmap - mark used slots with 1s in HIGH bits (matching xenia).
  // Xenia formula: bitmap[count_div32] = -1 << (32 - ((num_slots + 3) & 0x1C))
  uint32_t bitmap_slots = slots_padded / 32;
  for (uint32_t i = 0; i < 8; i++) {
    if (i < bitmap_slots) {
      process->tls_slot_bitmap[i] = 0xFFFFFFFF;
    } else if (i == bitmap_slots) {
      uint32_t remaining = slots_padded % 32;
      process->tls_slot_bitmap[i] = remaining ? ~0u << (32 - remaining) : 0;
    } else {
      process->tls_slot_bitmap[i] = 0;
    }
  }
}

KernelState::~KernelState() {
  app_manager_.reset();

  // Stop the dispatch thread before touching the object table
  if (dispatch_thread_running_) {
    dispatch_thread_running_ = false;
    dispatch_cond_.notify_all();
    WaitOn(this, dispatch_thread_, 0, 0, 0, nullptr);
    object_table_.Close(dispatch_thread_);
    dispatch_thread_ = 0;
  }

  // Unload through UnloadUserModule so the recompiled-DLL teardown runs.
  // call_entry=false because guest DllMain is unsafe at shutdown.
  const std::vector<X_HANDLE> loaded_modules = modules_.user_modules();
  for (X_HANDLE module : std::views::reverse(loaded_modules)) {
    UnloadUserModule(module, /*call_entry=*/false);
  }
  modules_.Shutdown();
  if (!module_libraries_.empty()) {
    REXLOG_ERROR(system, "~KernelState: {} recompiled libraries still loaded after module sweep",
                 module_libraries_.size());
    module_libraries_.clear();
  }

  for (X_HANDLE handle : notify_listeners_) {
    object_table_.Close(handle);
  }
  notify_listeners_.clear();

  // Safe to reset now: Runtime::Shutdown() has already stopped graphics,
  // audio, and input before destroying KernelState.
  object_table_.Reset();

  // Destroy any host fibers that were not explicitly cleaned up.
  for (auto& [guest_addr, info] : fiber_map_) {
    if (info.host_fiber) {
      info.host_fiber->Destroy();
    }
    if (!info.is_thread_fiber && info.guest_stack_bottom) {
      memory_->LookupHeap(0x70000000)->Release(info.guest_stack_bottom);
    }
    if (info.guest_context_addr) {
      memory_->SystemHeapFree(info.guest_context_addr);
    }
  }
  fiber_map_.clear();

  if (kernel_guest_globals_) {
    memory_->SystemHeapFree(kernel_guest_globals_);
    kernel_guest_globals_ = 0;
  }

  if (shared_kernel_state_ == this) {
    shared_kernel_state_ = nullptr;
  } else {
    REXLOG_ERROR(system, "~KernelState: shared_kernel_state_ does not match this instance");
  }

  // Drain last: FreeLibrary runs the DLL's host static dtors.
  deferred_unload_libraries_.clear();
}

KernelState* KernelState::shared() {
  return shared_kernel_state_;
}

uint32_t KernelState::title_id() const {
  auto module = object_table_.Get<ModuleObject>(modules_.executable());
  assert_true(module);

  xex2_opt_execution_info* exec_info = 0;
  UserModuleGetOptHeader(*module, XEX_HEADER_EXECUTION_INFO, &exec_info);

  if (exec_info) {
    return exec_info->title_id;
  }

  return 0;
}

util::XdbfGameData KernelState::title_xdbf() const {
  auto module = object_table_.Get<ModuleObject>(modules_.executable());
  assert_true(module);
  return module_xdbf(*module);
}

util::XdbfGameData KernelState::module_xdbf(const ModuleObject& exec_module) const {
  uint32_t resource_data = 0;
  uint32_t resource_size = 0;
  if (XSUCCEEDED(ModuleGetSection(exec_module,
                                  fmt::format("{:08X}", UserModuleTitleID(exec_module)),
                                  &resource_data, &resource_size))) {
    util::XdbfGameData db(memory()->TranslateVirtual(resource_data), resource_size);
    return db;
  }
  return util::XdbfGameData(nullptr, resource_size);
}

void KernelState::SetLoadedAchievements(std::vector<AchievementInfo> achievements) {
  achievement_manager_.ReplaceAchievements(std::move(achievements));
}

AchievementListenerHandle KernelState::RegisterAchievementUnlockCallback(
    AchievementUnlockCallback cb) {
  return achievement_manager_.RegisterUnlockCallback(
      [cb = std::move(cb)](const AchievementEvent& event) { cb(event.achievement); });
}

void KernelState::UnlockAchievement(uint32_t id) {
  (void)achievement_manager_.UnlockAchievement(id, AchievementNotification::kShow);
}

bool KernelState::IsAchievementUnlocked(uint32_t id) const {
  return achievement_manager_.IsUnlocked(id);
}

uint64_t KernelState::GetAchievementUnlockTime(uint32_t id) const {
  return achievement_manager_.GetUnlockTime(id);
}

std::vector<AchievementInfo> KernelState::loaded_achievements() const {
  return achievement_manager_.ListAchievements();
}

void KernelState::LoadAchievementsData() {
  std::vector<AchievementInfo> achievements;

  const util::XdbfGameData db = title_xdbf();
  if (db.is_valid()) {
    const XLanguage language =
        db.GetExistingLanguage(static_cast<XLanguage>(config_.user_language));
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

  SetLoadedAchievements(std::move(achievements));
  if (auto metadata_path = emulator_->FindMetadataPath("achievements.toml")) {
    achievement_manager_.LoadMetadataFile(*metadata_path);
  }

  // Set up the unlock save path and restore persisted state.
  const auto user_root = emulator_->user_data_root();
  if (!user_root.empty()) {
    achievement_manager_.SetUnlockSavePath(user_root / "achievements" /
                                           fmt::format("{:08X}.toml", title_id()));
    achievement_manager_.LoadUnlockState();
  }
}

uint32_t KernelState::process_type() const {
  auto globals = memory_->TranslateVirtual<KernelGuestGlobals*>(kernel_guest_globals_);
  return globals->title_process.process_type;
}

void KernelState::set_process_type(uint32_t value) {
  auto globals = memory_->TranslateVirtual<KernelGuestGlobals*>(kernel_guest_globals_);
  globals->title_process.process_type = uint8_t(value);
}

uint32_t KernelState::AllocateTLS(PPCContext* context) {
  if (!context) {
    REX_FAIL(system, X_TLS_OUT_OF_INDEXES, "AllocateTLS: null PPCContext");
  }

  auto globals = memory()->TranslateVirtual<KernelGuestGlobals*>(GetKernelGuestGlobals());
  auto tls_lock = &globals->tls_lock;
  auto old_irql = kernel::xboxkrnl::KeKfAcquireSpinLock(context, tls_lock);

  uint32_t result = X_TLS_OUT_OF_INDEXES;

  auto current_thread = ThreadRecord::GetCurrentThread();
  if (!current_thread) {
    REXLOG_ERROR(system, "AllocateTLS: no current thread");
    kernel::xboxkrnl::KeKfReleaseSpinLock(context, tls_lock, old_irql);
    return X_TLS_OUT_OF_INDEXES;
  }

  auto process_ptr = memory()->TranslateVirtual<X_KPROCESS*>(
      static_cast<uint32_t>(current_thread->guest_object<X_KTHREAD>()->process));
  if (!process_ptr) {
    REXLOG_ERROR(system, "AllocateTLS: failed to resolve current process");
    kernel::xboxkrnl::KeKfReleaseSpinLock(context, tls_lock, old_irql);
    return X_TLS_OUT_OF_INDEXES;
  }

  for (uint32_t bitmap_index = 0; bitmap_index < 8; ++bitmap_index) {
    uint32_t bitmap_value = static_cast<uint32_t>(process_ptr->tls_slot_bitmap[bitmap_index]);
    uint32_t leading_zeros = rex::lzcnt(bitmap_value);
    if (leading_zeros == 32) {
      continue;
    }

    uint32_t slot = bitmap_index * 32 + leading_zeros;
    if (slot >= 256) {
      continue;
    }

    uint32_t bit_index = 31 - leading_zeros;
    process_ptr->tls_slot_bitmap[bitmap_index] = bitmap_value & ~(1U << bit_index);
    result = slot;
    break;
  }

  kernel::xboxkrnl::KeKfReleaseSpinLock(context, tls_lock, old_irql);
  return result;
}

void KernelState::FreeTLS(PPCContext* context, uint32_t slot) {
  if (!context) {
    REXLOG_ERROR(system, "FreeTLS: null PPCContext");
    return;
  }
  if (slot >= 256) {
    REXLOG_WARN(system, "FreeTLS: invalid TLS slot {}", slot);
    return;
  }

  auto current_thread = ThreadRecord::GetCurrentThread();
  if (!current_thread) {
    REXLOG_ERROR(system, "FreeTLS: no current thread");
    return;
  }

  auto current_kthread = current_thread->guest_object<X_KTHREAD>();
  if (!current_kthread) {
    REXLOG_ERROR(system, "FreeTLS: failed to resolve current KTHREAD");
    return;
  }

  auto process_ptr =
      memory()->TranslateVirtual<X_KPROCESS*>(static_cast<uint32_t>(current_kthread->process));
  if (!process_ptr) {
    REXLOG_ERROR(system, "FreeTLS: failed to resolve current process");
    return;
  }

  auto globals = memory()->TranslateVirtual<KernelGuestGlobals*>(GetKernelGuestGlobals());
  auto tls_lock = &globals->tls_lock;
  auto old_irql = kernel::xboxkrnl::KeKfAcquireSpinLock(context, tls_lock);

  uint32_t bitmap_index = slot / 32;
  uint32_t bit_mask = 1U << (31 - (slot % 32));
  uint32_t bitmap_value = static_cast<uint32_t>(process_ptr->tls_slot_bitmap[bitmap_index]);

  if (bitmap_value & bit_mask) {
    REXLOG_WARN(system, "FreeTLS: slot {} already free", slot);
    kernel::xboxkrnl::KeKfReleaseSpinLock(context, tls_lock, old_irql);
    return;
  }

  const std::vector<ThreadPin> threads = threads_.All();
  uint32_t current_process_ptr = static_cast<uint32_t>(current_kthread->process);

  for (const ThreadPin& thread : threads) {
    if (!thread || !thread->is_guest_thread()) {
      continue;
    }
    auto thread_kthread = thread->guest_object<X_KTHREAD>();
    if (thread_kthread && static_cast<uint32_t>(thread_kthread->process) == current_process_ptr) {
      thread->SetTLSValue(slot, 0);
    }
  }

  process_ptr->tls_slot_bitmap[bitmap_index] = bitmap_value | bit_mask;

  kernel::xboxkrnl::KeKfReleaseSpinLock(context, tls_lock, old_irql);
}

void KernelState::RegisterTitleTerminateNotification(uint32_t routine, uint32_t priority) {
  TerminateNotification notify;
  notify.guest_routine = routine;
  notify.priority = priority;

  terminate_notifications_.push_back(notify);
}

void KernelState::RemoveTitleTerminateNotification(uint32_t routine) {
  for (auto it = terminate_notifications_.begin(); it != terminate_notifications_.end(); it++) {
    if (it->guest_routine == routine) {
      terminate_notifications_.erase(it);
      break;
    }
  }
}

X_HANDLE KernelState::GetKernelModule(const std::string_view name) {
  assert_true(modules_.IsKernel(name));
  return modules_.FindKernel(name);
}

X_HANDLE KernelState::GetModule(const std::string_view name, bool user_only) {
  if (name.empty()) {
    return GetExecutableModule();
  } else if (rex::utf8_equal_case(name, "kernel32.dll")) {
    return 0;
  }

  if (!user_only) {
    if (X_HANDLE kernel_module = modules_.FindKernel(name)) {
      return kernel_module;
    }
  }

  std::string path(name);
  if (auto entry = file_system_->Stat(name)) {
    path = entry->absolute_path;
  }

  return modules_.FindUser(path);
}

X_HANDLE KernelState::PrepareModuleLaunch(X_HANDLE module_handle) {
  auto module = object_table_.Get<ModuleObject>(module_handle);
  if (!module || !module->processor_module->is_executable()) {
    return 0;
  }

  SetExecutableModule(module_handle);
  REXLOG_DEBUG(system, "KernelState: Preparing module launch...");

  // Create a thread to run in.
  // We start suspended so the caller can inspect/attach before resume.
  auto record =
      std::make_unique<ThreadRecord>(this, module->user.stack_size, 0, module->user.entry_point, 0,
                                     X_CREATE_SUSPENDED, true, true);

  // We know this is the 'main thread'.
  record->set_name("Main XThread");

  X_HANDLE handle = 0;
  X_STATUS result = threads_.Launch(std::move(record), &handle);
  if (XFAILED(result)) {
    REX_FAIL(system, 0, "Could not create launch thread: {:08X}", result);
  }

  return handle;
}

X_HANDLE KernelState::LaunchModule(X_HANDLE module) {
  X_HANDLE handle = PrepareModuleLaunch(module);
  if (handle) {
    if (auto thread = threads_.FindByHandle(handle)) {
      thread->Resume();
    }
  }
  return handle;
}

X_HANDLE KernelState::GetExecutableModule() {
  return modules_.executable();
}

void KernelState::SetExecutableModule(X_HANDLE module_handle) {
  auto module = object_table_.Get<ModuleObject>(module_handle);
  if (auto current = object_table_.Get<ModuleObject>(modules_.executable());
      current && module && &*current == &*module) {
    return;
  }
  modules_.SetExecutable(module_handle);
  if (!module) {
    return;
  }

  // Update title process fields from the executable module.
  // Do NOT call InitializeProcess() again - it was already called in the
  // constructor, and threads (XMA decoder, dispatch) may already be linked
  // into the thread_list. Reinitializing would orphan them.
  auto globals = memory_->TranslateVirtual<KernelGuestGlobals*>(kernel_guest_globals_);
  auto* pib = &globals->title_process;
  pib->unk_18 = 10;
  pib->unk_19 = 13;
  pib->unk_1A = 17;

  // Read default stack size from XEX header, align to 4KB, clamp to min 16KB.
  uint32_t default_stack_size = 0;
  UserModuleGetOptHeader(*module, XEX_HEADER_DEFAULT_STACK_SIZE, &default_stack_size);
  if (default_stack_size) {
    default_stack_size = rex::round_up(default_stack_size, 4096u);
    if (default_stack_size < 16 * 1024) {
      default_stack_size = 16 * 1024;
    }
    pib->kernel_stack_size = default_stack_size;
  }

  // Update title process TLS info from the executable module.
  xex2_opt_tls_info* tls_header = nullptr;
  UserModuleGetOptHeader(*module, XEX_HEADER_TLS_INFO, &tls_header);
  if (tls_header) {
    pib->tls_data_size = tls_header->data_size;
    pib->tls_raw_data_size = tls_header->raw_data_size;
    SetProcessTLSVars(pib, tls_header->slot_count, tls_header->data_size,
                      tls_header->raw_data_address);
  }

  auto xboxkrnl = object_table_.Get<ModuleObject>(GetKernelModule("xboxkrnl.exe"));
  if (xboxkrnl) {
    if (const uint32_t address =
            KernelModuleVariable(*xboxkrnl, 0x0193 /* XexExecutableModuleHandle */)) {
      auto variable_ptr = memory_->TranslateVirtual<be<uint32_t>*>(address);
      *variable_ptr = module->hmodule_ptr;
    }
    if (const uint32_t address = KernelModuleVariable(*xboxkrnl, 0x01AF /* ExLoadedImageName */)) {
      char* variable_ptr = memory_->TranslateVirtual<char*>(address);
      rex::copy_truncating(variable_ptr, module->path, kExLoadedImageNameSize);
    }
  }

  // Spin up deferred dispatch worker.
  // TODO(benvanik): move someplace more appropriate (out of ctor, but around
  // here).
  if (!dispatch_thread_running_) {
    dispatch_thread_running_ = true;
    auto record = std::make_unique<ThreadRecord>(this, 128 * 1024, 0, [this]() {
      std::unique_lock lock(dispatch_mutex_, std::defer_lock);
      while (dispatch_thread_running_) {
        lock.lock();
        if (dispatch_queue_.empty()) {
          dispatch_cond_.wait(lock);
          if (!dispatch_thread_running_) {
            lock.unlock();
            break;
          }
        }
        auto fn = std::move(dispatch_queue_.front());
        dispatch_queue_.pop_front();
        REXLOG_DEV(system, "Dispatch thread processing queued item ({} remaining)",
                   dispatch_queue_.size());
        lock.unlock();

        // Throws out of fn leave the originating overlapped uncompleted and
        // the waiting guest thread stuck; fail visibly rather than swallow.
        try {
          fn();
        } catch (const std::exception& e) {
          REX_FATAL(system, "Dispatch thread: deferred completion threw '{}'", e.what());
        } catch (...) {
          REX_FATAL(system, "Dispatch thread: deferred completion threw non-std exception");
        }
        REXLOG_DEV(system, "Dispatch thread completed item");
      }
      return 0;
    });
    record->set_name("Kernel Dispatch");
    X_STATUS create_status = threads_.Launch(std::move(record), &dispatch_thread_);
    if (XFAILED(create_status)) {
      dispatch_thread_running_ = false;
      dispatch_thread_ = 0;
      REX_FATAL(system, "Failed to create kernel dispatch thread (status {:#x})", create_status);
    }
  }

  LoadAchievementsData();
}

X_HANDLE KernelState::LoadKernelModule(X_HANDLE created) {
  return modules_.AddKernel(created);
}

X_HANDLE KernelState::LoadUserModule(const std::string_view raw_name, bool call_entry) {
  // Some games try to load relative to launch module, others specify full path.
  auto name = rex::utf8_find_name_from_guest_path(raw_name);
  std::string path(raw_name);
  if (name == raw_name) {
    auto executable = object_table_.Get<ModuleObject>(modules_.executable());
    assert_true(executable);
    path = rex::utf8_join_guest_paths(rex::utf8_find_base_guest_path(executable->path), name);
  }

  // The loading mark serializes concurrent loaders of the same path; we
  // can't hold the global lock across LoadFromFile or DllMain ATTACH.
  if (X_HANDLE existing = modules_.FindUser(path)) {
    return existing;
  }
  if (!modules_.BeginLoad(path)) {
    REXLOG_WARN(system, "LoadUserModule: '{}' already being loaded by another thread", path);
    return 0;
  }

  struct LoadingMarkerGuard {
    ModuleRegistry* registry;
    const std::string& key;
    ~LoadingMarkerGuard() { registry->EndLoad(key); }
  } loading_guard{&modules_, path};

  if (X_HANDLE existing = modules_.FindUser(path)) {
    return existing;
  }

  const X_HANDLE created = CreateUserModule(this);
  if (!created) {
    return 0;
  }
  auto module = object_table_.Get<ModuleObject>(created);
  X_STATUS status = UserModuleLoadFromFile(this, *module, path);
  if (XFAILED(status)) {
    object_table_.Close(created);
    return 0;
  }

  // Wire recompiled code (if any) before publishing to the module registry, so a
  // failure in this block leaves no half-loaded entry behind.
  auto recomp = FindRecompiledModule(path);
  bool wired_recomp = false;
  if (recomp && !recomp->shared_lib_name.empty()) {
    const std::string& lib_key = recomp->guest_path;

    bool inserted;
    {
      std::lock_guard lock(recompiled_mutex_);
      inserted = module_libraries_.emplace(lib_key, rex::platform::DynamicLibrary{}).second;
    }
    if (!inserted) {
      REXLOG_ERROR(system, "Recompiled module '{}' already loaded; refusing duplicate load",
                   lib_key);
      object_table_.Close(created);
      return 0;
    }

    rex::platform::DynamicLibrary library_local;
    if (!library_local.Load(std::filesystem::path(recomp->shared_lib_name),
                            rex::platform::SymbolResolution::kImmediate)) {
      REXLOG_ERROR(system, "Failed to load shared library for module '{}'", recomp->pe_name);
    } else {
      auto register_func = reinterpret_cast<runtime::FunctionDispatcher::RegisterFn>(
          library_local.GetRawSymbol("ReXModule_Register"));
      using GetImageInfoFn = const rex::PPCImageInfo* (*)();
      auto get_image_info =
          reinterpret_cast<GetImageInfoFn>(library_local.GetRawSymbol("ReXModule_GetImageInfo"));
      if (!register_func) {
        REXLOG_ERROR(system, "ReXModule_Register not found in '{}'", recomp->shared_lib_name);
      } else if (!get_image_info) {
        REXLOG_ERROR(system, "ReXModule_GetImageInfo not found in '{}'", recomp->shared_lib_name);
      } else {
        auto* xex = UserModuleXex(*module);
        const auto* image_info = get_image_info();
        if (!image_info) {
          REXLOG_ERROR(system, "ReXModule_GetImageInfo returned null for '{}'",
                       recomp->shared_lib_name);
        } else if (image_info->image_base != xex->base_address() ||
                   image_info->image_size != xex->image_size()) {
          REXLOG_ERROR(system,
                       "Recompiled module '{}' layout does not match loaded XEX "
                       "(DLL image={:08X}-{:08X}, XEX image={:08X}-{:08X})",
                       recomp->pe_name, image_info->image_base,
                       image_info->image_base + image_info->image_size, xex->base_address(),
                       xex->base_address() + xex->image_size());
        } else if (!function_dispatcher_->InitializeFunctionTable(
                       image_info->code_base, image_info->code_size, image_info->image_base,
                       image_info->image_size)) {
          REXLOG_ERROR(system, "InitializeFunctionTable failed for module '{}'", recomp->pe_name);
        } else {
          function_dispatcher_->RegisterModule(lib_key, image_info->code_base, register_func);
          std::lock_guard lock(recompiled_mutex_);
          auto lib_it = module_libraries_.find(lib_key);
          assert_true(lib_it != module_libraries_.end());
          lib_it->second = std::move(library_local);
          wired_recomp = true;
        }
      }
    }

    if (!wired_recomp) {
      {
        std::lock_guard lock(recompiled_mutex_);
        module_libraries_.erase(lib_key);
      }
      object_table_.Close(created);
      return 0;
    }
  }

  if (!modules_.AddUser(created)) {
    object_table_.Close(created);
    return modules_.FindUser(path);
  }
  const X_HANDLE loaded = modules_.FindUser(module->path);

  if (module->user.is_dll_module && module->user.entry_point && call_entry) {
    if (!ThreadRecord::IsInThread()) {
      REXLOG_WARN(system, "DllMain(DLL_PROCESS_ATTACH) skipped for '{}': not on a guest thread",
                  module->name);
    } else {
      auto* thread = ThreadRecord::GetCurrentThread();
      uint64_t args[] = {module->hmodule_ptr, 1 /* DLL_PROCESS_ATTACH */, 0};
      uint64_t dllmain_ret =
          function_dispatcher_->Execute(thread->thread_state(), module->user.entry_point, args, 3);
      if (static_cast<uint32_t>(dllmain_ret) == 0) {
        REXLOG_ERROR(system,
                     "DllMain(DLL_PROCESS_ATTACH) returned FALSE for '{}'; rolling back load",
                     module->name);
        // call_entry=false: the guest already declined ATTACH, so don't run DETACH.
        UnloadUserModule(loaded, /*call_entry=*/false);
        return 0;
      }
    }
  }

  return loaded;
}

void KernelState::UnloadUserModule(X_HANDLE module_handle, bool call_entry) {
  auto module = object_table_.Get<ModuleObject>(module_handle);
  if (!module) {
    REXLOG_ERROR(system, "UnloadUserModule: invalid module handle {:08X}", module_handle);
    return;
  }

  // Run guest DllMain DETACH outside the global lock to avoid deadlock with
  // subsystem mutexes acquired from inside the guest callback.
  if (module->user.is_dll_module && module->user.entry_point && call_entry) {
    if (!ThreadRecord::IsInThread()) {
      REXLOG_WARN(system, "DllMain(DLL_PROCESS_DETACH) skipped for '{}': not on a guest thread",
                  module->name);
    } else {
      auto* thread = ThreadRecord::GetCurrentThread();
      uint64_t args[] = {module->hmodule_ptr, 0 /* DLL_PROCESS_DETACH */, 0};
      function_dispatcher_->Execute(thread->thread_state(), module->user.entry_point, args, 3);
    }
  }

  auto recomp = FindRecompiledModule(module->path);
  if (recomp) {
    const std::string& key = recomp->guest_path;
    auto cleared_range = function_dispatcher_->UnregisterModule(key);
    if (cleared_range) {
      for (X_HANDLE kernel_handle : modules_.kernel_modules()) {
        if (auto kernel_module = object_table_.Get<ModuleObject>(kernel_handle)) {
          KernelModuleInvalidateThunkCacheInRange(*kernel_module, cleared_range->first,
                                                  cleared_range->second);
        }
      }
    }

    if (!recomp->shared_lib_name.empty()) {
      std::lock_guard lock(recompiled_mutex_);
      auto lib_it = module_libraries_.find(key);
      if (lib_it != module_libraries_.end()) {
        deferred_unload_libraries_.push_back(std::move(lib_it->second));
        module_libraries_.erase(lib_it);
      }
    }
  }

  const bool found_module = modules_.RemoveUser(module_handle);
  if (found_module) {
    object_table_.Close(ModuleHandleFromHModule(memory_->TranslateVirtual(module->hmodule_ptr)));
  }

  if (!found_module) {
    REXLOG_ERROR(system, "UnloadUserModule: module '{}' not found in the module registry",
                 module->path);
  }
}

void KernelState::RegisterRecompiledModule(const char* pe_name, const char* guest_path,
                                           const char* shared_lib_name) {
  std::lock_guard lock(recompiled_mutex_);
  RecompiledModuleInfo info;
  info.pe_name = pe_name ? pe_name : "";
  info.guest_path = guest_path ? NormalizeGuestPath(guest_path) : "";
  info.shared_lib_name = shared_lib_name ? shared_lib_name : "";

  for (const auto& existing : recompiled_modules_) {
    if (existing.guest_path == info.guest_path) {
      REXLOG_ERROR(
          system,
          "RegisterRecompiledModule: duplicate guest_path '{}' (existing pe='{}', new pe='{}')",
          info.guest_path, existing.pe_name, info.pe_name);
      return;
    }
  }

  REXLOG_DEBUG(system, "Registered recompiled module: pe='{}' guest='{}' lib='{}'", info.pe_name,
               info.guest_path, info.shared_lib_name);
  recompiled_modules_.push_back(std::move(info));
}

std::optional<KernelState::RecompiledModuleInfo> KernelState::FindRecompiledModule(
    std::string_view guest_path) {
  std::lock_guard lock(recompiled_mutex_);
  auto normalized = NormalizeGuestPath(guest_path);
  for (const auto& info : recompiled_modules_) {
    if (info.guest_path == normalized)
      return info;
  }
  return std::nullopt;
}

void KernelState::SignalAllWaitableObjects() {
  for (PinnedSlot& slot : object_table_.GetAll()) {
    std::visit(
        [](auto& object) {
          using T = std::decay_t<decltype(object)>;
          if constexpr (std::is_same_v<T, EventObject>) {
            EventSet(object);
          } else if constexpr (std::is_same_v<T, MutantObject>) {
            if (ThreadRecord::IsInThread()) {
              MutantRelease(object, false);
            }
          } else if constexpr (std::is_same_v<T, SemaphoreObject>) {
            (void)SemaphoreRelease(object, static_cast<i32>(object.maximum_count), nullptr);
          } else if constexpr (std::is_same_v<T, TimerObject>) {
            TimerCancel(object);
          }
        },
        slot.data());
  }
}

void KernelState::TerminateTitle() {
  REXLOG_DEBUG(system, "KernelState::TerminateTitle");

  const auto timeout = emulator()->config().runtime.terminate_timeout;

  // Guest threads poll this flag in the kernel wait primitives
  // (ThreadRecord::CheckTitleTermination) and self-exit.
  terminating_title_.store(true, std::memory_order_release);

  // Pinned so a thread that wakes and exits below can't be freed mid-drain.
  std::vector<ThreadPin> target_threads;
  for (ThreadPin& thread : threads_.All()) {
    if (!ThreadRecord::IsInThread(thread.get()) && thread->is_guest_thread() &&
        thread->is_running()) {
      target_threads.push_back(std::move(thread));
    }
  }

  // Wake blocked waiters: signal objects (non-alertable waiters) and a bare user
  // callback per target (alertable waits/delays).
  SignalAllWaitableObjects();
  for (auto& thread : target_threads) {
    thread->thread()->QueueUserCallback([] {});
  }

  WaitForThreadsToExit(target_threads, timeout);

  for (auto& thread : target_threads) {
    if (thread->is_running()) {
      REXLOG_WARN(system,
                  "TerminateTitle: thread {:08X} '{}' entry {:08X} still running after {} ms",
                  thread->thread_id(), thread->name(), thread->creation_params()->start_address,
                  timeout.count());
      thread->Terminate(0);
    }
  }

  // A thread that was already in Exit when the bound passed finishes on its own.
  WaitForThreadsToExit(target_threads, timeout);

  // Drop refs before the self-terminate below (which does not return) so they
  // aren't leaked.
  target_threads.clear();

  // Self-terminate if called from a guest thread (e.g. XamLoaderTerminateTitle).
  if (ThreadRecord::IsInThread()) {
    ThreadRecord::GetCurrentThread()->Terminate(0);
  }
}

void KernelState::ExitTitle(runtime::TitleExitReason reason) {
  if (Runtime* runtime = emulator()) {
    runtime->ReportTitleExit(reason);
  }
  TerminateTitle();
}

void KernelState::RegisterNotifyListener(X_HANDLE listener_handle) {
  X_HANDLE owned = 0;
  if (XFAILED(object_table_.Duplicate(listener_handle, &owned))) {
    return;
  }
  auto listener = object_table_.Get<NotifyListenerObject>(owned);
  std::lock_guard lock(notify_mutex_);
  notify_listeners_.push_back(owned);
  if (!has_notified_startup_ && listener->mask & 0x00000001) {
    has_notified_startup_ = true;
    NotifyListenerEnqueue(*listener, 0x00000009, 1);
    NotifyListenerEnqueue(*listener, 0x00000009, 0);
    NotifyListenerEnqueue(*listener, 0x0000000A, 1);
    NotifyListenerEnqueue(*listener, 0x0000000A, 1);
    NotifyListenerEnqueue(*listener, 0x00000012, 0);
    NotifyListenerEnqueue(*listener, 0x00000012, 0);
    NotifyListenerEnqueue(*listener, 0x00000013, 0);
    NotifyListenerEnqueue(*listener, 0x00000013, 0);
  }
}

void KernelState::BroadcastNotification(XNotificationID id, uint32_t data) {
  std::vector<X_HANDLE> snapshot;
  {
    std::lock_guard lock(notify_mutex_);
    REXLOG_DEBUG(system, "BroadcastNotification(id={:#x}, data={}) to {} listeners",
                 static_cast<uint32_t>(id), data, notify_listeners_.size());
    snapshot = notify_listeners_;
  }
  for (X_HANDLE handle : snapshot) {
    if (auto listener = object_table_.Get<NotifyListenerObject>(handle)) {
      NotifyListenerEnqueue(*listener, id, data);
    }
  }
}

void KernelState::CompleteOverlapped(uint32_t overlapped_ptr, X_RESULT result) {
  CompleteOverlappedEx(overlapped_ptr, result, result, 0);
}

void KernelState::CompleteOverlappedEx(uint32_t overlapped_ptr, X_RESULT result,
                                       uint32_t extended_error, uint32_t length) {
  auto ptr = memory()->TranslateVirtual(overlapped_ptr);
  XOverlappedSetResult(ptr, result);
  XOverlappedSetExtendedError(ptr, extended_error);
  XOverlappedSetLength(ptr, length);
  X_HANDLE event_handle = XOverlappedGetEvent(ptr);
  if (event_handle) {
    auto ev = object_table()->Get<EventObject>(event_handle);
    assert_true(static_cast<bool>(ev));
    if (ev) {
      EventSet(*ev);
    }
  }
  if (XOverlappedGetCompletionRoutine(ptr)) {
    X_HANDLE thread_handle = XOverlappedGetContext(ptr);
    auto thread = threads_.FindByHandle(thread_handle);
    if (thread) {
      // Queue APC on the thread that requested the overlapped operation.
      uint32_t routine = XOverlappedGetCompletionRoutine(ptr);
      thread->EnqueueApc(routine, result, length, overlapped_ptr);
    }
  }
}

void KernelState::CompleteOverlappedImmediate(uint32_t overlapped_ptr, X_RESULT result) {
  // TODO(gibbed): there are games that check 'length' of overlapped as
  // an indication of success. WTF?
  // Setting length to -1 when not success seems to be helping.
  uint32_t length = !result ? 0 : 0xFFFFFFFF;
  CompleteOverlappedImmediateEx(overlapped_ptr, result, result, length);
}

void KernelState::CompleteOverlappedImmediateEx(uint32_t overlapped_ptr, X_RESULT result,
                                                uint32_t extended_error, uint32_t length) {
  auto ptr = memory()->TranslateVirtual(overlapped_ptr);
  XOverlappedSetContext(ptr, ThreadRecord::GetCurrentThreadHandle());
  CompleteOverlappedEx(overlapped_ptr, result, extended_error, length);
}

void KernelState::CompleteOverlappedDeferred(std::function<void()> completion_callback,
                                             uint32_t overlapped_ptr, X_RESULT result,
                                             std::function<void()> pre_callback,
                                             std::function<void()> post_callback) {
  CompleteOverlappedDeferredEx(std::move(completion_callback), overlapped_ptr, result, result, 0,
                               pre_callback, post_callback);
}

void KernelState::CompleteOverlappedDeferredEx(std::function<void()> completion_callback,
                                               uint32_t overlapped_ptr, X_RESULT result,
                                               uint32_t extended_error, uint32_t length,
                                               std::function<void()> pre_callback,
                                               std::function<void()> post_callback) {
  CompleteOverlappedDeferredEx(
      [completion_callback, result, extended_error, length](uint32_t& cb_extended_error,
                                                            uint32_t& cb_length) -> X_RESULT {
        completion_callback();
        cb_extended_error = extended_error;
        cb_length = length;
        return result;
      },
      overlapped_ptr, pre_callback, post_callback);
}

void KernelState::CompleteOverlappedDeferred(std::function<X_RESULT()> completion_callback,
                                             uint32_t overlapped_ptr,
                                             std::function<void()> pre_callback,
                                             std::function<void()> post_callback) {
  CompleteOverlappedDeferredEx(
      [completion_callback](uint32_t& extended_error, uint32_t& length) -> X_RESULT {
        auto result = completion_callback();
        extended_error = static_cast<uint32_t>(result);
        length = 0;
        return result;
      },
      overlapped_ptr, pre_callback, post_callback);
}

void KernelState::CompleteOverlappedDeferredEx(
    std::function<X_RESULT(uint32_t&, uint32_t&)> completion_callback, uint32_t overlapped_ptr,
    std::function<void()> pre_callback, std::function<void()> post_callback) {
  REXLOG_DEBUG(system, "CompleteOverlappedDeferredEx: queuing for overlapped {:08X}",
               overlapped_ptr);
  auto ptr = memory()->TranslateVirtual(overlapped_ptr);
  XOverlappedSetResult(ptr, X_ERROR_IO_PENDING);
  XOverlappedSetContext(ptr, ThreadRecord::GetCurrentThreadHandle());
  std::lock_guard lock(dispatch_mutex_);
  dispatch_queue_.push_back(
      [this, completion_callback, overlapped_ptr, pre_callback, post_callback]() {
        REXLOG_DEBUG(system, "Deferred overlapped {:08X}: running pre_callback", overlapped_ptr);
        if (pre_callback) {
          pre_callback();
        }
        REXLOG_DEBUG(system, "Deferred overlapped {:08X}: sleeping {}ms", overlapped_ptr,
                     kDeferredOverlappedDelayMillis);
        rex::platform::Sleep(std::chrono::milliseconds(kDeferredOverlappedDelayMillis));
        uint32_t extended_error, length;
        REXLOG_DEBUG(system, "Deferred overlapped {:08X}: running completion", overlapped_ptr);
        auto result = completion_callback(extended_error, length);
        REXLOG_DEBUG(system, "Deferred overlapped {:08X}: completing with result {:08X}",
                     overlapped_ptr, result);
        CompleteOverlappedEx(overlapped_ptr, result, extended_error, length);
        if (post_callback) {
          REXLOG_DEBUG(system, "Deferred overlapped {:08X}: running post_callback", overlapped_ptr);
          post_callback();
        }
      });
  dispatch_cond_.notify_all();
}

DPCImpersonationScope KernelState::BeginDPCImpersonation() {
  auto* thread = ThreadRecord::GetCurrentThread();
  auto* ctx = thread->thread_state()->context();
  auto pcr = memory_->TranslateVirtual<X_KPCR*>(static_cast<uint32_t>(ctx->r13.u64));
  DPCImpersonationScope scope;
  scope.previous_irql_ = pcr->current_irql;
  pcr->current_irql = IRQL_DISPATCH;
  pcr->prcb_data.dpc_active = 1;
  return scope;
}

void KernelState::EndDPCImpersonation(const DPCImpersonationScope& scope) {
  auto* thread = ThreadRecord::GetCurrentThread();
  auto* ctx = thread->thread_state()->context();
  auto pcr = memory_->TranslateVirtual<X_KPCR*>(static_cast<uint32_t>(ctx->r13.u64));
  pcr->prcb_data.dpc_active = 0;
  pcr->current_irql = scope.previous_irql_;
}

FiberInfo* KernelState::LookupFiber(uint32_t guest_addr) {
  std::lock_guard lock(fiber_mutex_);
  auto it = fiber_map_.find(guest_addr);
  return it != fiber_map_.end() ? &it->second : nullptr;
}

void KernelState::RegisterFiber(uint32_t guest_addr, const FiberInfo& info) {
  std::lock_guard lock(fiber_mutex_);
  fiber_map_[guest_addr] = info;
}

void KernelState::UnregisterFiber(uint32_t guest_addr) {
  std::lock_guard lock(fiber_mutex_);
  fiber_map_.erase(guest_addr);
}

const char* KernelState::GetOrCreateFiberName(uint32_t guest_addr, const char* thread_name) {
  std::lock_guard lock(fiber_name_pool_mutex_);
  auto it = fiber_name_pool_.find(guest_addr);
  if (it != fiber_name_pool_.end()) {
    return it->second.get();
  }
  auto name = fmt::format("Fiber {:08X} ({})", guest_addr, thread_name);
  auto buf = std::make_unique<char[]>(name.size() + 1);
  std::memcpy(buf.get(), name.c_str(), name.size() + 1);
  const char* ptr = buf.get();
  fiber_name_pool_.emplace(guest_addr, std::move(buf));
  return ptr;
}

}  // namespace rex::system
