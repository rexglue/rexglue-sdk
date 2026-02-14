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
#include <string>

#include <rex/system/thread_state.h>
#include <rex/system/util/native_list.h>
#include <rex/system/xmutant.h>
#include <rex/system/xobject.h>
#include <rex/system/xtypes.h>
#include <rex/thread.h>
#include <rex/thread/mutex.h>

namespace rex::system {

constexpr memory::fourcc_t kThreadSaveSignature = memory::make_fourcc("THRD");

class XEvent;

constexpr uint32_t X_CREATE_SUSPENDED = 0x00000001;

constexpr uint32_t X_TLS_OUT_OF_INDEXES = UINT32_MAX;

struct XAPC {
  static const uint32_t kSize = 40;
  static const uint32_t kDummyKernelRoutine = 0xF00DFF00;
  static const uint32_t kDummyRundownRoutine = 0xF00DFF01;

  // KAPC is 0x28(40) bytes? (what's passed to ExAllocatePoolWithTag)
  // This is 4b shorter than NT - looks like the reserved dword at +4 is gone.
  // NOTE: stored in guest memory.
  uint8_t type;                       // +0
  uint8_t unk1;                       // +1
  uint8_t processor_mode;             // +2
  uint8_t enqueued;                   // +3
  rex::be<uint32_t> thread_ptr;       // +4
  rex::be<uint32_t> flink;            // +8
  rex::be<uint32_t> blink;            // +12
  rex::be<uint32_t> kernel_routine;   // +16
  rex::be<uint32_t> rundown_routine;  // +20
  rex::be<uint32_t> normal_routine;   // +24
  rex::be<uint32_t> normal_context;   // +28
  rex::be<uint32_t> arg1;             // +32
  rex::be<uint32_t> arg2;             // +36

  void Initialize() {
    type = 18;  // ApcObject
    unk1 = 0;
    processor_mode = 0;
    enqueued = 0;
    thread_ptr = 0;
    flink = blink = 0;
    kernel_routine = 0;
    normal_routine = 0;
    normal_context = 0;
    arg1 = arg2 = 0;
  }
};

// Processor Control Region
struct X_KPCR {
  rex::be<uint32_t> tls_ptr;         // 0x0
  uint8_t unk_04[0x2C];              // 0x4
  rex::be<uint32_t> pcr_ptr;         // 0x30
  uint8_t unk_34[0x3C];              // 0x34
  rex::be<uint32_t> stack_base_ptr;  // 0x70 Stack base address (high addr)
  rex::be<uint32_t> stack_end_ptr;   // 0x74 Stack end (low addr)
  uint8_t unk_78[0x88];              // 0x78
  rex::be<uint32_t> current_thread;  // 0x100
  uint8_t unk_104[0x8];              // 0x104
  uint8_t current_cpu;               // 0x10C
  uint8_t unk_10D[0x43];             // 0x10D
  rex::be<uint32_t> dpc_active;      // 0x150
};

struct X_KTHREAD {
  X_DISPATCH_HEADER header;            // 0x0
  rex::be<uint32_t> unk_10;            // 0x10
  rex::be<uint32_t> unk_14;            // 0x14
  uint8_t unk_18[0x28];                // 0x10
  rex::be<uint32_t> unk_40;            // 0x40
  rex::be<uint32_t> unk_44;            // 0x44
  rex::be<uint32_t> unk_48;            // 0x48
  rex::be<uint32_t> unk_4C;            // 0x4C
  uint8_t unk_50[0x4];                 // 0x50
  rex::be<uint16_t> unk_54;            // 0x54
  rex::be<uint16_t> unk_56;            // 0x56
  uint8_t unk_58[0x4];                 // 0x58
  rex::be<uint32_t> stack_base;        // 0x5C
  rex::be<uint32_t> stack_limit;       // 0x60
  uint8_t unk_64[0x4];                 // 0x64
  rex::be<uint32_t> tls_address;       // 0x68
  uint8_t unk_6C;                      // 0x6C
  uint8_t unk_6D[0x7];                 // 0x6D
  rex::be<uint32_t> unk_74;            // 0x74
  rex::be<uint32_t> unk_78;            // 0x78
  rex::be<uint32_t> unk_7C;            // 0x7C
  rex::be<uint32_t> unk_80;            // 0x80
  rex::be<uint32_t> unk_84;            // 0x84
  uint8_t unk_88[0x3];                 // 0x88
  uint8_t unk_8B;                      // 0x8B
  uint8_t unk_8C[0x10];                // 0x8C
  rex::be<uint32_t> unk_9C;            // 0x9C
  uint8_t unk_A0[0x10];                // 0xA0
  int32_t apc_disable_count;           // 0xB0
  uint8_t unk_B4[0x8];                 // 0xB4
  uint8_t suspend_count;               // 0xBC
  uint8_t unk_BD;                      // 0xBD
  uint8_t unk_BE;                      // 0xBE
  uint8_t current_cpu;                 // 0xBF
  uint8_t unk_C0[0x10];                // 0xC0
  rex::be<uint32_t> stack_alloc_base;  // 0xD0
  uint8_t unk_D4[0x5C];                // 0xD4
  rex::be<uint64_t> create_time;       // 0x130
  rex::be<uint64_t> exit_time;         // 0x138
  rex::be<uint32_t> exit_status;       // 0x140
  rex::be<uint32_t> unk_144;           // 0x144
  rex::be<uint32_t> unk_148;           // 0x148
  rex::be<uint32_t> thread_id;         // 0x14C
  rex::be<uint32_t> start_address;     // 0x150
  rex::be<uint32_t> unk_154;           // 0x154
  rex::be<uint32_t> unk_158;           // 0x158
  uint8_t unk_15C[0x4];                // 0x15C
  rex::be<uint32_t> last_error;        // 0x160
  rex::be<uint32_t> fiber_ptr;         // 0x164
  uint8_t unk_168[0x4];                // 0x168
  rex::be<uint32_t> creation_flags;    // 0x16C
  uint8_t unk_170[0xC];                // 0x170
  rex::be<uint32_t> unk_17C;           // 0x17C
  uint8_t unk_180[0x930];              // 0x180

  // This struct is actually quite long... so uh, not filling this out!
};
static_assert_size(X_KTHREAD, 0xAB0);

class XThread : public XObject {
 public:
  static const XObject::Type kObjectType = XObject::Type::Thread;

  static constexpr uint32_t kStackAddressRangeBegin = 0x70000000;
  static constexpr uint32_t kStackAddressRangeEnd = 0x7F000000;

  struct CreationParams {
    uint32_t stack_size;
    uint32_t xapi_thread_startup;
    uint32_t start_address;
    uint32_t start_context;
    uint32_t creation_flags;
  };

  XThread(KernelState* kernel_state);
  XThread(KernelState* kernel_state, uint32_t stack_size, uint32_t xapi_thread_startup,
          uint32_t start_address, uint32_t start_context, uint32_t creation_flags,
          bool guest_thread, bool main_thread = false);
  ~XThread() override;

  static bool IsInThread(XThread* other);
  static bool IsInThread();
  static XThread* GetCurrentThread();
  static uint32_t GetCurrentThreadHandle();
  static uint32_t GetCurrentThreadId();

  static uint32_t GetLastError();
  static void SetLastError(uint32_t error_code);

  const CreationParams* creation_params() const { return &creation_params_; }
  uint32_t tls_ptr() const { return tls_static_address_; }
  uint32_t pcr_ptr() const { return pcr_address_; }
  // True if the thread is created by the guest app.
  bool is_guest_thread() const { return guest_thread_; }
  bool main_thread() const { return main_thread_; }
  bool is_running() const { return running_; }

  uint32_t thread_id() const { return thread_id_; }
  uint32_t last_error();
  void set_last_error(uint32_t error_code);
  void set_name(const std::string_view name);

  X_STATUS Create();
  X_STATUS Exit(int exit_code);
  X_STATUS Terminate(int exit_code);

  virtual void Execute();

  virtual void Reenter(uint32_t address);

  void EnterCriticalRegion();
  void LeaveCriticalRegion();
  uint32_t RaiseIrql(uint32_t new_irql);
  void LowerIrql(uint32_t new_irql);

  void CheckApcs();
  void LockApc();
  void UnlockApc(bool queue_delivery);
  util::NativeList* apc_list() { return &apc_list_; }
  void EnqueueApc(uint32_t normal_routine, uint32_t normal_context, uint32_t arg1, uint32_t arg2);

  int32_t priority() const { return priority_; }
  int32_t QueryPriority();
  void SetPriority(int32_t increment);

  // Xbox thread IDs:
  // 0 - core 0, thread 0 - user
  // 1 - core 0, thread 1 - user
  // 2 - core 1, thread 0 - sometimes xcontent
  // 3 - core 1, thread 1 - user
  // 4 - core 2, thread 0 - xaudio
  // 5 - core 2, thread 1 - user
  void SetAffinity(uint32_t affinity);
  uint8_t active_cpu() const;
  void SetActiveCpu(uint8_t cpu_index);

  bool GetTLSValue(uint32_t slot, uint32_t* value_out);
  bool SetTLSValue(uint32_t slot, uint32_t value);

  uint32_t suspend_count();
  X_STATUS Resume(uint32_t* out_suspend_count = nullptr);
  X_STATUS Suspend(uint32_t* out_suspend_count = nullptr);
  X_STATUS Delay(uint32_t processor_mode, uint32_t alertable, uint64_t interval);

  rex::thread::Thread* thread() { return thread_.get(); }
  runtime::ThreadState* thread_state() { return thread_state_.get(); }

  virtual bool Save(stream::ByteStream* stream) override;
  static object_ref<XThread> Restore(KernelState* kernel_state, stream::ByteStream* stream);

  // Internal - do not use.
  void AcquireMutantOnStartup(object_ref<XMutant> mutant) {
    pending_mutant_acquires_.push_back(mutant);
  }

 protected:
  bool AllocateStack(uint32_t size);
  void FreeStack();
  void InitializeGuestObject();

  void DeliverAPCs();
  void RundownAPCs();

  rex::thread::WaitHandle* GetWaitHandle() override { return thread_.get(); }

  CreationParams creation_params_ = {0, 0, 0, 0, 0};

  std::vector<object_ref<XMutant>> pending_mutant_acquires_;

  uint32_t thread_id_ = 0;
  uint32_t scratch_address_ = 0;
  uint32_t scratch_size_ = 0;
  uint32_t tls_static_address_ = 0;
  uint32_t tls_dynamic_address_ = 0;
  uint32_t tls_total_size_ = 0;
  uint32_t pcr_address_ = 0;
  uint32_t stack_alloc_base_ = 0;  // Stack alloc base
  uint32_t stack_alloc_size_ = 0;  // Stack alloc size
  uint32_t stack_base_ = 0;        // High address
  uint32_t stack_limit_ = 0;       // Low address
  bool guest_thread_ = false;
  bool main_thread_ = false;  // Entry-point thread
  bool running_ = false;

  std::string thread_name_;
  std::unique_ptr<runtime::ThreadState> thread_state_;

  int32_t priority_ = 0;

  rex::thread::global_critical_region global_critical_region_;
  std::atomic<uint32_t> irql_ = {0};
  util::NativeList apc_list_;

  std::unique_ptr<rex::thread::Thread> thread_ = nullptr;
};

class XHostThread : public XThread {
 public:
  XHostThread(KernelState* kernel_state, uint32_t stack_size, uint32_t creation_flags,
              std::function<int()> host_fn);

  virtual void Execute();

 private:
  std::function<int()> host_fn_;
};

}  // namespace rex::system
