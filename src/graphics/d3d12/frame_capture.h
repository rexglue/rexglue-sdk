// ReXGlue D3D12 guest-frame capture journal.

#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <rex/graphics/draw_capture_mailbox.h>

namespace rex::graphics::d3d12 {

// The policy is kept free of D3D12 and RenderDoc types so its frame-boundary
// and bounded-journal contract can be tested without a graphics device.
struct FrameCapturePolicy {
  // Late-game frames exceed 4096 events. Each event holds an ~80 KiB register file.
  static constexpr size_t kMaxEvents = 16384;
  static constexpr size_t kMaxBytes = size_t(2) * 1024 * 1024 * 1024;

  static bool CanAppend(size_t event_count, size_t bytes_used, size_t additional_bytes) noexcept {
    return event_count < kMaxEvents && bytes_used <= kMaxBytes &&
           additional_bytes <= kMaxBytes - bytes_used;
  }
  static bool IsComplete(bool capture_started, bool capture_ended, bool ending_swap,
                         bool journal_complete, bool renderdoc_capture_present) noexcept {
    return capture_started && capture_ended && ending_swap && journal_complete &&
           renderdoc_capture_present;
  }
};

class FrameCapture {
 public:
  enum class EventKind { kDraw, kCopy, kSwap };

  struct ShaderView {
    bool present = false;
    uint64_t hash = 0;
    const uint32_t* dwords = nullptr;
    size_t dword_count = 0;
  };

  struct IndexInfo {
    bool present = false;
    uint32_t format = 0;
    uint32_t endianness = 0;
    uint32_t count = 0;
    uint32_t guest_base = 0;
    uint64_t length = 0;
  };

  struct HostDrawInfo {
    uint32_t used_texture_mask = 0;
    uint32_t normalized_color_mask = 0;
    uint64_t vertex_shader_hash = 0;
    uint64_t pixel_shader_hash = 0;
    uint32_t guest_primitive_type = 0;
    uint32_t host_primitive_type = 0;
    uint32_t host_vertex_shader_type = 0;
    uint32_t tessellation_mode = 0;
    uint32_t guest_draw_vertex_count = 0;
    uint32_t host_draw_vertex_count = 0;
    uint32_t line_loop_closing_index = 0;
    uint32_t index_buffer_type = 0;
    uint32_t guest_index_base = 0;
    uint32_t host_index_format = 0;
    uint32_t host_shader_index_endian = 0;
    bool host_primitive_reset_enabled = false;
    uint32_t native_topology = 0;
    uint32_t native_vertex_count = 0;
    uint32_t native_index_count = 0;
    bool half_pixel_offset = false;
  };

  class DrawScope {
   public:
    DrawScope() = default;
    ~DrawScope();
    DrawScope(const DrawScope&) = delete;
    DrawScope& operator=(const DrawScope&) = delete;
    DrawScope(DrawScope&& other) noexcept;
    DrawScope& operator=(DrawScope&& other) noexcept;

    std::string marker() const;
    uint64_t event_id() const { return event_id_; }
    bool host_issued() const { return host_issued_; }
    bool copy() const { return copy_; }
    diagnostic::DrawCaptureAvailability acquisition() const { return acquisition_; }
    void MarkHostIssued(const HostDrawInfo& host_info);
    void MarkCopy(bool success);
    void SetResult(bool success) { success_ = success; }
    void SetResult(bool success, diagnostic::DrawCaptureReason reason) {
      success_ = success;
      execution_reason_ = reason;
    }
    void SetCaptureResult(diagnostic::DrawCaptureAvailability availability,
                          diagnostic::DrawCaptureReason reason) {
      acquisition_ = availability;
      acquisition_reason_ = reason;
    }

   private:
    friend class FrameCapture;
    DrawScope(FrameCapture* owner, uint64_t event_id) : owner_(owner), event_id_(event_id) {}
    void Finish() noexcept;

    FrameCapture* owner_ = nullptr;
    uint64_t event_id_ = 0;
    bool host_issued_ = false;
    bool copy_ = false;
    bool success_ = false;
    HostDrawInfo host_info_;
    diagnostic::DrawCaptureReason execution_reason_ = diagnostic::DrawCaptureReason::kNone;
    diagnostic::DrawCaptureAvailability acquisition_ =
        diagnostic::DrawCaptureAvailability::kComplete;
    diagnostic::DrawCaptureReason acquisition_reason_ = diagnostic::DrawCaptureReason::kNone;
  };

  class CopyScope {
   public:
    CopyScope() = default;
    ~CopyScope();
    CopyScope(const CopyScope&) = delete;
    CopyScope& operator=(const CopyScope&) = delete;
    CopyScope(CopyScope&& other) noexcept;
    CopyScope& operator=(CopyScope&& other) noexcept;

    std::string marker() const;
    uint64_t event_id() const { return event_id_; }
    void SetResult(bool success, std::string_view mode);
    void SetCaptureResult(diagnostic::DrawCaptureAvailability availability,
                          diagnostic::DrawCaptureReason reason) {
      acquisition_ = availability;
      acquisition_reason_ = reason;
    }

   private:
    friend class FrameCapture;
    CopyScope(FrameCapture* owner, uint64_t event_id) : owner_(owner), event_id_(event_id) {}
    void Finish() noexcept;

    FrameCapture* owner_ = nullptr;
    uint64_t event_id_ = 0;
    bool result_set_ = false;
    bool success_ = false;
    std::string mode_;
    diagnostic::DrawCaptureAvailability acquisition_ =
        diagnostic::DrawCaptureAvailability::kComplete;
    diagnostic::DrawCaptureReason acquisition_reason_ = diagnostic::DrawCaptureReason::kNone;
  };

  FrameCapture() = default;
  ~FrameCapture() = default;

  FrameCapture(const FrameCapture&) = delete;
  FrameCapture& operator=(const FrameCapture&) = delete;

  // These methods are called on the graphics command-processor thread.
  DrawScope BeginDraw(uint64_t frame, uint64_t submission, uint32_t primitive_type,
                      uint32_t index_count, bool major_mode_explicit, const IndexInfo& index_info,
                      const uint32_t* registers, size_t register_count,
                      const ShaderView& vertex_shader, const ShaderView& pixel_shader);
  CopyScope BeginCopy(uint64_t frame, uint64_t submission, const uint32_t* registers,
                      size_t register_count);

  // Records the swap marker immediately before the host submission. The
  // returned label is empty when capture is inactive or the journal failed.
  std::string RecordSwapEnd(uint64_t frame, uint64_t submission, uint64_t frontbuffer_ptr,
                            uint32_t frontbuffer_width, uint32_t frontbuffer_height,
                            const uint32_t* registers, size_t register_count);

  // Starts a device-independent journal after a completed swap. The initial
  // swap is stored as the first completed event so consumers can verify both
  // frame boundaries without RenderDoc or filesystem artifacts.
  bool BeginOwnedFrame(uint64_t frame, uint64_t submission, uint64_t frontbuffer_ptr,
                       uint32_t frontbuffer_width, uint32_t frontbuffer_height,
                       const uint32_t* registers, size_t register_count);
  std::shared_ptr<diagnostic::DrawCaptureFrameSnapshot> TakeOwnedFrame(uint64_t end_frame,
                                                                       uint64_t end_submission);
  bool IsOwnedActive() const { return owned_active_; }
  const std::string& failure_reason() const { return failure_reason_; }
  size_t event_count() const { return events_.size(); }
  size_t bytes_used() const { return bytes_used_; }

  // A successful IssueSwap/EndSubmission starts the next frame capture when
  // idle, or closes the currently captured guest frame when active.
  void OnCompletedSwap(void* device, const std::filesystem::path& requested_path,
                       uint64_t completed_frame, uint64_t completed_submission);
  void Abort(void* device, std::string_view reason);

  bool IsActive() const { return state_ == State::kCapturing || owned_active_; }
  bool IsIdle() const { return state_ == State::kIdle && !owned_active_; }
  bool HasFailed() const {
    return state_ == State::kFailed || (!owned_active_ && !failure_reason_.empty());
  }

 private:
  enum class State { kIdle, kCapturing, kFinished, kFailed };

  struct ShaderRecord {
    uint32_t type = 0;
    uint64_t hash = 0;
    std::vector<uint32_t> dwords;
    std::string file;
  };

  struct Event {
    uint64_t id = 0;
    uint64_t frame = 0;
    uint64_t submission = 0;
    EventKind kind = EventKind::kDraw;
    std::vector<uint32_t> registers;
    std::string registers_file;

    uint32_t primitive_type = 0;
    uint32_t index_count = 0;
    bool major_mode_explicit = false;
    IndexInfo index_info;
    uint64_t vertex_shader_hash = 0;
    uint64_t pixel_shader_hash = 0;
    std::string vertex_shader_file;
    std::string pixel_shader_file;

    std::string marker;
    std::string outcome;
    bool success = false;
    bool host_issued = false;
    diagnostic::DrawCaptureExecutionStatus execution_status =
        diagnostic::DrawCaptureExecutionStatus::kUnfinished;
    diagnostic::DrawCaptureReason execution_reason = diagnostic::DrawCaptureReason::kNone;
    diagnostic::DrawCaptureAvailability acquisition =
        diagnostic::DrawCaptureAvailability::kComplete;
    diagnostic::DrawCaptureReason acquisition_reason = diagnostic::DrawCaptureReason::kNone;
    HostDrawInfo host_info;
    std::string copy_mode;

    uint64_t frontbuffer_ptr = 0;
    uint32_t frontbuffer_width = 0;
    uint32_t frontbuffer_height = 0;
  };

  DrawScope BeginDrawInternal(uint64_t frame, uint64_t submission, uint32_t primitive_type,
                              uint32_t index_count, bool major_mode_explicit,
                              const IndexInfo& index_info, const uint32_t* registers,
                              size_t register_count, const ShaderView& vertex_shader,
                              const ShaderView& pixel_shader);
  CopyScope BeginCopyInternal(uint64_t frame, uint64_t submission, const uint32_t* registers,
                              size_t register_count);

  bool CopyRegisters(Event& event, const uint32_t* registers, size_t register_count);
  bool AddShader(uint32_t type, const ShaderView& shader, std::string& file_out);
  void FinishDraw(uint64_t event_id, bool host_issued, bool copy, bool success,
                  const HostDrawInfo& host_info, diagnostic::DrawCaptureReason execution_reason,
                  diagnostic::DrawCaptureAvailability acquisition,
                  diagnostic::DrawCaptureReason acquisition_reason) noexcept;
  void FinishCopy(uint64_t event_id, bool result_set, bool success, std::string_view mode,
                  diagnostic::DrawCaptureAvailability acquisition,
                  diagnostic::DrawCaptureReason acquisition_reason) noexcept;

  void MarkFailure(std::string_view reason);
  bool PrepareOutput(const std::filesystem::path& path);
  bool AcquireRenderDoc();
  bool StartRenderDoc(void* device);
  bool EndRenderDoc();
  bool GetRenderDocCapturePath(std::filesystem::path& path_out) const;
  bool WriteArtifacts(bool complete);
  bool WriteNewFile(const std::filesystem::path& path, const std::string& bytes) const;
  bool WriteDwords(const std::filesystem::path& path, const std::vector<uint32_t>& dwords,
                   bool guest_big_endian) const;
  bool WriteJournal(std::vector<std::string>& files_out);
  bool WriteManifest(bool complete, const std::vector<std::string>& files);
  bool AppendOwnedStartSwap(uint64_t frame, uint64_t submission, uint64_t frontbuffer_ptr,
                            uint32_t frontbuffer_width, uint32_t frontbuffer_height,
                            const uint32_t* registers, size_t register_count);
  bool BuildOwnedFrameSnapshot(uint64_t end_frame, uint64_t end_submission,
                               diagnostic::DrawCaptureFrameSnapshot& snapshot) const;

  State state_ = State::kIdle;
  std::filesystem::path output_path_;
  std::filesystem::path renderdoc_capture_path_;
  std::vector<Event> events_;
  std::vector<ShaderRecord> shaders_;
  std::map<std::pair<uint32_t, uint64_t>, size_t> shader_indices_;
  size_t bytes_used_ = 0;
  uint64_t next_event_id_ = 1;
  uint64_t start_frame_ = 0;
  uint64_t start_submission_ = 0;
  uint64_t end_frame_ = 0;
  uint64_t end_submission_ = 0;
  uint64_t pending_swap_event_id_ = 0;
  std::string failure_reason_;

  // RenderDoc API1.0 is obtained from an already injected module. The pointer
  // is intentionally opaque here so the policy header remains device-free.
  void* renderdoc_api_ = nullptr;
  uint32_t captures_before_ = 0;
  bool renderdoc_started_ = false;
  void* renderdoc_device_ = nullptr;
  int renderdoc_api_major_ = 0;
  int renderdoc_api_minor_ = 0;
  int renderdoc_api_patch_ = 0;
  bool artifacts_written_ = false;

  bool owned_active_ = false;
};

}  // namespace rex::graphics::d3d12
