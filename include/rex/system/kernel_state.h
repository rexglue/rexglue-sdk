#pragma once
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

#include <atomic>
#include <condition_variable>
#include <filesystem>
#include <functional>
#include <list>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <rex/filesystem.h>
#include <rex/logging.h>
#include <rex/system/achievement_manager.h>
#include <rex/system/module_registry.h>
#include <rex/system/thread_registry.h>
#include <rex/system/thread_state.h>
#include <rex/platform/fiber.h>
#include <rex/system/util/native_list.h>
#include <rex/system/util/object_table.h>
#include <rex/system/util/xdbf_utils.h>
#include <rex/system/xam/app_manager.h>
#include <rex/system/xam/content_manager.h>
#include <rex/system/xam/user_profile.h>
#include <rex/platform/dynlib.h>
#include <rex/system/function_dispatcher.h>
#include <rex/system/xcontent.h>

#include <rex/runtime/config.h>
#include <rex/system/xmemory.h>
#include <rex/system/xobject.h>
#include <rex/system/xtypes.h>
#include <rex/platform/lock_order.h>
#include <rex/types.h>

//=============================================================================
// Kernel State Access Macros
//=============================================================================
// Convenience macros for accessing the current thread's kernel state and
// subsystems from kernel export (_entry) functions. These use the ThreadState
// bound to the current thread via ThreadState::Bind().

#define REX_KERNEL_STATE() (::rex::runtime::current_kernel_state())
#define REX_KERNEL_MEMORY() (REX_KERNEL_STATE()->memory())
#define REX_KERNEL_OBJECTS() (REX_KERNEL_STATE()->object_table())
#define REX_KERNEL_FS() (REX_KERNEL_STATE()->file_system())

namespace rex {
class Runtime;
namespace runtime {
class FunctionDispatcher;
}  // namespace runtime
}  // namespace rex

struct PPCContext;

namespace rex::system {

class Dispatcher;
class ThreadRecord;

// (?), used by KeGetCurrentProcessType
constexpr uint32_t X_PROCTYPE_IDLE = 0;
constexpr uint32_t X_PROCTYPE_USER = 1;
constexpr uint32_t X_PROCTYPE_SYSTEM = 2;

struct X_KPROCESS {
  X_KSPINLOCK thread_list_spinlock;           // 0x00
  X_LIST_ENTRY thread_list;                   // 0x04
  rex::be<int32_t> quantum;                   // 0x0C
  rex::be<uint32_t> clrdataa_masked_ptr;      // 0x10
  rex::be<uint32_t> thread_count;             // 0x14
  uint8_t unk_18;                             // 0x18
  uint8_t unk_19;                             // 0x19
  uint8_t unk_1A;                             // 0x1A
  uint8_t unk_1B;                             // 0x1B
  rex::be<uint32_t> kernel_stack_size;        // 0x1C
  rex::be<uint32_t> tls_static_data_address;  // 0x20
  rex::be<uint32_t> tls_data_size;            // 0x24
  rex::be<uint32_t> tls_raw_data_size;        // 0x28
  rex::be<uint16_t> tls_slot_size;            // 0x2C
  uint8_t is_terminating;                     // 0x2E
  uint8_t process_type;                       // 0x2F
  rex::be<uint32_t> tls_slot_bitmap[8];       // 0x30
  rex::be<uint32_t> unk_50;                   // 0x50
  X_LIST_ENTRY unk_54;                        // 0x54
  rex::be<uint32_t> unk_5C;                   // 0x5C
};
static_assert_size(X_KPROCESS, 0x60);

// Keep old name as alias for code that still references it
using ProcessInfoBlock = X_KPROCESS;

struct X_UNKNOWN_TYPE_REFED {
  rex::be<uint32_t> field0;
  rex::be<uint32_t> field4;
  rex::be<uint32_t> points_to_self;
  rex::be<uint32_t> points_to_prior;
};
static_assert_size(X_UNKNOWN_TYPE_REFED, 16);

struct X_KEVENT;  // forward decl, defined in xevent.h

struct KernelGuestGlobals {
  X_OBJECT_TYPE ExThreadObjectType;
  X_OBJECT_TYPE ExEventObjectType;
  X_OBJECT_TYPE ExMutantObjectType;
  X_OBJECT_TYPE ExSemaphoreObjectType;
  X_OBJECT_TYPE ExTimerObjectType;
  X_OBJECT_TYPE IoCompletionObjectType;
  X_OBJECT_TYPE IoDeviceObjectType;
  X_OBJECT_TYPE IoFileObjectType;
  X_OBJECT_TYPE ObDirectoryObjectType;
  X_OBJECT_TYPE ObSymbolicLinkObjectType;
  X_UNKNOWN_TYPE_REFED OddObj;
  X_KPROCESS idle_process;
  X_KPROCESS title_process;
  X_KPROCESS system_process;
  X_KSPINLOCK dispatcher_lock;
  X_KSPINLOCK ob_lock;
  X_KSPINLOCK tls_lock;  // protects per-process TLS slot allocation bitmap
  // UsbdBootEnumerationDoneEvent uses X_DISPATCH_HEADER layout (0x10 bytes)
  uint8_t UsbdBootEnumerationDoneEvent[0x10];
};

struct DPCImpersonationScope {
  uint8_t previous_irql_;
};

struct TerminateNotification {
  uint32_t guest_routine;
  uint32_t priority;
};

/// Host-side metadata for a fiber managed by the shim fiber routines.
struct FiberInfo {
  rex::platform::Fiber* host_fiber;  ///< Host OS fiber handle
  uint32_t uid;                      ///< Unique identifier
  uint32_t guest_context_addr;       ///< Guest fiber context buffer address
  uint32_t guest_stack_base;         ///< Top of guest kernel stack (0 for thread fibers)
  uint32_t guest_stack_bottom;       ///< Bottom of guest kernel stack (0 for thread fibers)
  bool is_thread_fiber;              ///< true = ConvertThreadToFiber (don't free guest stack)
};

class KernelState {
 public:
  explicit KernelState(Runtime* emulator);
  ~KernelState();

  /** @return The runtime.kernel slice of the configuration. */
  const runtime::KernelConfig& config() const { return config_; }

  static KernelState* shared();

  Runtime* emulator() const { return emulator_; }
  memory::Memory* memory() const { return memory_; }
  runtime::FunctionDispatcher* function_dispatcher() const { return function_dispatcher_; }
  rex::filesystem::IFileSystem* file_system() const { return file_system_; }

  uint32_t title_id() const;
  util::XdbfGameData title_xdbf() const;
  util::XdbfGameData module_xdbf(const ModuleObject& exec_module) const;

  xam::AppManager* app_manager() const { return app_manager_.get(); }
  xam::ContentManager* content_manager() const { return content_manager_.get(); }
  xam::UserProfile* user_profile() const { return user_profile_.get(); }

  util::ObjectTable* object_table() { return &object_table_; }

  /**
   * Returns the lock that serializes the creation of an object behind a guest dispatch header.
   *
   * @return The identity mutex.
   */
  rex::platform::RankedMutex& identity_mutex() { return identity_mutex_; }

  /**
   * Returns the lock that serializes module load count changes against module lookups.
   *
   * @return The load count mutex.
   */
  rex::platform::RankedMutex& load_count_mutex() { return load_count_mutex_; }

  /**
   * Returns the lock that guards the DPC list.
   *
   * @return The DPC mutex.
   */
  rex::platform::RankedMutex& dpc_mutex() { return dpc_mutex_; }

  uint32_t process_type() const;
  void set_process_type(uint32_t value);
  uint32_t process_info_block_address() const {
    return kernel_guest_globals_
               ? kernel_guest_globals_ + offsetof(KernelGuestGlobals, title_process)
               : 0;
  }

  uint32_t GetKernelGuestGlobals() const { return kernel_guest_globals_; }

  uint32_t GetTitleProcess() const {
    return kernel_guest_globals_ + offsetof(KernelGuestGlobals, title_process);
  }
  uint32_t GetSystemProcess() const {
    return kernel_guest_globals_ + offsetof(KernelGuestGlobals, system_process);
  }
  uint32_t GetIdleProcess() const {
    return kernel_guest_globals_ + offsetof(KernelGuestGlobals, idle_process);
  }

  DPCImpersonationScope BeginDPCImpersonation();
  void EndDPCImpersonation(const DPCImpersonationScope& scope);

  uint32_t AllocateTLS(PPCContext* context);
  void FreeTLS(PPCContext* context, uint32_t slot);

  void RegisterTitleTerminateNotification(uint32_t routine, uint32_t priority);
  void RemoveTitleTerminateNotification(uint32_t routine);

  /**
   * Returns the module registry.
   *
   * @return The registry.
   */
  ModuleRegistry* modules() { return &modules_; }

  /**
   * Finds a loaded module.
   *
   * @param name A module name, a file name or a path, empty for the executable.
   * @param user_only True to skip the kernel modules.
   * @return The handle the registry owns, which the caller must not close, or 0.
   */
  X_HANDLE GetModule(const std::string_view name, bool user_only = false);

  /**
   * Prepares a module launch and resumes the main thread.
   *
   * @param module The handle of the executable module.
   * @return The main thread handle with a count of 1 that the caller closes, 0 when it failed.
   */
  X_HANDLE LaunchModule(X_HANDLE module);

  /**
   * Makes a module the executable and creates its suspended main thread.
   *
   * @param module The handle of the executable module.
   * @return The main thread handle with a count of 1 that the caller closes, 0 when it failed.
   */
  X_HANDLE PrepareModuleLaunch(X_HANDLE module);

  /**
   * Returns the executable module.
   *
   * @return The handle the registry owns, which the caller must not close, or 0.
   */
  X_HANDLE GetExecutableModule();

  /**
   * Sets the executable module and updates the title process from its XEX header.
   *
   * @param module Any handle of the module, or 0 to clear it.
   */
  void SetExecutableModule(X_HANDLE module);

  /**
   * Loads a user module, or finds it when it is already loaded.
   *
   * @param name A path, or a file name that is relative to the executable.
   * @param call_entry True to call the entry point of a DLL module for the process attach.
   * @return The handle the registry owns, which the caller must not close, or 0 when it failed.
   */
  X_HANDLE LoadUserModule(const std::string_view name, bool call_entry = true);

  /**
   * Unloads a user module and closes its handles.
   *
   * @param module Any handle of the module.
   * @param call_entry True to call the entry point of a DLL module for the process detach.
   */
  void UnloadUserModule(X_HANDLE module, bool call_entry = true);

  // Recompiled module registry (populated by generated RegisterRecompiledModules)
  struct RecompiledModuleInfo {
    std::string pe_name;
    std::string guest_path;
    std::string shared_lib_name;
  };

  void RegisterRecompiledModule(const char* pe_name, const char* guest_path,
                                const char* shared_lib_name);
  std::optional<RecompiledModuleInfo> FindRecompiledModule(std::string_view guest_path);

  /**
   * Finds a kernel module.
   *
   * @param name A module name, a file name or a path.
   * @return The handle the registry owns, which the caller must not close, or 0.
   */
  X_HANDLE GetKernelModule(const std::string_view name);

  /**
   * Registers a kernel module.
   *
   * @param created A handle of the module that a kernel module creator returned.
   * @return The handle the registry owns, which the caller must not close, or 0.
   */
  X_HANDLE LoadKernelModule(X_HANDLE created);

  // Terminates a title: Unloads all modules, and kills all guest threads.
  // This DOES NOT RETURN if called from a guest thread!
  void TerminateTitle();

  /**
   * Reports a title exit to the runtime, then terminates the title from the calling thread.
   *
   * @param reason Why the title exited.
   */
  void ExitTitle(runtime::TitleExitReason reason);
  bool is_terminating_title() const { return terminating_title_.load(std::memory_order_acquire); }

  /**
   * Returns the thread registry.
   *
   * @return The registry.
   */
  ThreadRegistry* threads() { return &threads_; }

  FiberInfo* LookupFiber(uint32_t guest_addr);
  void RegisterFiber(uint32_t guest_addr, const FiberInfo& info);
  void UnregisterFiber(uint32_t guest_addr);

  /// Returns a fiber name for profiling
  const char* GetOrCreateFiberName(uint32_t guest_addr, const char* thread_name);

  /**
   * Registers a notify listener. The kernel keeps its own duplicate of the handle.
   *
   * @param listener_handle The handle of a NotifyListenerObject.
   */
  void RegisterNotifyListener(X_HANDLE listener_handle);

  void BroadcastNotification(XNotificationID id, uint32_t data);

  util::NativeList* dpc_list() { return &dpc_list_; }

  void CompleteOverlapped(uint32_t overlapped_ptr, X_RESULT result);
  void CompleteOverlappedEx(uint32_t overlapped_ptr, X_RESULT result, uint32_t extended_error,
                            uint32_t length);

  void CompleteOverlappedImmediate(uint32_t overlapped_ptr, X_RESULT result);
  void CompleteOverlappedImmediateEx(uint32_t overlapped_ptr, X_RESULT result,
                                     uint32_t extended_error, uint32_t length);

  void CompleteOverlappedDeferred(std::function<void()> completion_callback,
                                  uint32_t overlapped_ptr, X_RESULT result,
                                  std::function<void()> pre_callback = nullptr,
                                  std::function<void()> post_callback = nullptr);
  void CompleteOverlappedDeferredEx(std::function<void()> completion_callback,
                                    uint32_t overlapped_ptr, X_RESULT result,
                                    uint32_t extended_error, uint32_t length,
                                    std::function<void()> pre_callback = nullptr,
                                    std::function<void()> post_callback = nullptr);

  void CompleteOverlappedDeferred(std::function<X_RESULT()> completion_callback,
                                  uint32_t overlapped_ptr,
                                  std::function<void()> pre_callback = nullptr,
                                  std::function<void()> post_callback = nullptr);
  void CompleteOverlappedDeferredEx(
      std::function<X_RESULT(uint32_t&, uint32_t&)> completion_callback, uint32_t overlapped_ptr,
      std::function<void()> pre_callback = nullptr, std::function<void()> post_callback = nullptr);

  void SetLoadedAchievements(std::vector<AchievementInfo> achievements);
  void UnlockAchievement(uint32_t id);
  bool IsAchievementUnlocked(uint32_t id) const;
  // Returns the unlock FILETIME (100-ns intervals since 1601-01-01), or 0 if locked.
  uint64_t GetAchievementUnlockTime(uint32_t id) const;
  std::vector<AchievementInfo> loaded_achievements() const;
  AchievementManager& achievements() { return achievement_manager_; }
  const AchievementManager& achievements() const { return achievement_manager_; }

  using AchievementUnlockCallback = std::function<void(const AchievementInfo&)>;
  AchievementListenerHandle RegisterAchievementUnlockCallback(AchievementUnlockCallback cb);

 private:
  void SignalAllWaitableObjects();
  void InitializeProcess(X_KPROCESS* process, uint32_t process_type, uint8_t unk_18, uint8_t unk_19,
                         uint8_t unk_1A);
  void SetProcessTLSVars(X_KPROCESS* process, uint32_t num_slots, uint32_t tls_data_size,
                         uint32_t tls_raw_data_address);
  void LoadAchievementsData();

  Runtime* emulator_;

  const runtime::KernelConfig& config_;
  memory::Memory* memory_;
  runtime::FunctionDispatcher* function_dispatcher_;
  rex::filesystem::IFileSystem* file_system_;

  std::unique_ptr<xam::AppManager> app_manager_;
  std::unique_ptr<xam::ContentManager> content_manager_;
  std::unique_ptr<xam::UserProfile> user_profile_;

  ThreadRegistry threads_{&object_table_};
  mutable util::ObjectTable object_table_;
  ModuleRegistry modules_{&object_table_};
  rex::platform::RankedMutex identity_mutex_{rex::platform::LockRank::Identity};
  rex::platform::RankedMutex load_count_mutex_{rex::platform::LockRank::Identity};
  rex::platform::RankedMutex notify_mutex_{rex::platform::LockRank::Leaf};
  rex::platform::RankedMutex dpc_mutex_{rex::platform::LockRank::Leaf};
  rex::platform::RankedMutex dispatch_mutex_{rex::platform::LockRank::Leaf};
  rex::platform::RankedMutex fiber_mutex_{rex::platform::LockRank::Leaf};
  rex::platform::RankedMutex recompiled_mutex_{rex::platform::LockRank::Leaf};
  std::vector<X_HANDLE> notify_listeners_;
  bool has_notified_startup_ = false;

  std::unordered_map<uint32_t, FiberInfo> fiber_map_;

  // Fiber name pool for profiling.
  // Never erased, Tracy references pointers async.
  std::mutex fiber_name_pool_mutex_;
  std::unordered_map<uint32_t, std::unique_ptr<char[]>> fiber_name_pool_;

  uint32_t process_type_ = X_PROCTYPE_USER;
  std::vector<TerminateNotification> terminate_notifications_;
  std::vector<RecompiledModuleInfo> recompiled_modules_;
  std::unordered_map<std::string, rex::platform::DynamicLibrary> module_libraries_;
  // FreeLibrary deferred to teardown so guest threads still in unloaded code
  // don't return into freed pages. Drained at the end of ~KernelState.
  std::vector<rex::platform::DynamicLibrary> deferred_unload_libraries_;

  uint32_t kernel_guest_globals_ = 0;

  AchievementManager achievement_manager_;

  std::atomic<bool> dispatch_thread_running_;
  std::atomic<bool> terminating_title_{false};
  X_HANDLE dispatch_thread_ = 0;
  util::NativeList dpc_list_;
  std::condition_variable_any dispatch_cond_;
  std::list<std::function<void()>> dispatch_queue_;
};

// Global kernel state accessor (defined in kernel_state.cpp)
KernelState* kernel_state();

// Convenience accessor for kernel memory
inline memory::Memory* kernel_memory() {
  return kernel_state()->memory();
}

template <typename T>
Pinned<T> ResolveGuest(KernelState* kernel_state, void* native_ptr, i32 as_type) {
  return kernel_state->object_table()->Get<T>(
      ResolveGuestObject(kernel_state, native_ptr, as_type));
}

template <typename T, typename... Args>
X_STATUS CreateWithBlock(KernelState* kernel_state, X_HANDLE* out_handle, u32 attributes_ptr,
                         u32 block_size, Args&&... args) {
  GuestBlock block;
  if (block_size) {
    block = AllocateGuestBlock(kernel_state->memory(), block_size);
    if (!block.address) {
      return X_STATUS_NO_MEMORY;
    }
  }
  const std::string name = NameFromAttributes(kernel_state->memory(), attributes_ptr);
  X_HANDLE handle = 0;
  const X_STATUS status = kernel_state->object_table()->Emplace<T>(&handle, AddOptions{name, block},
                                                                   std::forward<Args>(args)...);
  if (XFAILED(status)) {
    if (block.address) {
      FreeGuestBlock(kernel_state->memory(), block.address);
    }
    return status;
  }
  if (block.address) {
    StashHandle(kernel_state->memory()->TranslateVirtual<X_DISPATCH_HEADER*>(block.address),
                handle);
  }
  *out_handle = handle;
  return X_STATUS_SUCCESS;
}

}  // namespace rex::system
