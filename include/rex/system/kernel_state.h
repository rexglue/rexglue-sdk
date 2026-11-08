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

#include <rex/filesystem/vfs.h>
#include <rex/logging.h>
#include <rex/system/achievement_manager.h>
#include <rex/system/thread_state.h>
#include <rex/thread/fiber.h>
#include <rex/system/util/native_list.h>
#include <rex/system/util/object_table.h>
#include <rex/system/util/xdbf_utils.h>
#include <rex/system/xam/app_manager.h>
#include <rex/system/xam/content_manager.h>
#include <rex/system/xam/user_profile.h>
#include <rex/platform/dynlib.h>
#include <rex/system/function_dispatcher.h>
#include <rex/system/xcontent.h>
#include <rex/system/xmemory.h>
#include <rex/system/xobject.h>
#include <rex/system/xtypes.h>
#include <rex/thread/mutex.h>
#include <rex/types.h>

//=============================================================================
// Kernel Import Trace Helpers
//=============================================================================
// Use these macros for consistent logging of kernel import function calls.
// Example usage:
//   REXKRNL_IMPORT_TRACE("NtCreateFile", "path={} options={:#x}", path, opts);
//   REXKRNL_IMPORT_RESULT("NtCreateFile", "{:#x}", result);
//   REXKRNL_IMPORT_FAIL("NtCreateFile", "path='{}' -> {:#x}", path, result);

#define REXKRNL_IMPORT_TRACE(name, fmt, ...) REXKRNL_NOISY_TRACE("[" name "] " fmt, ##__VA_ARGS__)

#define REXKRNL_IMPORT_RESULT(name, fmt, ...) \
  REXKRNL_NOISY_TRACE("[" name "] -> " fmt, ##__VA_ARGS__)

#define REXKRNL_IMPORT_FAIL(name, fmt, ...) REXKRNL_WARN("[" name "] FAILED: " fmt, ##__VA_ARGS__)

#define REXKRNL_IMPORT_WARN(name, fmt, ...) REXKRNL_NOISY_DEBUG("[" name "] " fmt, ##__VA_ARGS__)

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
namespace stream {
class ByteStream;
}  // namespace stream
namespace runtime {
class FunctionDispatcher;
}  // namespace runtime
}  // namespace rex

struct PPCContext;

namespace rex::system {

constexpr memory::fourcc_t kKernelSaveSignature = memory::make_fourcc("KRNL");

class Dispatcher;
class XHostThread;
class KernelModule;
class XModule;
class XNotifyListener;
class XThread;
class UserModule;

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

/// Host-side metadata for a fiber managed by rexcrt hooks.
struct FiberInfo {
  rex::thread::Fiber* host_fiber;  ///< Host OS fiber handle
  uint32_t uid;                    ///< Unique identifier
  uint32_t guest_context_addr;     ///< Guest fiber context buffer address
  uint32_t guest_stack_base;       ///< Top of guest kernel stack (0 for thread fibers)
  uint32_t guest_stack_bottom;     ///< Bottom of guest kernel stack (0 for thread fibers)
  bool is_thread_fiber;            ///< true = ConvertThreadToFiber (don't free guest stack)
};

class KernelState {
 public:
  explicit KernelState(Runtime* emulator);
  ~KernelState();

  static KernelState* shared();

  Runtime* emulator() const { return emulator_; }
  memory::Memory* memory() const { return memory_; }
  runtime::FunctionDispatcher* function_dispatcher() const { return function_dispatcher_; }
  rex::filesystem::VirtualFileSystem* file_system() const { return file_system_; }

  uint32_t title_id() const;
  util::XdbfGameData title_xdbf() const;
  util::XdbfGameData module_xdbf(object_ref<UserModule> exec_module) const;

  xam::AppManager* app_manager() const { return app_manager_.get(); }
  xam::ContentManager* content_manager() const { return content_manager_.get(); }
  xam::UserProfile* user_profile() const { return user_profile_.get(); }

  // Access must be guarded by the global critical region.
  util::ObjectTable* object_table() { return &object_table_; }
  // Hang diagnostics: print every waitable object's host state and each
  // guest thread's current wait to stderr and the log.
  void DumpKernelObjects();

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

  void RegisterModule(XModule* module);
  void UnregisterModule(XModule* module);
  bool RegisterUserModule(object_ref<UserModule> module);
  void UnregisterUserModule(UserModule* module);
  bool IsKernelModule(const std::string_view name);
  object_ref<XModule> GetModule(const std::string_view name, bool user_only = false);

  object_ref<XThread> LaunchModule(object_ref<UserModule> module);
  object_ref<XThread> PrepareModuleLaunch(object_ref<UserModule> module);
  object_ref<UserModule> GetExecutableModule();
  void SetExecutableModule(object_ref<UserModule> module);
  object_ref<UserModule> LoadUserModule(const std::string_view name, bool call_entry = true);
  void UnloadUserModule(const object_ref<UserModule>& module, bool call_entry = true);

  // Recompiled module registry (populated by generated RegisterRecompiledModules)
  struct RecompiledModuleInfo {
    std::string pe_name;
    std::string guest_path;
    std::string shared_lib_name;
  };

  void RegisterRecompiledModule(const char* pe_name, const char* guest_path,
                                const char* shared_lib_name);
  std::optional<RecompiledModuleInfo> FindRecompiledModule(std::string_view guest_path);

  object_ref<KernelModule> GetKernelModule(const std::string_view name);
  template <typename T>
  object_ref<KernelModule> LoadKernelModule() {
    auto kernel_module = object_ref<KernelModule>(new T(emulator_, this));
    LoadKernelModule(kernel_module);
    return kernel_module;
  }
  template <typename T>
  object_ref<T> GetKernelModule(const std::string_view name) {
    auto module = GetKernelModule(name);
    return object_ref<T>(reinterpret_cast<T*>(module.release()));
  }

  // Terminates a title: Unloads all modules, and kills all guest threads.
  // This DOES NOT RETURN if called from a guest thread!
  void TerminateTitle();
  bool is_terminating_title() const { return terminating_title_.load(std::memory_order_acquire); }

  void RegisterThread(XThread* thread);
  void UnregisterThread(XThread* thread);
  void OnThreadExecute(XThread* thread);
  void OnThreadExit(XThread* thread);
  object_ref<XThread> GetThreadByID(uint32_t thread_id);

  FiberInfo* LookupFiber(uint32_t guest_addr);
  void RegisterFiber(uint32_t guest_addr, const FiberInfo& info);
  void UnregisterFiber(uint32_t guest_addr);

  /// Returns a fiber name for profiling
  const char* GetOrCreateFiberName(uint32_t guest_addr, const char* thread_name);

  void RegisterNotifyListener(XNotifyListener* listener);
  void UnregisterNotifyListener(XNotifyListener* listener);
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

  bool Save(stream::ByteStream* stream);
  bool Restore(stream::ByteStream* stream);

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
  void WaitForThreadsToExit(const std::vector<object_ref<XThread>>& threads, uint32_t timeout_ms);
  void LoadKernelModule(object_ref<KernelModule> kernel_module);
  void InitializeProcess(X_KPROCESS* process, uint32_t process_type, uint8_t unk_18, uint8_t unk_19,
                         uint8_t unk_1A);
  void SetProcessTLSVars(X_KPROCESS* process, uint32_t num_slots, uint32_t tls_data_size,
                         uint32_t tls_raw_data_address);
  void LoadAchievementsData();

  Runtime* emulator_;
  memory::Memory* memory_;
  runtime::FunctionDispatcher* function_dispatcher_;
  rex::filesystem::VirtualFileSystem* file_system_;

  std::unique_ptr<xam::AppManager> app_manager_;
  std::unique_ptr<xam::ContentManager> content_manager_;
  std::unique_ptr<xam::UserProfile> user_profile_;

  rex::thread::global_critical_region global_critical_region_;

  // Must be guarded by the global critical region.
  util::ObjectTable object_table_;
  std::unordered_map<uint32_t, XThread*> threads_by_id_;
  std::vector<object_ref<XNotifyListener>> notify_listeners_;
  bool has_notified_startup_ = false;

  // Protected by global_critical_region_.
  std::unordered_map<uint32_t, FiberInfo> fiber_map_;

  // Fiber name pool for profiling.
  // Never erased, Tracy references pointers async.
  std::mutex fiber_name_pool_mutex_;
  std::unordered_map<uint32_t, std::unique_ptr<char[]>> fiber_name_pool_;

  uint32_t process_type_ = X_PROCTYPE_USER;
  object_ref<UserModule> executable_module_;
  std::vector<object_ref<KernelModule>> kernel_modules_;
  std::vector<object_ref<UserModule>> user_modules_;
  // Paths in-flight in LoadUserModule. Guarded by the global critical region.
  std::unordered_set<std::string> loading_paths_;
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
  object_ref<XHostThread> dispatch_thread_;
  // Must be guarded by the global critical region.
  util::NativeList dpc_list_;
  std::condition_variable_any dispatch_cond_;
  std::list<std::function<void()>> dispatch_queue_;

  friend class XObject;
};

// Global kernel state accessor (defined in kernel_state.cpp)
KernelState* kernel_state();

// Convenience accessor for kernel memory
inline memory::Memory* kernel_memory() {
  return kernel_state()->memory();
}

}  // namespace rex::system
