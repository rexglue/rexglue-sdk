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
#include <functional>
#include <list>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <rex/bit.h>
#include <rex/filesystem/vfs.h>
#include <rex/logging.h>
#include <rex/system/util/native_list.h>
#include <rex/system/util/object_table.h>
#include <rex/system/util/xdbf_utils.h>
#include <rex/system/xam/app_manager.h>
#include <rex/system/xam/content_manager.h>
#include <rex/system/xam/user_profile.h>
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

#define REXKRNL_IMPORT_TRACE(name, fmt, ...) REXKRNL_TRACE("[" name "] " fmt, ##__VA_ARGS__)

#define REXKRNL_IMPORT_RESULT(name, fmt, ...) REXKRNL_TRACE("[" name "] -> " fmt, ##__VA_ARGS__)

#define REXKRNL_IMPORT_FAIL(name, fmt, ...) REXKRNL_WARN("[" name "] FAILED: " fmt, ##__VA_ARGS__)

#define REXKRNL_IMPORT_WARN(name, fmt, ...) REXKRNL_DEBUG("[" name "] " fmt, ##__VA_ARGS__)

namespace rex {
class Runtime;
namespace stream {
class ByteStream;
}  // namespace stream
namespace runtime {
class Processor;
}  // namespace runtime
}  // namespace rex

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
  rex::be<uint32_t> bitmap[8];                // 0x30
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

class KernelState {
 public:
  explicit KernelState(Runtime* emulator);
  ~KernelState();

  static KernelState* shared();

  Runtime* emulator() const { return emulator_; }
  memory::Memory* memory() const { return memory_; }
  runtime::Processor* processor() const { return processor_; }
  rex::filesystem::VirtualFileSystem* file_system() const { return file_system_; }

  uint32_t title_id() const;
  util::XdbfGameData title_xdbf() const;
  util::XdbfGameData module_xdbf(object_ref<UserModule> exec_module) const;

  xam::AppManager* app_manager() const { return app_manager_.get(); }
  xam::ContentManager* content_manager() const { return content_manager_.get(); }
  xam::UserProfile* user_profile() const { return user_profile_.get(); }

  // Access must be guarded by the global critical region.
  util::ObjectTable* object_table() { return &object_table_; }

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

  uint32_t AllocateTLS();
  void FreeTLS(uint32_t slot);

  void RegisterTitleTerminateNotification(uint32_t routine, uint32_t priority);
  void RemoveTitleTerminateNotification(uint32_t routine);

  void RegisterModule(XModule* module);
  void UnregisterModule(XModule* module);
  bool RegisterUserModule(object_ref<UserModule> module);
  void UnregisterUserModule(UserModule* module);
  bool IsKernelModule(const std::string_view name);
  object_ref<XModule> GetModule(const std::string_view name, bool user_only = false);

  object_ref<XThread> LaunchModule(object_ref<UserModule> module);
  object_ref<UserModule> GetExecutableModule();
  void SetExecutableModule(object_ref<UserModule> module);
  object_ref<UserModule> LoadUserModule(const std::string_view name, bool call_entry = true);
  void UnloadUserModule(const object_ref<UserModule>& module, bool call_entry = true);

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

  void RegisterThread(XThread* thread);
  void UnregisterThread(XThread* thread);
  void OnThreadExecute(XThread* thread);
  void OnThreadExit(XThread* thread);
  object_ref<XThread> GetThreadByID(uint32_t thread_id);

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

  void CompleteOverlappedDeferred(std::move_only_function<void()> completion_callback,
                                  uint32_t overlapped_ptr, X_RESULT result,
                                  std::move_only_function<void()> pre_callback = nullptr,
                                  std::move_only_function<void()> post_callback = nullptr);
  void CompleteOverlappedDeferredEx(std::move_only_function<void()> completion_callback,
                                    uint32_t overlapped_ptr, X_RESULT result,
                                    uint32_t extended_error, uint32_t length,
                                    std::move_only_function<void()> pre_callback = nullptr,
                                    std::move_only_function<void()> post_callback = nullptr);

  void CompleteOverlappedDeferred(std::move_only_function<X_RESULT()> completion_callback,
                                  uint32_t overlapped_ptr,
                                  std::move_only_function<void()> pre_callback = nullptr,
                                  std::move_only_function<void()> post_callback = nullptr);
  void CompleteOverlappedDeferredEx(
      std::move_only_function<X_RESULT(uint32_t&, uint32_t&)> completion_callback,
      uint32_t overlapped_ptr, std::move_only_function<void()> pre_callback = nullptr,
      std::move_only_function<void()> post_callback = nullptr);

  bool Save(stream::ByteStream* stream);
  bool Restore(stream::ByteStream* stream);

 private:
  void LoadKernelModule(object_ref<KernelModule> kernel_module);
  void InitializeProcess(X_KPROCESS* process, uint32_t process_type,
                         uint8_t unk_18, uint8_t unk_19, uint8_t unk_1A);
  void SetProcessTLSVars(X_KPROCESS* process, uint32_t num_slots,
                         uint32_t tls_data_size, uint32_t tls_raw_data_address);

  Runtime* emulator_;
  memory::Memory* memory_;
  runtime::Processor* processor_;
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

  uint32_t process_type_ = X_PROCTYPE_USER;
  object_ref<UserModule> executable_module_;
  std::vector<object_ref<KernelModule>> kernel_modules_;
  std::vector<object_ref<UserModule>> user_modules_;
  std::vector<TerminateNotification> terminate_notifications_;

  uint32_t kernel_guest_globals_ = 0;

  std::atomic<bool> dispatch_thread_running_;
  object_ref<XHostThread> dispatch_thread_;
  // Must be guarded by the global critical region.
  util::NativeList dpc_list_;
  std::condition_variable_any dispatch_cond_;
  std::list<std::move_only_function<void()>> dispatch_queue_;

  bit::BitMap tls_bitmap_;

  friend class XObject;
};

// Global kernel state accessor (defined in kernel_state.cpp)
KernelState* kernel_state();

// Convenience accessor for kernel memory
inline memory::Memory* kernel_memory() {
  return kernel_state()->memory();
}

}  // namespace rex::system
