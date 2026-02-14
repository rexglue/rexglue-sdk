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
#include <rex/byte_order.h>
#include <rex/logging.h>
#include <rex/thread/mutex.h>
#include <rex/kernel/util/native_list.h>
#include <rex/kernel/util/object_table.h>
#include <rex/kernel/util/xdbf_utils.h>
#include <rex/kernel/xam/app_manager.h>
#include <rex/kernel/xam/content_manager.h>
#include <rex/kernel/xam/user_profile.h>
#include <rex/kernel/xmemory.h>
#include <rex/filesystem/vfs.h>
#include <rex/kernel/xtypes.h>
#include <rex/kernel/xcontent.h>

//=============================================================================
// Kernel Import Trace Helpers
//=============================================================================
// Use these macros for consistent logging of kernel import function calls.
// Example usage:
//   REXKRNL_IMPORT_TRACE("NtCreateFile", "path={} options={:#x}", path, opts);
//   REXKRNL_IMPORT_RESULT("NtCreateFile", "{:#x}", result);
//   REXKRNL_IMPORT_FAIL("NtCreateFile", "path='{}' -> {:#x}", path, result);

#define REXKRNL_IMPORT_TRACE(name, fmt, ...) \
  REXKRNL_DEBUG("[" name "] " fmt, ##__VA_ARGS__)

#define REXKRNL_IMPORT_RESULT(name, fmt, ...) \
  REXKRNL_DEBUG("[" name "] -> " fmt, ##__VA_ARGS__)

#define REXKRNL_IMPORT_FAIL(name, fmt, ...) \
  REXKRNL_WARN("[" name "] FAILED: " fmt, ##__VA_ARGS__)

#define REXKRNL_IMPORT_WARN(name, fmt, ...) \
  REXKRNL_WARN("[" name "] " fmt, ##__VA_ARGS__)

namespace rex {
class Runtime;
namespace stream {
class ByteStream;
}  // namespace stream
namespace runtime {
class Processor;
}  // namespace runtime
namespace input {
class InputSystem;
}  // namespace input
}  // namespace rex

namespace rex::kernel {

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

struct ProcessInfoBlock {
  rex::be<uint32_t> unk_00;
  rex::be<uint32_t> unk_04;  // blink
  rex::be<uint32_t> unk_08;  // flink
  rex::be<uint32_t> unk_0C;
  rex::be<uint32_t> unk_10;
  rex::be<uint32_t> thread_count;
  uint8_t unk_18;
  uint8_t unk_19;
  uint8_t unk_1A;
  uint8_t unk_1B;
  rex::be<uint32_t> kernel_stack_size;
  rex::be<uint32_t> unk_20;
  rex::be<uint32_t> tls_data_size;
  rex::be<uint32_t> tls_raw_data_size;
  rex::be<uint16_t> tls_slot_size;
  uint8_t unk_2E;
  uint8_t process_type;
  rex::be<uint32_t> bitmap[0x20 / 4];
  rex::be<uint32_t> unk_50;
  rex::be<uint32_t> unk_54;  // blink
  rex::be<uint32_t> unk_58;  // flink
  rex::be<uint32_t> unk_5C;
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
  xam::ContentManager* content_manager() const {
    return content_manager_.get();
  }
  xam::UserProfile* user_profile() const { return user_profile_.get(); }
  rex::input::InputSystem* input_system() const { return input_system_.get(); }

  // Initialize input drivers - must be called after Runtime::Setup sets tool_mode
  void SetupInputDrivers();

  // Access must be guarded by the global critical region.
  util::ObjectTable* object_table() { return &object_table_; }

  uint32_t process_type() const;
  void set_process_type(uint32_t value);
  uint32_t process_info_block_address() const {
    return process_info_block_address_;
  }

  uint32_t AllocateTLS();
  void FreeTLS(uint32_t slot);

  void RegisterTitleTerminateNotification(uint32_t routine, uint32_t priority);
  void RemoveTitleTerminateNotification(uint32_t routine);

  void RegisterModule(XModule* module);
  void UnregisterModule(XModule* module);
  bool RegisterUserModule(object_ref<UserModule> module);
  void UnregisterUserModule(UserModule* module);
  bool IsKernelModule(const std::string_view name);
  object_ref<XModule> GetModule(const std::string_view name,
                                bool user_only = false);

  object_ref<XThread> LaunchModule(object_ref<UserModule> module);
  object_ref<UserModule> GetExecutableModule();
  void SetExecutableModule(object_ref<UserModule> module);
  object_ref<UserModule> LoadUserModule(const std::string_view name,
                                        bool call_entry = true);
  void UnloadUserModule(const object_ref<UserModule>& module,
                        bool call_entry = true);

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
  void CompleteOverlappedEx(uint32_t overlapped_ptr, X_RESULT result,
                            uint32_t extended_error, uint32_t length);

  void CompleteOverlappedImmediate(uint32_t overlapped_ptr, X_RESULT result);
  void CompleteOverlappedImmediateEx(uint32_t overlapped_ptr, X_RESULT result,
                                     uint32_t extended_error, uint32_t length);

  void CompleteOverlappedDeferred(
      std::move_only_function<void()> completion_callback, uint32_t overlapped_ptr,
      X_RESULT result, std::move_only_function<void()> pre_callback = nullptr,
      std::move_only_function<void()> post_callback = nullptr);
  void CompleteOverlappedDeferredEx(
      std::move_only_function<void()> completion_callback, uint32_t overlapped_ptr,
      X_RESULT result, uint32_t extended_error, uint32_t length,
      std::move_only_function<void()> pre_callback = nullptr,
      std::move_only_function<void()> post_callback = nullptr);

  void CompleteOverlappedDeferred(
      std::move_only_function<X_RESULT()> completion_callback, uint32_t overlapped_ptr,
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

  Runtime* emulator_;
  memory::Memory* memory_;
  runtime::Processor* processor_;
  rex::filesystem::VirtualFileSystem* file_system_;

  std::unique_ptr<xam::AppManager> app_manager_;
  std::unique_ptr<xam::ContentManager> content_manager_;
  std::unique_ptr<xam::UserProfile> user_profile_;
  std::unique_ptr<rex::input::InputSystem> input_system_;

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

  uint32_t process_info_block_address_ = 0;

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
inline memory::Memory* kernel_memory() { return kernel_state()->memory(); }

}  // namespace rex::kernel
