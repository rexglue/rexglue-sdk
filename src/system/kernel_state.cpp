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

#include <cstring>
#include <string>

#include <fmt/format.h>

#include <rex/assert.h>
#include <rex/logging.h>
#include <rex/math.h>
#include <rex/ppc/function.h>
#include <rex/runtime.h>
#include <rex/stream.h>
#include <rex/string.h>
#include <rex/string/util.h>
#include <rex/kernel/xboxkrnl/threading.h>
#include <rex/system/kernel_module.h>
#include <rex/system/kernel_state.h>
#include <rex/system/function_dispatcher.h>
#include <rex/system/user_module.h>
#include <rex/system/xevent.h>
#include <rex/system/xmodule.h>
#include <rex/system/xnotifylistener.h>
#include <rex/system/xobject.h>
#include <rex/system/xthread.h>

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
      memory_(emulator->memory()),
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
    REXSYS_ERROR("KernelState constructed but shared_kernel_state_ already set");
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
  // Destroy app_manager while terminated thread stacks are still valid
  app_manager_.reset();

  // Stop the dispatch thread before touching the object table
  if (dispatch_thread_running_) {
    dispatch_thread_running_ = false;
    dispatch_cond_.notify_all();
    dispatch_thread_->Wait(0, 0, 0, nullptr);
  }

  // Unload all user modules: release guest heap memory and remove handles.
  for (size_t i = 0; i < user_modules_.size(); i++) {
    X_STATUS status = user_modules_[i]->Unload();
    assert_true(XSUCCEEDED(status));
    object_table_.RemoveHandle(user_modules_[i]->handle());
  }
  user_modules_.clear();
  executable_module_.reset();
  kernel_modules_.clear();

  // Unregister all notify listeners.
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
    REXSYS_ERROR("~KernelState: shared_kernel_state_ does not match this instance");
  }
}

KernelState* KernelState::shared() {
  return shared_kernel_state_;
}

uint32_t KernelState::title_id() const {
  assert_not_null(executable_module_);

  xex2_opt_execution_info* exec_info = 0;
  executable_module_->GetOptHeader(XEX_HEADER_EXECUTION_INFO, &exec_info);

  if (exec_info) {
    return exec_info->title_id;
  }

  return 0;
}

util::XdbfGameData KernelState::title_xdbf() const {
  return module_xdbf(executable_module_);
}

util::XdbfGameData KernelState::module_xdbf(object_ref<UserModule> exec_module) const {
  assert_not_null(exec_module);

  uint32_t resource_data = 0;
  uint32_t resource_size = 0;
  if (XSUCCEEDED(exec_module->GetSection(fmt::format("{:08X}", exec_module->title_id()).c_str(),
                                         &resource_data, &resource_size))) {
    util::XdbfGameData db(memory()->TranslateVirtual(resource_data), resource_size);
    return db;
  }
  return util::XdbfGameData(nullptr, resource_size);
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
    REXSYS_ERROR("AllocateTLS: null PPCContext");
    return X_TLS_OUT_OF_INDEXES;
  }

  auto globals = memory()->TranslateVirtual<KernelGuestGlobals*>(GetKernelGuestGlobals());
  auto tls_lock = &globals->tls_lock;
  auto old_irql = kernel::xboxkrnl::xeKeKfAcquireSpinLock(context, tls_lock);

  uint32_t result = X_TLS_OUT_OF_INDEXES;

  auto current_thread = XThread::GetCurrentThread();
  if (!current_thread) {
    REXSYS_ERROR("AllocateTLS: no current thread");
    kernel::xboxkrnl::xeKeKfReleaseSpinLock(context, tls_lock, old_irql);
    return X_TLS_OUT_OF_INDEXES;
  }

  auto process_ptr = memory()->TranslateVirtual<X_KPROCESS*>(
      static_cast<uint32_t>(current_thread->guest_object<X_KTHREAD>()->process));
  if (!process_ptr) {
    REXSYS_ERROR("AllocateTLS: failed to resolve current process");
    kernel::xboxkrnl::xeKeKfReleaseSpinLock(context, tls_lock, old_irql);
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

  kernel::xboxkrnl::xeKeKfReleaseSpinLock(context, tls_lock, old_irql);
  return result;
}

void KernelState::FreeTLS(PPCContext* context, uint32_t slot) {
  if (!context) {
    REXSYS_ERROR("FreeTLS: null PPCContext");
    return;
  }
  if (slot >= 256) {
    REXSYS_WARN("FreeTLS: invalid TLS slot {}", slot);
    return;
  }

  auto current_thread = XThread::GetCurrentThread();
  if (!current_thread) {
    REXSYS_ERROR("FreeTLS: no current thread");
    return;
  }

  auto current_kthread = current_thread->guest_object<X_KTHREAD>();
  if (!current_kthread) {
    REXSYS_ERROR("FreeTLS: failed to resolve current KTHREAD");
    return;
  }

  auto process_ptr =
      memory()->TranslateVirtual<X_KPROCESS*>(static_cast<uint32_t>(current_kthread->process));
  if (!process_ptr) {
    REXSYS_ERROR("FreeTLS: failed to resolve current process");
    return;
  }

  auto globals = memory()->TranslateVirtual<KernelGuestGlobals*>(GetKernelGuestGlobals());
  auto tls_lock = &globals->tls_lock;
  auto old_irql = kernel::xboxkrnl::xeKeKfAcquireSpinLock(context, tls_lock);

  uint32_t bitmap_index = slot / 32;
  uint32_t bit_mask = 1U << (31 - (slot % 32));
  uint32_t bitmap_value = static_cast<uint32_t>(process_ptr->tls_slot_bitmap[bitmap_index]);

  if (bitmap_value & bit_mask) {
    REXSYS_WARN("FreeTLS: slot {} already free", slot);
    kernel::xboxkrnl::xeKeKfReleaseSpinLock(context, tls_lock, old_irql);
    return;
  }

  const std::vector<object_ref<XThread>> threads = object_table()->GetObjectsByType<XThread>();
  uint32_t current_process_ptr = static_cast<uint32_t>(current_kthread->process);

  for (const object_ref<XThread>& thread : threads) {
    if (!thread || !thread->is_guest_thread()) {
      continue;
    }
    auto thread_kthread = thread->guest_object<X_KTHREAD>();
    if (thread_kthread && static_cast<uint32_t>(thread_kthread->process) == current_process_ptr) {
      thread->SetTLSValue(slot, 0);
    }
  }

  process_ptr->tls_slot_bitmap[bitmap_index] = bitmap_value | bit_mask;

  kernel::xboxkrnl::xeKeKfReleaseSpinLock(context, tls_lock, old_irql);
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

void KernelState::RegisterModule(XModule* module) {}

void KernelState::UnregisterModule(XModule* module) {}

bool KernelState::RegisterUserModule(object_ref<UserModule> module) {
  auto lock = global_critical_region_.Acquire();

  for (auto user_module : user_modules_) {
    if (user_module->path() == module->path()) {
      // Already loaded.
      return false;
    }
  }

  user_modules_.push_back(module);
  return true;
}

void KernelState::UnregisterUserModule(UserModule* module) {
  auto lock = global_critical_region_.Acquire();

  for (auto it = user_modules_.begin(); it != user_modules_.end(); it++) {
    if ((*it)->path() == module->path()) {
      user_modules_.erase(it);
      return;
    }
  }
}

bool KernelState::IsKernelModule(const std::string_view name) {
  if (name.empty()) {
    // Executing module isn't a kernel module.
    return false;
  }
  // NOTE: no global lock required as the kernel module list is static.
  for (auto kernel_module : kernel_modules_) {
    if (kernel_module->Matches(name)) {
      return true;
    }
  }
  return false;
}

object_ref<KernelModule> KernelState::GetKernelModule(const std::string_view name) {
  assert_true(IsKernelModule(name));

  for (auto kernel_module : kernel_modules_) {
    if (kernel_module->Matches(name)) {
      return retain_object(kernel_module.get());
    }
  }

  return nullptr;
}

object_ref<XModule> KernelState::GetModule(const std::string_view name, bool user_only) {
  if (name.empty()) {
    // NULL name = self.
    // TODO(benvanik): lookup module from caller address.
    return GetExecutableModule();
  } else if (rex::string::utf8_equal_case(name, "kernel32.dll")) {
    // Some games request this, for some reason. wtf.
    return nullptr;
  }

  // Search kernel modules under lock
  if (!user_only) {
    auto global_lock = global_critical_region_.Acquire();
    for (auto kernel_module : kernel_modules_) {
      if (kernel_module->Matches(name)) {
        return retain_object(kernel_module.get());
      }
    }
  }

  // Resolve path WITHOUT lock
  auto path(name);
  auto entry = file_system_->ResolvePath(name);
  if (entry) {
    path = entry->absolute_path();
  }

  // Search user modules under lock
  {
    auto global_lock = global_critical_region_.Acquire();
    for (auto user_module : user_modules_) {
      if (user_module->Matches(path)) {
        return retain_object(user_module.get());
      }
    }
  }
  return nullptr;
}

object_ref<XThread> KernelState::PrepareModuleLaunch(object_ref<UserModule> module) {
  if (!module->is_executable()) {
    return nullptr;
  }

  SetExecutableModule(module);
  REXSYS_INFO("KernelState: Preparing module launch...");

  // Create a thread to run in.
  // We start suspended so the caller can inspect/attach before resume.
  auto thread = object_ref<XThread>(new XThread(
      this, module->stack_size(), 0, module->entry_point(), 0, X_CREATE_SUSPENDED, true, true));

  // We know this is the 'main thread'.
  thread->set_name("Main XThread");

  X_STATUS result = thread->Create();
  if (XFAILED(result)) {
    REXSYS_ERROR("Could not create launch thread: {:08X}", result);
    return nullptr;
  }

  return thread;
}

object_ref<XThread> KernelState::LaunchModule(object_ref<UserModule> module) {
  auto thread = PrepareModuleLaunch(std::move(module));
  if (thread) {
    thread->Resume();
  }
  return thread;
}

object_ref<UserModule> KernelState::GetExecutableModule() {
  if (!executable_module_) {
    return nullptr;
  }
  return executable_module_;
}

void KernelState::SetExecutableModule(object_ref<UserModule> module) {
  if (module.get() == executable_module_.get()) {
    return;
  }
  executable_module_ = std::move(module);
  if (!executable_module_) {
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
  executable_module_->GetOptHeader(XEX_HEADER_DEFAULT_STACK_SIZE, &default_stack_size);
  if (default_stack_size) {
    default_stack_size = rex::round_up(default_stack_size, 4096u);
    if (default_stack_size < 16 * 1024) {
      default_stack_size = 16 * 1024;
    }
    pib->kernel_stack_size = default_stack_size;
  }

  // Update title process TLS info from the executable module.
  xex2_opt_tls_info* tls_header = nullptr;
  executable_module_->GetOptHeader(XEX_HEADER_TLS_INFO, &tls_header);
  if (tls_header) {
    pib->tls_data_size = tls_header->data_size;
    pib->tls_raw_data_size = tls_header->raw_data_size;
    SetProcessTLSVars(pib, tls_header->slot_count, tls_header->data_size,
                      tls_header->raw_data_address);
  }

  // Setup the kernel's XexExecutableModuleHandle field.
  auto export_entry = emulator_->function_dispatcher()->export_resolver()->GetExportByOrdinal(
      "xboxkrnl.exe", 0x0193 /* XexExecutableModuleHandle */);
  if (export_entry) {
    assert_not_zero(export_entry->variable_ptr);
    auto variable_ptr = memory_->TranslateVirtual<be<uint32_t>*>(export_entry->variable_ptr);
    *variable_ptr = executable_module_->hmodule_ptr();
  }

  // Setup the kernel's ExLoadedImageName field
  export_entry = emulator_->function_dispatcher()->export_resolver()->GetExportByOrdinal(
      "xboxkrnl.exe", 0x01AF /* ExLoadedImageName */);
  if (export_entry) {
    char* variable_ptr = memory_->TranslateVirtual<char*>(export_entry->variable_ptr);
    rex::string::util_copy_truncating(variable_ptr, executable_module_->path(),
                                      kExLoadedImageNameSize);
  }

  // Spin up deferred dispatch worker.
  // TODO(benvanik): move someplace more appropriate (out of ctor, but around
  // here).
  if (!dispatch_thread_running_) {
    dispatch_thread_running_ = true;
    dispatch_thread_ = object_ref<XHostThread>(new XHostThread(this, 128 * 1024, 0, [this]() {
      auto global_lock = global_critical_region_.AcquireDeferred();
      while (dispatch_thread_running_) {
        global_lock.lock();
        if (dispatch_queue_.empty()) {
          dispatch_cond_.wait(global_lock);
          if (!dispatch_thread_running_) {
            global_lock.unlock();
            break;
          }
        }
        auto fn = std::move(dispatch_queue_.front());
        dispatch_queue_.pop_front();
        REXSYS_NOISY_DEBUG("Dispatch thread processing queued item ({} remaining)",
                           dispatch_queue_.size());
        global_lock.unlock();

        fn();
        REXSYS_NOISY_DEBUG("Dispatch thread completed item");
      }
      return 0;
    }));
    dispatch_thread_->set_name("Kernel Dispatch");
    dispatch_thread_->Create();
  }
}

void KernelState::LoadKernelModule(object_ref<KernelModule> kernel_module) {
  auto global_lock = global_critical_region_.Acquire();
  kernel_modules_.push_back(std::move(kernel_module));
}

object_ref<UserModule> KernelState::LoadUserModule(const std::string_view raw_name,
                                                   bool call_entry) {
  // Some games try to load relative to launch module, others specify full path.
  auto name = rex::string::utf8_find_name_from_guest_path(raw_name);
  std::string path(raw_name);
  if (name == raw_name) {
    assert_not_null(executable_module_);
    path = rex::string::utf8_join_guest_paths(
        rex::string::utf8_find_base_guest_path(executable_module_->path()), name);
  }

  object_ref<UserModule> module;
  {
    auto global_lock = global_critical_region_.Acquire();

    // See if we've already loaded it
    for (auto& existing_module : user_modules_) {
      if (existing_module->path() == path) {
        return existing_module;
      }
    }

    global_lock.unlock();

    // Module wasn't loaded, so load it.
    module = object_ref<UserModule>(new UserModule(this));
    X_STATUS status = module->LoadFromFile(path);
    if (XFAILED(status)) {
      object_table()->ReleaseHandle(module->handle());
      return nullptr;
    }

    global_lock.lock();

    // Putting into the listing automatically retains.
    user_modules_.push_back(module);
  }

  module->Dump();

  if (module->is_dll_module() && module->entry_point() && call_entry) {
    // TODO(tomc): add support for this. sort of coupled with the rest of the guest module loading.
    //              impl of GetProcAddressByOrdinal is critical to the impl of the dll loading.

    REXSYS_WARN("LoadUserModule: DllMain(DLL_PROCESS_ATTACH) not implemented");
  }

  return module;
}

void KernelState::UnloadUserModule(const object_ref<UserModule>& module, bool call_entry) {
  auto global_lock = global_critical_region_.Acquire();

  if (module->is_dll_module() && module->entry_point() && call_entry) {
    // TODO(tomc): add support for this. see comment in LoadUserModule
    REXSYS_WARN("UnloadUserModule: DllMain(DLL_PROCESS_DETACH) not implemented");
  }

  auto iter = std::find_if(user_modules_.begin(), user_modules_.end(),
                           [&module](const auto& e) { return e->path() == module->path(); });
  assert_true(iter != user_modules_.end());  // Unloading an unregistered module
                                             // is probably really bad
  user_modules_.erase(iter);

  // Ensure this module was not somehow registered twice
  assert_true(std::find_if(user_modules_.begin(), user_modules_.end(), [&module](const auto& e) {
                return e->path() == module->path();
              }) == user_modules_.end());

  object_table()->ReleaseHandle(module->handle());
}

void KernelState::TerminateTitle() {
  REXSYS_DEBUG("KernelState::TerminateTitle");
  auto global_lock = global_critical_region_.Acquire();

  // Suspend all running guest threads so they stop touching shared state.
  std::vector<XThread*> suspended_threads;
  for (auto it = threads_by_id_.begin(); it != threads_by_id_.end(); ++it) {
    if (!XThread::IsInThread(it->second) && it->second->is_guest_thread() &&
        it->second->is_running()) {
      it->second->thread()->Suspend();
      suspended_threads.push_back(it->second);
    }
  }

  // Terminate each suspended thread. Must drop the lock since Terminate waits.
  global_lock.unlock();
  for (auto* thread : suspended_threads) {
    thread->Terminate(0);
  }
  global_lock.lock();

  // Remove all guest threads from the map.
  for (auto it = threads_by_id_.begin(); it != threads_by_id_.end();) {
    if (!XThread::IsInThread(it->second) && it->second->is_guest_thread()) {
      it = threads_by_id_.erase(it);
    } else {
      ++it;
    }
  }

  // If called from a guest thread, self-terminate last.
  if (XThread::IsInThread()) {
    threads_by_id_.erase(XThread::GetCurrentThread()->thread_id());

    // Now commit suicide (using Terminate, because we can't call into guest
    // code anymore).
    global_lock.unlock();
    XThread::GetCurrentThread()->Terminate(0);
  }
}

void KernelState::RegisterThread(XThread* thread) {
  auto global_lock = global_critical_region_.Acquire();
  threads_by_id_[thread->thread_id()] = thread;

  // Thread count is now managed via thread-process linking in
  // XThread::InitializeGuestObject and XThread::Exit.
}

void KernelState::UnregisterThread(XThread* thread) {
  auto global_lock = global_critical_region_.Acquire();
  auto it = threads_by_id_.find(thread->thread_id());
  if (it != threads_by_id_.end()) {
    threads_by_id_.erase(it);
  }
}

void KernelState::OnThreadExecute(XThread* thread) {
  auto global_lock = global_critical_region_.Acquire();

  // Must be called on executing thread.
  assert_true(XThread::GetCurrentThread() == thread);

  // TODO(tomc): Do we need this?
  //             Xenia would iterate user_modules_ and call function_dispatcher()->Execute() for
  //             each Note that this would require reimplementation of guest thread management
  (void)thread;
}

void KernelState::OnThreadExit(XThread* thread) {
  auto global_lock = global_critical_region_.Acquire();

  // Must be called on executing thread.
  assert_true(XThread::GetCurrentThread() == thread);

  // TODO(tomc): Do we need this? Same idea as OnThreadExecute
  (void)thread;
}

object_ref<XThread> KernelState::GetThreadByID(uint32_t thread_id) {
  auto global_lock = global_critical_region_.Acquire();
  XThread* thread = nullptr;
  auto it = threads_by_id_.find(thread_id);
  if (it != threads_by_id_.end()) {
    thread = it->second;
  }
  return retain_object(thread);
}

void KernelState::RegisterNotifyListener(XNotifyListener* listener) {
  auto global_lock = global_critical_region_.Acquire();
  notify_listeners_.push_back(retain_object(listener));

  // Games seem to expect a few notifications on startup, only for the first
  // listener.
  // https://cs.rin.ru/forum/viewtopic.php?f=38&t=60668&hilit=resident+evil+5&start=375
  if (!has_notified_startup_ && listener->mask() & 0x00000001) {
    has_notified_startup_ = true;
    // XN_SYS_UI (on, off)
    listener->EnqueueNotification(0x00000009, 1);
    listener->EnqueueNotification(0x00000009, 0);
    // XN_SYS_SIGNINCHANGED x2
    listener->EnqueueNotification(0x0000000A, 1);
    listener->EnqueueNotification(0x0000000A, 1);
    // XN_SYS_INPUTDEVICESCHANGED x2
    listener->EnqueueNotification(0x00000012, 0);
    listener->EnqueueNotification(0x00000012, 0);
    // XN_SYS_INPUTDEVICECONFIGCHANGED x2
    listener->EnqueueNotification(0x00000013, 0);
    listener->EnqueueNotification(0x00000013, 0);
  }
}

void KernelState::UnregisterNotifyListener(XNotifyListener* listener) {
  auto global_lock = global_critical_region_.Acquire();
  for (auto it = notify_listeners_.begin(); it != notify_listeners_.end(); ++it) {
    if ((*it).get() == listener) {
      notify_listeners_.erase(it);
      break;
    }
  }
}

void KernelState::BroadcastNotification(XNotificationID id, uint32_t data) {
  std::vector<object_ref<XNotifyListener>> snapshot;
  {
    auto global_lock = global_critical_region_.Acquire();
    REXSYS_DEBUG("BroadcastNotification(id={:#x}, data={}) to {} listeners",
                 static_cast<uint32_t>(id), data, notify_listeners_.size());
    snapshot = notify_listeners_;
  }
  for (const auto& notify_listener : snapshot) {
    notify_listener->EnqueueNotification(id, data);
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
    auto ev = object_table()->LookupObject<XEvent>(event_handle);
    assert_not_null(ev);
    if (ev) {
      ev->Set(0, false);
    }
  }
  if (XOverlappedGetCompletionRoutine(ptr)) {
    X_HANDLE thread_handle = XOverlappedGetContext(ptr);
    auto thread = object_table()->LookupObject<XThread>(thread_handle);
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
  XOverlappedSetContext(ptr, XThread::GetCurrentThreadHandle());
  CompleteOverlappedEx(overlapped_ptr, result, extended_error, length);
}

void KernelState::CompleteOverlappedDeferred(std::move_only_function<void()> completion_callback,
                                             uint32_t overlapped_ptr, X_RESULT result,
                                             std::move_only_function<void()> pre_callback,
                                             std::move_only_function<void()> post_callback) {
  CompleteOverlappedDeferredEx(std::move(completion_callback), overlapped_ptr, result, result, 0,
                               std::move(pre_callback), std::move(post_callback));
}

void KernelState::CompleteOverlappedDeferredEx(std::move_only_function<void()> completion_callback,
                                               uint32_t overlapped_ptr, X_RESULT result,
                                               uint32_t extended_error, uint32_t length,
                                               std::move_only_function<void()> pre_callback,
                                               std::move_only_function<void()> post_callback) {
  CompleteOverlappedDeferredEx(
      [completion_callback = std::move(completion_callback), result, extended_error, length](
          uint32_t& cb_extended_error, uint32_t& cb_length) mutable -> X_RESULT {
        completion_callback();
        cb_extended_error = extended_error;
        cb_length = length;
        return result;
      },
      overlapped_ptr, std::move(pre_callback), std::move(post_callback));
}

void KernelState::CompleteOverlappedDeferred(
    std::move_only_function<X_RESULT()> completion_callback, uint32_t overlapped_ptr,
    std::move_only_function<void()> pre_callback, std::move_only_function<void()> post_callback) {
  CompleteOverlappedDeferredEx(
      [completion_callback = std::move(completion_callback)](uint32_t& extended_error,
                                                             uint32_t& length) mutable -> X_RESULT {
        auto result = completion_callback();
        extended_error = static_cast<uint32_t>(result);
        length = 0;
        return result;
      },
      overlapped_ptr, std::move(pre_callback), std::move(post_callback));
}

void KernelState::CompleteOverlappedDeferredEx(
    std::move_only_function<X_RESULT(uint32_t&, uint32_t&)> completion_callback,
    uint32_t overlapped_ptr, std::move_only_function<void()> pre_callback,
    std::move_only_function<void()> post_callback) {
  REXSYS_DEBUG("CompleteOverlappedDeferredEx: queuing for overlapped {:08X}", overlapped_ptr);
  auto ptr = memory()->TranslateVirtual(overlapped_ptr);
  XOverlappedSetResult(ptr, X_ERROR_IO_PENDING);
  XOverlappedSetContext(ptr, XThread::GetCurrentThreadHandle());
  auto global_lock = global_critical_region_.Acquire();
  dispatch_queue_.push_back(
      [this, overlapped_ptr, completion_callback = std::move(completion_callback),
       pre_callback = std::move(pre_callback), post_callback = std::move(post_callback)]() mutable {
        REXSYS_DEBUG("Deferred overlapped {:08X}: running pre_callback", overlapped_ptr);
        if (pre_callback) {
          pre_callback();
        }
        REXSYS_DEBUG("Deferred overlapped {:08X}: sleeping {}ms", overlapped_ptr,
                     kDeferredOverlappedDelayMillis);
        rex::thread::Sleep(std::chrono::milliseconds(kDeferredOverlappedDelayMillis));
        uint32_t extended_error, length;
        REXSYS_DEBUG("Deferred overlapped {:08X}: running completion", overlapped_ptr);
        auto result = completion_callback(extended_error, length);
        REXSYS_DEBUG("Deferred overlapped {:08X}: completing with result {:08X}", overlapped_ptr,
                     result);
        CompleteOverlappedEx(overlapped_ptr, result, extended_error, length);
        if (post_callback) {
          REXSYS_DEBUG("Deferred overlapped {:08X}: running post_callback", overlapped_ptr);
          post_callback();
        }
      });
  dispatch_cond_.notify_all();
}

DPCImpersonationScope KernelState::BeginDPCImpersonation() {
  auto* thread = XThread::GetCurrentThread();
  auto* ctx = thread->thread_state()->context();
  auto pcr = memory_->TranslateVirtual<X_KPCR*>(static_cast<uint32_t>(ctx->r13.u64));
  DPCImpersonationScope scope;
  scope.previous_irql_ = pcr->current_irql;
  pcr->current_irql = IRQL_DISPATCH;
  pcr->prcb_data.dpc_active = 1;
  return scope;
}

void KernelState::EndDPCImpersonation(const DPCImpersonationScope& scope) {
  auto* thread = XThread::GetCurrentThread();
  auto* ctx = thread->thread_state()->context();
  auto pcr = memory_->TranslateVirtual<X_KPCR*>(static_cast<uint32_t>(ctx->r13.u64));
  pcr->prcb_data.dpc_active = 0;
  pcr->current_irql = scope.previous_irql_;
}

bool KernelState::Save(stream::ByteStream* stream) {
  REXSYS_DEBUG("Serializing the kernel...");
  stream->Write(kKernelSaveSignature);

  // Save the object table
  object_table_.Save(stream);

  // Legacy save-state field (global TLS bitmap) no longer used.
  stream->Write(uint32_t(0));

  // We save XThreads absolutely first, as they will execute code upon save
  // (which could modify the kernel state)
  auto threads = object_table_.GetObjectsByType<XThread>();
  uint32_t* num_threads_ptr = reinterpret_cast<uint32_t*>(stream->data() + stream->offset());
  stream->Write(static_cast<uint32_t>(threads.size()));

  size_t num_threads = threads.size();
  REXSYS_DEBUG("Serializing {} threads...", threads.size());
  for (auto thread : threads) {
    if (!thread->is_guest_thread()) {
      // Don't save host threads. They can be reconstructed on startup.
      num_threads--;
      continue;
    }

    if (!thread->Save(stream)) {
      REXSYS_DEBUG("Failed to save thread \"{}\"", thread->name());
      num_threads--;
    }
  }

  *num_threads_ptr = static_cast<uint32_t>(num_threads);

  // Save all other objects
  auto objects = object_table_.GetAllObjects();
  uint32_t* num_objects_ptr = reinterpret_cast<uint32_t*>(stream->data() + stream->offset());
  stream->Write(static_cast<uint32_t>(objects.size()));

  size_t num_objects = objects.size();
  REXSYS_DEBUG("Serializing {} objects...", num_objects);
  for (auto object : objects) {
    auto prev_offset = stream->offset();

    if (object->is_host_object() || object->type() == XObject::Type::Thread) {
      // Don't save host objects or save XThreads again
      num_objects--;
      continue;
    }

    stream->Write<uint32_t>(static_cast<uint32_t>(object->type()));
    if (!object->Save(stream)) {
      REXSYS_DEBUG("Did not save object of type {}", object->type());
      assert_always();

      // Revert backwards and overwrite if a save failed.
      stream->set_offset(prev_offset);
      num_objects--;
    }
  }

  *num_objects_ptr = static_cast<uint32_t>(num_objects);
  return true;
}

bool KernelState::Restore(stream::ByteStream* stream) {
  // Check the magic value.
  if (stream->Read<uint32_t>() != kKernelSaveSignature) {
    return false;
  }

  // Restore the object table
  object_table_.Restore(stream);

  // Global TLS bitmap field is kept for old save-state compatibility.
  auto num_bitmap_entries = stream->Read<uint32_t>();
  for (uint32_t i = 0; i < num_bitmap_entries; i++) {
    stream->Read<uint64_t>();
  }

  uint32_t num_threads = stream->Read<uint32_t>();
  REXSYS_DEBUG("Loading {} threads...", num_threads);
  for (uint32_t i = 0; i < num_threads; i++) {
    auto thread = XObject::Restore(this, XObject::Type::Thread, stream);
    if (!thread) {
      // Can't continue the restore or we risk misalignment.
      assert_always();
      return false;
    }
  }

  uint32_t num_objects = stream->Read<uint32_t>();
  REXSYS_DEBUG("Loading {} objects...", num_objects);
  for (uint32_t i = 0; i < num_objects; i++) {
    uint32_t type = stream->Read<uint32_t>();

    auto obj = XObject::Restore(this, XObject::Type(type), stream);
    if (!obj) {
      // Can't continue the restore or we risk misalignment.
      assert_always();
      return false;
    }
  }

  return true;
}

FiberInfo* KernelState::LookupFiber(uint32_t guest_addr) {
  auto lock = global_critical_region_.Acquire();
  auto it = fiber_map_.find(guest_addr);
  return it != fiber_map_.end() ? &it->second : nullptr;
}

void KernelState::RegisterFiber(uint32_t guest_addr, const FiberInfo& info) {
  auto lock = global_critical_region_.Acquire();
  fiber_map_[guest_addr] = info;
}

void KernelState::UnregisterFiber(uint32_t guest_addr) {
  auto lock = global_critical_region_.Acquire();
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
