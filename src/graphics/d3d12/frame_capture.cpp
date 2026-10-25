// ReXGlue D3D12 guest-frame capture journal.

#include "frame_capture.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>
#include <limits>
#include <new>
#include <system_error>

#include <fmt/format.h>

#include <rex/filesystem.h>
#include <rex/graphics/register_file.h>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "renderdoc_app.h"
#endif

#include <rex/logging.h>
#include <rex/string.h>

namespace rex::graphics::d3d12 {

namespace {

constexpr size_t kEventMetadataBytes = 1024;
constexpr size_t kShaderTypeVertex = 0;
constexpr size_t kShaderTypePixel = 1;
constexpr size_t kRegisterCountExpected = RegisterFile::kRegisterCount;
constexpr uint32_t kMaxRenderDocPathBytes = 32768;

void AppendJsonString(std::string& json, std::string_view value) {
  json.push_back('"');
  for (const char character : value) {
    switch (character) {
      case '"':
        json += "\\\"";
        break;
      case '\\':
        json += "\\\\";
        break;
      case '\n':
        json += "\\n";
        break;
      case '\r':
        json += "\\r";
        break;
      case '\t':
        json += "\\t";
        break;
      default:
        json.push_back(character);
        break;
    }
  }
  json.push_back('"');
}

std::string Hex(uint64_t value) {
  return fmt::format("{:016X}", value);
}

const char* EventKindName(FrameCapture::EventKind kind) {
  switch (kind) {
    case FrameCapture::EventKind::kDraw:
      return "draw";
    case FrameCapture::EventKind::kCopy:
      return "copy";
    case FrameCapture::EventKind::kSwap:
      return "swap";
  }
  return "unknown";
}

const char* ExecutionStatusName(diagnostic::DrawCaptureExecutionStatus status) {
  switch (status) {
    case diagnostic::DrawCaptureExecutionStatus::kUnfinished:
      return "unfinished";
    case diagnostic::DrawCaptureExecutionStatus::kIssued:
      return "issued";
    case diagnostic::DrawCaptureExecutionStatus::kNoEffect:
      return "no_effect";
    case diagnostic::DrawCaptureExecutionStatus::kFailed:
      return "failed";
  }
  return "unknown";
}

const char* AvailabilityName(diagnostic::DrawCaptureAvailability availability) {
  switch (availability) {
    case diagnostic::DrawCaptureAvailability::kUnavailable:
      return "unavailable";
    case diagnostic::DrawCaptureAvailability::kPartial:
      return "partial";
    case diagnostic::DrawCaptureAvailability::kComplete:
      return "complete";
  }
  return "unknown";
}

const char* ReasonName(diagnostic::DrawCaptureReason reason) {
  switch (reason) {
    case diagnostic::DrawCaptureReason::kNone:
      return "none";
    case diagnostic::DrawCaptureReason::kNoVertexShader:
      return "no_vertex_shader";
    case diagnostic::DrawCaptureReason::kRenderTargetPath:
      return "render_target_path";
    case diagnostic::DrawCaptureReason::kResolutionScale:
      return "resolution_scale";
    case diagnostic::DrawCaptureReason::kMemexport:
      return "memexport";
    case diagnostic::DrawCaptureReason::kIndexTranslation:
      return "index_translation";
    case diagnostic::DrawCaptureReason::kVertexFetch:
      return "vertex_fetch";
    case diagnostic::DrawCaptureReason::kIndexSource:
      return "index_source";
    case diagnostic::DrawCaptureReason::kReadbackBudget:
      return "readback_budget";
    case diagnostic::DrawCaptureReason::kReadbackAllocation:
      return "readback_allocation";
    case diagnostic::DrawCaptureReason::kTextureUnresolved:
      return "texture_unresolved";
    case diagnostic::DrawCaptureReason::kTextureDimension:
      return "texture_dimension";
    case diagnostic::DrawCaptureReason::kTextureFormat:
      return "texture_format";
    case diagnostic::DrawCaptureReason::kTextureSource:
      return "texture_source";
    case diagnostic::DrawCaptureReason::kReadback:
      return "readback";
    case diagnostic::DrawCaptureReason::kSubmission:
      return "submission";
    case diagnostic::DrawCaptureReason::kFence:
      return "fence";
    case diagnostic::DrawCaptureReason::kJournal:
      return "journal";
    case diagnostic::DrawCaptureReason::kSwap:
      return "swap";
    case diagnostic::DrawCaptureReason::kNoEffect:
      return "no_effect";
    case diagnostic::DrawCaptureReason::kExecutionFailure:
      return "execution_failure";
    case diagnostic::DrawCaptureReason::kSignMaskUnused:
      return "sign_mask_unused";
  }
  return "unknown";
}

}  // namespace

FrameCapture::DrawScope::~DrawScope() {
  Finish();
}

FrameCapture::DrawScope::DrawScope(DrawScope&& other) noexcept
    : owner_(other.owner_),
      event_id_(other.event_id_),
      host_issued_(other.host_issued_),
      copy_(other.copy_),
      success_(other.success_),
      host_info_(other.host_info_),
      execution_reason_(other.execution_reason_),
      acquisition_(other.acquisition_),
      acquisition_reason_(other.acquisition_reason_) {
  other.owner_ = nullptr;
  other.event_id_ = 0;
}

FrameCapture::DrawScope& FrameCapture::DrawScope::operator=(DrawScope&& other) noexcept {
  if (this != &other) {
    Finish();
    owner_ = other.owner_;
    event_id_ = other.event_id_;
    host_issued_ = other.host_issued_;
    copy_ = other.copy_;
    success_ = other.success_;
    host_info_ = other.host_info_;
    execution_reason_ = other.execution_reason_;
    acquisition_ = other.acquisition_;
    acquisition_reason_ = other.acquisition_reason_;
    other.owner_ = nullptr;
    other.event_id_ = 0;
  }
  return *this;
}

std::string FrameCapture::DrawScope::marker() const {
  if (!owner_ || !event_id_ || event_id_ >= owner_->next_event_id_) {
    return {};
  }
  const auto it = std::find_if(owner_->events_.begin(), owner_->events_.end(),
                               [this](const Event& event) { return event.id == event_id_; });
  return it == owner_->events_.end() ? std::string() : it->marker;
}

void FrameCapture::DrawScope::MarkHostIssued(const HostDrawInfo& host_info) {
  if (!owner_ || !event_id_) {
    return;
  }
  host_issued_ = true;
  success_ = true;
  host_info_ = host_info;
  const auto it = std::find_if(owner_->events_.begin(), owner_->events_.end(),
                               [this](const Event& event) { return event.id == event_id_; });
  if (it != owner_->events_.end()) {
    it->host_info = host_info;
  }
}

void FrameCapture::DrawScope::MarkCopy(bool success) {
  if (!owner_ || !event_id_) {
    return;
  }
  copy_ = true;
  success_ = success;
}

void FrameCapture::DrawScope::Finish() noexcept {
  if (!owner_ || !event_id_) {
    return;
  }
  owner_->FinishDraw(event_id_, host_issued_, copy_, success_, host_info_, execution_reason_,
                     acquisition_, acquisition_reason_);
  owner_ = nullptr;
  event_id_ = 0;
}

FrameCapture::CopyScope::~CopyScope() {
  Finish();
}

FrameCapture::CopyScope::CopyScope(CopyScope&& other) noexcept
    : owner_(other.owner_),
      event_id_(other.event_id_),
      result_set_(other.result_set_),
      success_(other.success_),
      mode_(std::move(other.mode_)),
      acquisition_(other.acquisition_),
      acquisition_reason_(other.acquisition_reason_) {
  other.owner_ = nullptr;
  other.event_id_ = 0;
}

FrameCapture::CopyScope& FrameCapture::CopyScope::operator=(CopyScope&& other) noexcept {
  if (this != &other) {
    Finish();
    owner_ = other.owner_;
    event_id_ = other.event_id_;
    result_set_ = other.result_set_;
    success_ = other.success_;
    mode_ = std::move(other.mode_);
    acquisition_ = other.acquisition_;
    acquisition_reason_ = other.acquisition_reason_;
    other.owner_ = nullptr;
    other.event_id_ = 0;
  }
  return *this;
}

std::string FrameCapture::CopyScope::marker() const {
  if (!owner_ || !event_id_ || event_id_ >= owner_->next_event_id_) {
    return {};
  }
  const auto it = std::find_if(owner_->events_.begin(), owner_->events_.end(),
                               [this](const Event& event) { return event.id == event_id_; });
  return it == owner_->events_.end() ? std::string() : it->marker;
}

void FrameCapture::CopyScope::SetResult(bool success, std::string_view mode) {
  if (!owner_ || !event_id_) {
    return;
  }
  result_set_ = true;
  success_ = success;
  mode_.assign(mode);
}

void FrameCapture::CopyScope::Finish() noexcept {
  if (!owner_ || !event_id_) {
    return;
  }
  owner_->FinishCopy(event_id_, result_set_, success_, mode_, acquisition_, acquisition_reason_);
  owner_ = nullptr;
  event_id_ = 0;
}

FrameCapture::DrawScope FrameCapture::BeginDraw(
    uint64_t frame, uint64_t submission, uint32_t primitive_type, uint32_t index_count,
    bool major_mode_explicit, const IndexInfo& index_info, const uint32_t* registers,
    size_t register_count, const ShaderView& vertex_shader, const ShaderView& pixel_shader) {
  return BeginDrawInternal(frame, submission, primitive_type, index_count, major_mode_explicit,
                           index_info, registers, register_count, vertex_shader, pixel_shader);
}

FrameCapture::CopyScope FrameCapture::BeginCopy(uint64_t frame, uint64_t submission,
                                                const uint32_t* registers, size_t register_count) {
  return BeginCopyInternal(frame, submission, registers, register_count);
}

FrameCapture::DrawScope FrameCapture::BeginDrawInternal(
    uint64_t frame, uint64_t submission, uint32_t primitive_type, uint32_t index_count,
    bool major_mode_explicit, const IndexInfo& index_info, const uint32_t* registers,
    size_t register_count, const ShaderView& vertex_shader, const ShaderView& pixel_shader) {
  if (!IsActive() || !failure_reason_.empty()) {
    return {};
  }
  if (events_.size() >= FrameCapturePolicy::kMaxEvents) {
    MarkFailure("journal_bounds");
    return {};
  }
  const bool vertex_size_overflow =
      vertex_shader.present &&
      vertex_shader.dword_count > std::numeric_limits<size_t>::max() / sizeof(uint32_t);
  const bool pixel_size_overflow =
      pixel_shader.present &&
      pixel_shader.dword_count > std::numeric_limits<size_t>::max() / sizeof(uint32_t);
  const size_t vertex_bytes =
      vertex_size_overflow
          ? 0
          : (vertex_shader.present ? vertex_shader.dword_count * sizeof(uint32_t) : 0);
  const size_t pixel_bytes =
      pixel_size_overflow
          ? 0
          : (pixel_shader.present ? pixel_shader.dword_count * sizeof(uint32_t) : 0);
  if (register_count != kRegisterCountExpected || !registers || vertex_size_overflow ||
      pixel_size_overflow || vertex_bytes > FrameCapturePolicy::kMaxBytes ||
      pixel_bytes > FrameCapturePolicy::kMaxBytes ||
      register_count > std::numeric_limits<size_t>::max() / sizeof(uint32_t) ||
      !FrameCapturePolicy::CanAppend(events_.size(), bytes_used_,
                                     kEventMetadataBytes + register_count * sizeof(uint32_t))) {
    MarkFailure("journal_bounds_or_register_file");
    return {};
  }

  try {
    Event event;
    event.id = next_event_id_++;
    event.frame = frame;
    event.submission = submission;
    event.kind = EventKind::kDraw;
    event.primitive_type = primitive_type;
    event.index_count = index_count;
    event.major_mode_explicit = major_mode_explicit;
    event.index_info = index_info;
    event.marker = fmt::format("REXGLUE_FRAME_DRAW_{:016X}", event.id);
    if (!CopyRegisters(event, registers, register_count) ||
        !AddShader(uint32_t(kShaderTypeVertex), vertex_shader, event.vertex_shader_file) ||
        !AddShader(uint32_t(kShaderTypePixel), pixel_shader, event.pixel_shader_file)) {
      MarkFailure("journal_copy_failed");
      return {};
    }
    event.vertex_shader_hash = vertex_shader.present ? vertex_shader.hash : 0;
    event.pixel_shader_hash = pixel_shader.present ? pixel_shader.hash : 0;
    events_.push_back(std::move(event));
    bytes_used_ += kEventMetadataBytes + register_count * sizeof(uint32_t);
    return DrawScope(this, events_.back().id);
  } catch (const std::bad_alloc&) {
    MarkFailure("journal_allocation_failed");
    return {};
  }
}

FrameCapture::CopyScope FrameCapture::BeginCopyInternal(uint64_t frame, uint64_t submission,
                                                        const uint32_t* registers,
                                                        size_t register_count) {
  if (!IsActive() || !failure_reason_.empty()) {
    return {};
  }
  if (events_.size() >= FrameCapturePolicy::kMaxEvents) {
    MarkFailure("journal_bounds");
    return {};
  }
  if (register_count != kRegisterCountExpected || !registers ||
      !FrameCapturePolicy::CanAppend(events_.size(), bytes_used_,
                                     kEventMetadataBytes + register_count * sizeof(uint32_t))) {
    MarkFailure("journal_bounds_or_register_file");
    return {};
  }

  try {
    Event event;
    event.id = next_event_id_++;
    event.frame = frame;
    event.submission = submission;
    event.kind = EventKind::kCopy;
    event.marker = fmt::format("REXGLUE_FRAME_COPY_{:016X}", event.id);
    if (!CopyRegisters(event, registers, register_count)) {
      MarkFailure("journal_copy_failed");
      return {};
    }
    events_.push_back(std::move(event));
    bytes_used_ += kEventMetadataBytes + register_count * sizeof(uint32_t);
    return CopyScope(this, events_.back().id);
  } catch (const std::bad_alloc&) {
    MarkFailure("journal_allocation_failed");
    return {};
  }
}

std::string FrameCapture::RecordSwapEnd(uint64_t frame, uint64_t submission,
                                        uint64_t frontbuffer_ptr, uint32_t frontbuffer_width,
                                        uint32_t frontbuffer_height, const uint32_t* registers,
                                        size_t register_count) {
  if (!IsActive() || !failure_reason_.empty()) {
    return {};
  }
  if (pending_swap_event_id_ != 0) {
    MarkFailure("overlapping_swap_boundary");
    return {};
  }
  if (register_count != kRegisterCountExpected || !registers ||
      !FrameCapturePolicy::CanAppend(events_.size(), bytes_used_,
                                     kEventMetadataBytes + register_count * sizeof(uint32_t))) {
    MarkFailure("journal_bounds_or_register_file");
    return {};
  }

  try {
    Event event;
    event.id = next_event_id_++;
    event.frame = frame;
    event.submission = submission;
    event.kind = EventKind::kSwap;
    event.frontbuffer_ptr = frontbuffer_ptr;
    event.frontbuffer_width = frontbuffer_width;
    event.frontbuffer_height = frontbuffer_height;
    event.marker = fmt::format("REXGLUE_FRAME_SWAP_{:016X}", event.id);
    if (!CopyRegisters(event, registers, register_count)) {
      MarkFailure("journal_copy_failed");
      return {};
    }
    events_.push_back(std::move(event));
    bytes_used_ += kEventMetadataBytes + register_count * sizeof(uint32_t);
    pending_swap_event_id_ = events_.back().id;
    return events_.back().marker;
  } catch (const std::bad_alloc&) {
    MarkFailure("journal_allocation_failed");
    return {};
  }
}

bool FrameCapture::AppendOwnedStartSwap(uint64_t frame, uint64_t submission,
                                        uint64_t frontbuffer_ptr, uint32_t frontbuffer_width,
                                        uint32_t frontbuffer_height, const uint32_t* registers,
                                        size_t register_count) {
  if (RecordSwapEnd(frame, submission, frontbuffer_ptr, frontbuffer_width, frontbuffer_height,
                    registers, register_count)
          .empty() ||
      pending_swap_event_id_ == 0) {
    return false;
  }
  const auto it = std::find_if(events_.begin(), events_.end(), [this](const Event& event) {
    return event.id == pending_swap_event_id_;
  });
  if (it == events_.end()) {
    MarkFailure("owned_start_swap_missing");
    return false;
  }
  it->outcome = "completed";
  it->success = true;
  it->execution_status = diagnostic::DrawCaptureExecutionStatus::kIssued;
  pending_swap_event_id_ = 0;
  return true;
}

bool FrameCapture::BeginOwnedFrame(uint64_t frame, uint64_t submission, uint64_t frontbuffer_ptr,
                                   uint32_t frontbuffer_width, uint32_t frontbuffer_height,
                                   const uint32_t* registers, size_t register_count) {
  if (owned_active_ || state_ == State::kCapturing) {
    return false;
  }
  state_ = State::kIdle;
  output_path_.clear();
  renderdoc_capture_path_.clear();
  events_.clear();
  shaders_.clear();
  shader_indices_.clear();
  bytes_used_ = 0;
  next_event_id_ = 1;
  start_frame_ = frame;
  start_submission_ = submission;
  end_frame_ = 0;
  end_submission_ = 0;
  pending_swap_event_id_ = 0;
  failure_reason_.clear();
  renderdoc_started_ = false;
  renderdoc_device_ = nullptr;
  artifacts_written_ = false;
  owned_active_ = true;
  if (!AppendOwnedStartSwap(frame, submission, frontbuffer_ptr, frontbuffer_width,
                            frontbuffer_height, registers, register_count)) {
    owned_active_ = false;
    return false;
  }
  return true;
}

bool FrameCapture::BuildOwnedFrameSnapshot(uint64_t end_frame, uint64_t end_submission,
                                           diagnostic::DrawCaptureFrameSnapshot& snapshot) const {
  snapshot = {};
  snapshot.start_frame = start_frame_;
  snapshot.start_submission = start_submission_;
  snapshot.end_frame = end_frame;
  snapshot.end_submission = end_submission;
  snapshot.events.reserve(events_.size());
  for (const Event& event : events_) {
    diagnostic::DrawCaptureFrameEvent output;
    output.id = event.id;
    output.frame = event.frame;
    output.submission = event.submission;
    output.success = event.success;
    output.host_issued = event.host_issued;
    output.execution_status = event.execution_status;
    output.execution_reason = event.execution_reason;
    output.acquisition = event.acquisition;
    output.acquisition_reason = event.acquisition_reason;
    output.draw.draw.registers = event.registers;
    output.draw.draw.frame = event.frame;
    output.draw.draw.submission = event.submission;
    switch (event.kind) {
      case EventKind::kDraw: {
        output.kind = diagnostic::DrawCaptureFrameEvent::Kind::kDraw;
        diagnostic::DrawCaptureSnapshot& draw = output.draw.draw;
        draw.frame = event.frame;
        draw.submission = event.submission;
        output.draw.guest_vertex_shader_hash = event.vertex_shader_hash;
        output.draw.guest_pixel_shader_hash = event.pixel_shader_hash;
        output.draw.texture_bindings_complete = event.host_issued;
        output.draw.acquisition = event.acquisition;
        output.draw.acquisition_reason = event.acquisition_reason;
        draw.vertex_shader_hash = event.vertex_shader_hash;
        draw.pixel_shader_hash = event.pixel_shader_hash;
        draw.primitive_type = event.primitive_type;
        draw.requested_index_count = event.index_count;
        draw.major_mode_explicit = event.major_mode_explicit;
        draw.indexed = event.index_info.present;
        draw.guest_index_base = event.index_info.guest_base;
        draw.guest_index_size =
            event.index_info.length > UINT32_MAX ? 0 : uint32_t(event.index_info.length);
        draw.index.present = event.index_info.present;
        draw.index.guest_base = event.index_info.guest_base;
        draw.index.format = event.index_info.format;
        draw.index.endianness = event.index_info.endianness;
        draw.index.count = event.index_info.count;
        draw.index.dma_count = event.index_info.count;
        draw.index.dma_length =
            event.index_info.length > UINT32_MAX ? 0 : uint32_t(event.index_info.length);
        for (const ShaderRecord& shader : shaders_) {
          if (shader.file == event.vertex_shader_file) {
            draw.vertex_ucode = shader.dwords;
          }
          if (shader.file == event.pixel_shader_file) {
            draw.pixel_ucode = shader.dwords;
          }
        }
        if (event.host_issued) {
          const HostDrawInfo& host = event.host_info;
          draw.used_texture_mask = host.used_texture_mask;
          draw.normalized_color_mask = host.normalized_color_mask;
          draw.vertex_shader_hash = host.vertex_shader_hash;
          draw.pixel_shader_hash = host.pixel_shader_hash;
          draw.guest_primitive_type = host.guest_primitive_type;
          draw.host_primitive_type = host.host_primitive_type;
          draw.host_vertex_shader_type = host.host_vertex_shader_type;
          draw.tessellation_mode = host.tessellation_mode;
          draw.guest_draw_vertex_count = host.guest_draw_vertex_count;
          draw.host_draw_vertex_count = host.host_draw_vertex_count;
          draw.host_index_format = host.host_index_format;
          draw.host_shader_index_endian = host.host_shader_index_endian;
          draw.host_primitive_reset_enabled = host.host_primitive_reset_enabled;
          draw.color_target_written = host.normalized_color_mask != 0;
          draw.native_topology = host.native_topology;
          draw.native_vertex_count = host.native_vertex_count;
          draw.native_index_count = host.native_index_count;
          draw.native_instance_count = 1;
          draw.half_pixel_offset = host.half_pixel_offset;
        }
      } break;
      case EventKind::kCopy:
        output.kind = diagnostic::DrawCaptureFrameEvent::Kind::kCopy;
        output.copy_mode = event.copy_mode;
        break;
      case EventKind::kSwap:
        output.kind = diagnostic::DrawCaptureFrameEvent::Kind::kSwap;
        output.frontbuffer_ptr = event.frontbuffer_ptr;
        output.frontbuffer_width = event.frontbuffer_width;
        output.frontbuffer_height = event.frontbuffer_height;
        break;
    }
    const uint32_t output_severity = static_cast<uint32_t>(output.acquisition);
    const uint32_t snapshot_severity = static_cast<uint32_t>(snapshot.acquisition);
    if (output_severity < snapshot_severity ||
        (output_severity == snapshot_severity &&
         snapshot.acquisition_reason == diagnostic::DrawCaptureReason::kNone &&
         output.acquisition_reason != diagnostic::DrawCaptureReason::kNone)) {
      snapshot.acquisition = output.acquisition;
      snapshot.acquisition_reason = output.acquisition_reason;
    }
    snapshot.events.push_back(std::move(output));
  }
  return true;
}

std::shared_ptr<diagnostic::DrawCaptureFrameSnapshot> FrameCapture::TakeOwnedFrame(
    uint64_t end_frame, uint64_t end_submission) {
  if (!owned_active_ || !failure_reason_.empty()) {
    return {};
  }
  if (pending_swap_event_id_ != 0) {
    const auto it = std::find_if(events_.begin(), events_.end(), [this](const Event& event) {
      return event.id == pending_swap_event_id_;
    });
    if (it == events_.end()) {
      MarkFailure("owned_end_swap_missing");
      owned_active_ = false;
      return {};
    }
    it->outcome = "completed";
    it->success = true;
    it->execution_status = diagnostic::DrawCaptureExecutionStatus::kIssued;
    pending_swap_event_id_ = 0;
  }
  try {
    auto snapshot = std::make_shared<diagnostic::DrawCaptureFrameSnapshot>();
    if (!BuildOwnedFrameSnapshot(end_frame, end_submission, *snapshot)) {
      owned_active_ = false;
      return {};
    }
    owned_active_ = false;
    failure_reason_.clear();
    // The snapshot owns copies, so the journal can be released before the next capture.
    std::vector<Event>().swap(events_);
    bytes_used_ = 0;
    return snapshot;
  } catch (const std::bad_alloc&) {
    MarkFailure("owned_snapshot_allocation_failed");
    owned_active_ = false;
    return {};
  }
}

bool FrameCapture::CopyRegisters(Event& event, const uint32_t* registers, size_t register_count) {
  if (!registers || register_count != kRegisterCountExpected) {
    return false;
  }
  event.registers.assign(registers, registers + register_count);
  return true;
}

bool FrameCapture::AddShader(uint32_t type, const ShaderView& shader, std::string& file_out) {
  if (!shader.present) {
    return true;
  }
  if (!shader.dwords || !shader.dword_count ||
      shader.dword_count > FrameCapturePolicy::kMaxBytes / sizeof(uint32_t)) {
    return false;
  }
  const std::pair<uint32_t, uint64_t> key{type, shader.hash};
  const auto existing = shader_indices_.find(key);
  if (existing != shader_indices_.end()) {
    const ShaderRecord& record = shaders_[existing->second];
    if (record.dwords.size() != shader.dword_count ||
        std::memcmp(record.dwords.data(), shader.dwords, shader.dword_count * sizeof(uint32_t)) !=
            0) {
      MarkFailure("shader_hash_collision");
      return false;
    }
    file_out = record.file;
    return true;
  }
  const size_t shader_bytes = shader.dword_count * sizeof(uint32_t);
  if (!FrameCapturePolicy::CanAppend(events_.size(), bytes_used_, shader_bytes)) {
    MarkFailure("journal_shader_bounds");
    return false;
  }
  try {
    ShaderRecord record;
    record.type = type;
    record.hash = shader.hash;
    record.dwords.assign(shader.dwords, shader.dwords + shader.dword_count);
    record.file = fmt::format("shader_{}_{}.bin", type == uint32_t(kShaderTypeVertex) ? "vs" : "ps",
                              Hex(shader.hash));
    file_out = record.file;
    shader_indices_.emplace(key, shaders_.size());
    shaders_.push_back(std::move(record));
    bytes_used_ += shader_bytes;
    return true;
  } catch (const std::bad_alloc&) {
    return false;
  }
}

void FrameCapture::FinishDraw(uint64_t event_id, bool host_issued, bool copy, bool success,
                              const HostDrawInfo& host_info,
                              diagnostic::DrawCaptureReason execution_reason,
                              diagnostic::DrawCaptureAvailability acquisition,
                              diagnostic::DrawCaptureReason acquisition_reason) noexcept {
  try {
    const auto it = std::find_if(events_.begin(), events_.end(),
                                 [event_id](const Event& event) { return event.id == event_id; });
    if (it == events_.end() || it->kind != EventKind::kDraw) {
      MarkFailure("journal_event_missing");
      return;
    }
    it->host_issued = host_issued;
    it->success = success;
    it->execution_reason = execution_reason;
    it->acquisition = acquisition;
    it->acquisition_reason = acquisition_reason;
    it->execution_status = !success ? diagnostic::DrawCaptureExecutionStatus::kFailed
                           : host_issued || copy
                               ? diagnostic::DrawCaptureExecutionStatus::kIssued
                               : diagnostic::DrawCaptureExecutionStatus::kNoEffect;
    if (copy) {
      it->outcome = "copy";
    } else if (host_issued) {
      it->outcome = "issued";
      it->host_info = host_info;
    } else {
      it->outcome = "early_return";
    }
  } catch (...) {
    MarkFailure("journal_finish_failed");
  }
}

void FrameCapture::FinishCopy(uint64_t event_id, bool result_set, bool success,
                              std::string_view mode,
                              diagnostic::DrawCaptureAvailability acquisition,
                              diagnostic::DrawCaptureReason acquisition_reason) noexcept {
  try {
    const auto it = std::find_if(events_.begin(), events_.end(),
                                 [event_id](const Event& event) { return event.id == event_id; });
    if (it == events_.end() || it->kind != EventKind::kCopy) {
      MarkFailure("journal_event_missing");
      return;
    }
    it->success = result_set && success;
    it->execution_status = success ? diagnostic::DrawCaptureExecutionStatus::kIssued
                                   : diagnostic::DrawCaptureExecutionStatus::kFailed;
    it->execution_reason = success ? diagnostic::DrawCaptureReason::kNone
                                   : diagnostic::DrawCaptureReason::kExecutionFailure;
    it->acquisition = acquisition;
    it->acquisition_reason = acquisition_reason;
    it->outcome = result_set ? "completed" : "early_return";
    it->copy_mode.assign(mode);
  } catch (...) {
    MarkFailure("journal_finish_failed");
  }
}

void FrameCapture::MarkFailure(std::string_view reason) {
  if (failure_reason_.empty()) {
    failure_reason_.assign(reason);
  }
}

bool FrameCapture::PrepareOutput(const std::filesystem::path& path) {
  if (path.empty()) {
    MarkFailure("capture_path_empty");
    return false;
  }
  std::error_code error;
  if (!std::filesystem::is_directory(path, error) || error) {
    MarkFailure("capture_directory_missing");
    return false;
  }
  const std::filesystem::path absolute_path = std::filesystem::absolute(path, error);
  if (error || absolute_path.empty()) {
    MarkFailure("capture_directory_invalid");
    return false;
  }

  // The title driver owns arming and directory creation. A second run must not
  // replace a prior capture or its provenance files.
  std::filesystem::recursive_directory_iterator iterator(absolute_path, error);
  if (error) {
    MarkFailure("capture_directory_unreadable");
    return false;
  }
  for (const auto& entry : iterator) {
    if (error) {
      MarkFailure("capture_directory_unreadable");
      return false;
    }
    if (!entry.is_regular_file(error) || error) {
      error.clear();
      continue;
    }
    const std::string filename = entry.path().filename().string();
    std::string extension = entry.path().extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char character) { return char(std::tolower(character)); });
    if (extension == ".rdc" || filename == "manifest.json" || filename == "journal.json" ||
        filename == "manifest.json.tmp" || filename == "journal.json.tmp" ||
        filename.rfind("capture", 0) == 0 || filename.rfind("shader_", 0) == 0 ||
        filename.rfind("registers_", 0) == 0) {
      MarkFailure("capture_output_exists");
      return false;
    }
  }

  output_path_ = absolute_path;
  return true;
}

bool FrameCapture::AcquireRenderDoc() {
#if defined(_WIN32)
  if (renderdoc_api_) {
    return true;
  }
  // API1.0 contract: https://github.com/baldurk/renderdoc/blob/v1.46/docs/in_application_api.rst
  // The title driver injects RenderDoc before D3D12 device creation. Do not
  // load a DLL here: a missing injected module is a bounded capture failure.
  HMODULE module = GetModuleHandleW(L"renderdoc.dll");
  if (!module) {
    MarkFailure("renderdoc_unavailable");
    return false;
  }
  auto get_api = reinterpret_cast<pRENDERDOC_GetAPI>(GetProcAddress(module, "RENDERDOC_GetAPI"));
  if (!get_api) {
    MarkFailure("renderdoc_api_unavailable");
    return false;
  }
  void* api = nullptr;
  if (!get_api(eRENDERDOC_API_Version_1_0_1, &api) || !api) {
    MarkFailure("renderdoc_api_version_unavailable");
    return false;
  }
  renderdoc_api_ = api;
  auto* renderdoc = reinterpret_cast<RENDERDOC_API_1_0_1*>(renderdoc_api_);
  if (renderdoc->GetAPIVersion) {
    renderdoc->GetAPIVersion(&renderdoc_api_major_, &renderdoc_api_minor_, &renderdoc_api_patch_);
  }
  return true;
#else
  MarkFailure("renderdoc_unavailable");
  return false;
#endif
}

bool FrameCapture::StartRenderDoc(void* device) {
#if defined(_WIN32)
  if (!device || !AcquireRenderDoc()) {
    if (failure_reason_.empty()) {
      MarkFailure("renderdoc_device_unavailable");
    }
    return false;
  }
  auto* renderdoc = reinterpret_cast<RENDERDOC_API_1_0_1*>(renderdoc_api_);
  if (!renderdoc->IsFrameCapturing || renderdoc->IsFrameCapturing()) {
    MarkFailure("renderdoc_capture_overlap");
    return false;
  }
  if (!renderdoc->SetCaptureOptionU32 || !renderdoc->GetNumCaptures || !renderdoc->GetCapture ||
      !renderdoc->SetCaptureOptionU32(eRENDERDOC_Option_RefAllResources, 1) ||
      !renderdoc->SetCaptureOptionU32(eRENDERDOC_Option_SaveAllInitials, 1) ||
      !renderdoc->SetCaptureOptionU32(eRENDERDOC_Option_CaptureAllCmdLists, 1)) {
    MarkFailure("renderdoc_capture_options_failed");
    return false;
  }

  captures_before_ = renderdoc->GetNumCaptures ? renderdoc->GetNumCaptures() : 0;
  const std::filesystem::path template_path = output_path_ / "capture";
  const std::string template_utf8 = rex::path_to_utf8(template_path);
  if (!renderdoc->SetLogFilePathTemplate || template_utf8.empty()) {
    MarkFailure("renderdoc_capture_path_failed");
    return false;
  }
  renderdoc->SetLogFilePathTemplate(template_utf8.c_str());
  if (!renderdoc->StartFrameCapture || !renderdoc->IsFrameCapturing) {
    MarkFailure("renderdoc_capture_api_incomplete");
    return false;
  }
  renderdoc->StartFrameCapture(device, nullptr);
  if (!renderdoc->IsFrameCapturing()) {
    MarkFailure("renderdoc_capture_start_failed");
    return false;
  }
  renderdoc_started_ = true;
  renderdoc_device_ = device;
  return true;
#else
  (void)device;
  MarkFailure("renderdoc_unavailable");
  return false;
#endif
}

bool FrameCapture::EndRenderDoc() {
#if defined(_WIN32)
  if (!renderdoc_started_) {
    return true;
  }
  auto* renderdoc = reinterpret_cast<RENDERDOC_API_1_0_1*>(renderdoc_api_);
  if (!renderdoc || !renderdoc->EndFrameCapture) {
    MarkFailure("renderdoc_capture_api_incomplete");
    renderdoc_started_ = false;
    return false;
  }
  const bool ended = renderdoc->EndFrameCapture(renderdoc_device_, nullptr) != 0;
  renderdoc_started_ = false;
  renderdoc_device_ = nullptr;
  if (!ended) {
    MarkFailure("renderdoc_capture_end_failed");
    return false;
  }
  if (!GetRenderDocCapturePath(renderdoc_capture_path_)) {
    MarkFailure("renderdoc_capture_file_missing");
    return false;
  }
  return true;
#else
  MarkFailure("renderdoc_unavailable");
  return false;
#endif
}

bool FrameCapture::GetRenderDocCapturePath(std::filesystem::path& path_out) const {
#if defined(_WIN32)
  auto* renderdoc = reinterpret_cast<RENDERDOC_API_1_0_1*>(renderdoc_api_);
  if (!renderdoc || !renderdoc->GetNumCaptures || !renderdoc->GetCapture) {
    return false;
  }
  const uint32_t capture_count = renderdoc->GetNumCaptures();
  if (capture_count != captures_before_ + 1) {
    return false;
  }
  uint32_t path_length = 0;
  if (!renderdoc->GetCapture(capture_count - 1, nullptr, &path_length, nullptr) ||
      path_length == 0 || path_length > kMaxRenderDocPathBytes) {
    return false;
  }
  std::vector<char> path_buffer(size_t(path_length) + 1, '\0');
  uint32_t output_length = uint32_t(path_buffer.size());
  if (!renderdoc->GetCapture(capture_count - 1, path_buffer.data(), &output_length, nullptr)) {
    return false;
  }
  if (output_length >= path_buffer.size()) {
    if (output_length > kMaxRenderDocPathBytes) {
      return false;
    }
    path_buffer.assign(size_t(output_length) + 1, '\0');
    uint32_t retry_length = uint32_t(path_buffer.size());
    if (!renderdoc->GetCapture(capture_count - 1, path_buffer.data(), &retry_length, nullptr)) {
      return false;
    }
    output_length = retry_length;
  }
  size_t actual_length = std::min<size_t>(output_length, path_buffer.size() - 1);
  const auto terminator = std::find(path_buffer.begin(), path_buffer.end(), '\0');
  if (terminator != path_buffer.end()) {
    actual_length = std::min(actual_length, size_t(terminator - path_buffer.begin()));
  }
  if (!actual_length) {
    return false;
  }
  std::filesystem::path path = rex::to_path(std::string(path_buffer.data(), actual_length));
  std::error_code error;
  if (!std::filesystem::is_regular_file(path, error) || error) {
    return false;
  }
  const auto root = std::filesystem::weakly_canonical(output_path_, error);
  if (error) {
    return false;
  }
  const auto capture = std::filesystem::weakly_canonical(path, error);
  if (error) {
    return false;
  }
  const auto relative = std::filesystem::relative(capture, root, error);
  const std::string relative_generic = relative.generic_string();
  if (error || relative.empty() || relative_generic == "." || relative_generic == ".." ||
      relative_generic.rfind("../", 0) == 0) {
    return false;
  }
  path_out = capture;
  return true;
#else
  (void)path_out;
  return false;
#endif
}

void FrameCapture::OnCompletedSwap(void* device, const std::filesystem::path& requested_path,
                                   uint64_t completed_frame, uint64_t completed_submission) {
  if (owned_active_) {
    return;
  }
  if (state_ == State::kFinished || state_ == State::kFailed) {
    return;
  }
  if (state_ == State::kIdle) {
    if (requested_path.empty()) {
      return;
    }
    if (!PrepareOutput(requested_path) || !AcquireRenderDoc() || !StartRenderDoc(device)) {
      state_ = State::kFailed;
      REXGPU_ERROR("D3D12 guest frame RenderDoc capture did not start: {}",
                   failure_reason_.empty() ? std::string("unknown") : failure_reason_);
      WriteArtifacts(false);
      return;
    }
    start_frame_ = completed_frame;
    start_submission_ = completed_submission;
    REXGPU_INFO("D3D12 guest frame RenderDoc capture armed at frame {}, submission {}, path {}",
                start_frame_, start_submission_, rex::path_to_utf8(output_path_));
    state_ = State::kCapturing;
    return;
  }

  if (pending_swap_event_id_ == 0 && failure_reason_.empty()) {
    MarkFailure("swap_boundary_missing");
  }
  if (pending_swap_event_id_ != 0) {
    const auto it = std::find_if(events_.begin(), events_.end(), [this](const Event& event) {
      return event.id == pending_swap_event_id_;
    });
    if (it == events_.end()) {
      MarkFailure("swap_boundary_missing");
    } else {
      it->outcome = "completed";
      it->success = true;
      it->execution_status = diagnostic::DrawCaptureExecutionStatus::kIssued;
    }
    pending_swap_event_id_ = 0;
  }
  end_frame_ = completed_frame;
  end_submission_ = completed_submission;
  const bool renderdoc_ended = EndRenderDoc();
  const bool journal_complete = failure_reason_.empty() && pending_swap_event_id_ == 0;
  const bool complete = FrameCapturePolicy::IsComplete(
      true, renderdoc_ended, true, journal_complete, !renderdoc_capture_path_.empty());
  state_ = complete ? State::kFinished : State::kFailed;
  const bool artifacts_ok = WriteArtifacts(complete);
  if (complete && (!artifacts_ok || !failure_reason_.empty())) {
    if (failure_reason_.empty()) {
      MarkFailure("capture_artifact_write_failed");
    }
    state_ = State::kFailed;
  }
  const bool final_complete = complete && artifacts_ok && failure_reason_.empty();
  REXGPU_INFO("D3D12 guest frame RenderDoc capture {} at frame {}, events {}, path {}",
              final_complete ? "completed" : "failed", start_frame_, events_.size(),
              rex::path_to_utf8(output_path_));
}

void FrameCapture::Abort(void* device, std::string_view reason) {
  if (owned_active_) {
    MarkFailure(reason);
    owned_active_ = false;
    return;
  }
  if (state_ != State::kCapturing) {
    return;
  }
  MarkFailure(reason);
  if (!renderdoc_device_) {
    renderdoc_device_ = device;
  }
  EndRenderDoc();
  state_ = State::kFailed;
  WriteArtifacts(false);
}

bool FrameCapture::WriteNewFile(const std::filesystem::path& path, const std::string& bytes) const {
  std::error_code error;
  if (std::filesystem::exists(path, error) || error) {
    return false;
  }
  std::ofstream file(path, std::ios::binary | std::ios::out);
  if (!file) {
    return false;
  }
  file.write(bytes.data(), std::streamsize(bytes.size()));
  file.flush();
  const bool success = bool(file);
  file.close();
  return success && !file.fail();
}

bool FrameCapture::WriteDwords(const std::filesystem::path& path,
                               const std::vector<uint32_t>& dwords, bool guest_big_endian) const {
  std::error_code error;
  if (std::filesystem::exists(path, error) || error) {
    return false;
  }
  std::ofstream file(path, std::ios::binary | std::ios::out);
  if (!file) {
    return false;
  }
  for (uint32_t dword : dwords) {
    uint8_t bytes[sizeof(uint32_t)];
    if (guest_big_endian) {
      bytes[0] = uint8_t(dword >> 24);
      bytes[1] = uint8_t(dword >> 16);
      bytes[2] = uint8_t(dword >> 8);
      bytes[3] = uint8_t(dword);
    } else {
      bytes[0] = uint8_t(dword);
      bytes[1] = uint8_t(dword >> 8);
      bytes[2] = uint8_t(dword >> 16);
      bytes[3] = uint8_t(dword >> 24);
    }
    file.write(reinterpret_cast<const char*>(bytes), sizeof(bytes));
    if (!file) {
      file.close();
      return false;
    }
  }
  file.flush();
  const bool success = bool(file);
  file.close();
  return success && !file.fail();
}

bool FrameCapture::WriteJournal(std::vector<std::string>& files_out) {
  const auto add_file = [&files_out](const std::string& file) {
    if (std::find(files_out.begin(), files_out.end(), file) == files_out.end()) {
      files_out.push_back(file);
    }
  };

  for (Event& event : events_) {
    if (event.registers.empty()) {
      MarkFailure("journal_register_file_missing");
      return false;
    }
    if (event.registers_file.empty()) {
      event.registers_file = fmt::format("registers_{:016X}.bin", event.id);
    }
    add_file(event.registers_file);
    const std::filesystem::path registers_path = output_path_ / event.registers_file;
    if (!WriteDwords(registers_path, event.registers, false)) {
      // A previous partial attempt is itself an output collision and must not
      // be silently replaced.
      return false;
    }
  }

  std::string journal;
  journal +=
      "{\n  \"schema_version\":" + std::to_string(diagnostic::kDrawCaptureFrameSchemaVersion) +
      ",\n";
  journal += "  \"complete\":";
  journal += failure_reason_.empty() && pending_swap_event_id_ == 0 ? "true" : "false";
  journal += ",\n  \"frame\":" + std::to_string(start_frame_) +
             ",\n  \"start_submission\":" + std::to_string(start_submission_) +
             ",\n  \"end_frame\":" + std::to_string(end_frame_) +
             ",\n  \"end_submission\":" + std::to_string(end_submission_) + ",\n";
  journal += "  \"events\":[\n";
  for (size_t event_index = 0; event_index < events_.size(); ++event_index) {
    const Event& event = events_[event_index];
    if (event_index != 0) {
      journal += ",\n";
    }
    journal += "    {\"id\":" + std::to_string(event.id) + ",\"kind\":";
    AppendJsonString(journal, EventKindName(event.kind));
    journal += ",\"frame\":" + std::to_string(event.frame) +
               ",\"submission\":" + std::to_string(event.submission) + ",\"marker\":";
    AppendJsonString(journal, event.marker);
    journal += ",\"registers\":{\"file\":";
    AppendJsonString(journal, event.registers_file);
    journal += ",\"count\":" + std::to_string(event.registers.size()) +
               ",\"bytes\":" + std::to_string(event.registers.size() * sizeof(uint32_t)) +
               ",\"encoding\":\"little_endian_dwords\"}";

    if (event.kind == EventKind::kDraw) {
      journal +=
          ",\"original\":{\"primitive_type\":" + std::to_string(event.primitive_type) +
          ",\"index_count\":" + std::to_string(event.index_count) +
          ",\"major_mode_explicit\":" + std::string(event.major_mode_explicit ? "true" : "false") +
          ",\"index\":{";
      journal += "\"present\":" + std::string(event.index_info.present ? "true" : "false") +
                 ",\"format\":" + std::to_string(event.index_info.format) +
                 ",\"endianness\":" + std::to_string(event.index_info.endianness) +
                 ",\"count\":" + std::to_string(event.index_info.count) +
                 ",\"guest_base\":" + std::to_string(event.index_info.guest_base) +
                 ",\"length\":" + std::to_string(event.index_info.length) + "}}";
      journal += ",\"shaders\":{";
      journal += "\"vertex\":{";
      journal += "\"hash\":";
      AppendJsonString(journal, Hex(event.vertex_shader_hash));
      journal += ",\"file\":";
      if (event.vertex_shader_file.empty()) {
        journal += "null";
      } else {
        AppendJsonString(journal, event.vertex_shader_file);
      }
      journal += "},\"pixel\":{";
      journal += "\"hash\":";
      AppendJsonString(journal, Hex(event.pixel_shader_hash));
      journal += ",\"file\":";
      if (event.pixel_shader_file.empty()) {
        journal += "null";
      } else {
        AppendJsonString(journal, event.pixel_shader_file);
      }
      journal += "}}";
      journal += ",\"outcome\":{";
      journal += "\"kind\":";
      AppendJsonString(journal, event.outcome.empty() ? "journal_incomplete" : event.outcome);
      journal += ",\"success\":" + std::string(event.success ? "true" : "false") +
                 ",\"host_issued\":" + std::string(event.host_issued ? "true" : "false") +
                 ",\"execution_status\":";
      AppendJsonString(journal, ExecutionStatusName(event.execution_status));
      journal += ",\"execution_reason\":";
      AppendJsonString(journal, ReasonName(event.execution_reason));
      journal += ",\"acquisition\":";
      AppendJsonString(journal, AvailabilityName(event.acquisition));
      journal += ",\"acquisition_reason\":";
      AppendJsonString(journal, ReasonName(event.acquisition_reason));
      journal += "}";
      if (event.host_issued) {
        const HostDrawInfo& host = event.host_info;
        journal += ",\"host\":{";
        journal += "\"used_texture_mask\":" + std::to_string(host.used_texture_mask) +
                   ",\"vertex_shader_hash\":";
        AppendJsonString(journal, Hex(host.vertex_shader_hash));
        journal += ",\"pixel_shader_hash\":";
        AppendJsonString(journal, Hex(host.pixel_shader_hash));
        journal +=
            ",\"guest_primitive_type\":" + std::to_string(host.guest_primitive_type) +
            ",\"host_primitive_type\":" + std::to_string(host.host_primitive_type) +
            ",\"host_vertex_shader_type\":" + std::to_string(host.host_vertex_shader_type) +
            ",\"tessellation_mode\":" + std::to_string(host.tessellation_mode) +
            ",\"guest_draw_vertex_count\":" + std::to_string(host.guest_draw_vertex_count) +
            ",\"host_draw_vertex_count\":" + std::to_string(host.host_draw_vertex_count) +
            ",\"line_loop_closing_index\":" + std::to_string(host.line_loop_closing_index) +
            ",\"index_buffer_type\":" + std::to_string(host.index_buffer_type) +
            ",\"guest_index_base\":" + std::to_string(host.guest_index_base) +
            ",\"host_index_format\":" + std::to_string(host.host_index_format) +
            ",\"host_shader_index_endian\":" + std::to_string(host.host_shader_index_endian) +
            ",\"host_primitive_reset_enabled\":" +
            std::string(host.host_primitive_reset_enabled ? "true" : "false") +
            ",\"native_topology\":" + std::to_string(host.native_topology) + "}";
      }
    } else if (event.kind == EventKind::kCopy) {
      journal += ",\"outcome\":{";
      journal += "\"kind\":";
      AppendJsonString(journal, event.outcome.empty() ? "journal_incomplete" : event.outcome);
      journal += ",\"success\":" + std::string(event.success ? "true" : "false") +
                 ",\"execution_status\":";
      AppendJsonString(journal, ExecutionStatusName(event.execution_status));
      journal += ",\"execution_reason\":";
      AppendJsonString(journal, ReasonName(event.execution_reason));
      journal += ",\"acquisition\":";
      AppendJsonString(journal, AvailabilityName(event.acquisition));
      journal += ",\"acquisition_reason\":";
      AppendJsonString(journal, ReasonName(event.acquisition_reason));
      journal += ",\"mode\":";
      AppendJsonString(journal, event.copy_mode);
      journal += "}";
    } else {
      journal += ",\"frontbuffer\":{";
      journal += "\"ptr\":";
      AppendJsonString(journal, fmt::format("0x{:08X}", event.frontbuffer_ptr));
      journal += ",\"width\":" + std::to_string(event.frontbuffer_width) +
                 ",\"height\":" + std::to_string(event.frontbuffer_height) + "}";
      journal += ",\"outcome\":{";
      journal += "\"kind\":";
      AppendJsonString(journal, event.outcome.empty() ? "journal_incomplete" : event.outcome);
      journal += ",\"success\":" + std::string(event.success ? "true" : "false") +
                 ",\"execution_status\":";
      AppendJsonString(journal, ExecutionStatusName(event.execution_status));
      journal += ",\"execution_reason\":";
      AppendJsonString(journal, ReasonName(event.execution_reason));
      journal += ",\"acquisition\":";
      AppendJsonString(journal, AvailabilityName(event.acquisition));
      journal += ",\"acquisition_reason\":";
      AppendJsonString(journal, ReasonName(event.acquisition_reason));
      journal += "}";
    }
    journal += "}";
  }
  journal += "\n  ]\n}\n";
  if (!WriteNewFile(output_path_ / "journal.json", journal)) {
    return false;
  }
  files_out.push_back("journal.json");
  return true;
}

bool FrameCapture::WriteManifest(bool complete, const std::vector<std::string>& files) {
  std::string manifest;
  manifest += "{\n  \"schema_version\":1,\n  \"status\":";
  AppendJsonString(manifest, complete ? "complete" : "failed");
  manifest += ",\n  \"valid\":" + std::string(complete ? "true" : "false") + ",\n  \"completion\":";
  AppendJsonString(manifest, complete ? "guest_frame_and_renderdoc" : "failed");
  manifest += ",\n";
  manifest += "  \"failure_reason\":";
  if (failure_reason_.empty()) {
    manifest += "null";
  } else {
    AppendJsonString(manifest, failure_reason_);
  }
  manifest += ",\n  \"frame\":" + std::to_string(start_frame_) +
              ",\n  \"start_submission\":" + std::to_string(start_submission_) +
              ",\n  \"end_frame\":" + std::to_string(end_frame_) +
              ",\n  \"end_submission\":" + std::to_string(end_submission_) +
              ",\n  \"event_count\":" + std::to_string(events_.size()) +
              ",\n  \"journal_bytes\":" + std::to_string(bytes_used_) + ",\n";
  manifest += "  \"bounds\":{";
  manifest += "\"max_events\":" + std::to_string(FrameCapturePolicy::kMaxEvents) +
              ",\"max_bytes\":" + std::to_string(FrameCapturePolicy::kMaxBytes) + "},\n";
  manifest += "  \"renderdoc\":{";
  manifest += "\"api_major\":" + std::to_string(renderdoc_api_major_) +
              ",\"api_minor\":" + std::to_string(renderdoc_api_minor_) +
              ",\"api_patch\":" + std::to_string(renderdoc_api_patch_) + ",\"capture_path\":";
  if (renderdoc_capture_path_.empty()) {
    manifest += "null";
  } else {
    AppendJsonString(manifest, rex::path_to_utf8(renderdoc_capture_path_));
  }
  manifest += "},\n  \"files\":[\n";
  for (size_t i = 0; i < files.size(); ++i) {
    if (i != 0) {
      manifest += ",\n";
    }
    const std::filesystem::path path = output_path_ / files[i];
    std::error_code error;
    const uint64_t size = std::filesystem::file_size(path, error);
    if (error) {
      return false;
    }
    std::string extension = path.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char character) { return char(std::tolower(character)); });
    const std::string kind = files[i] == "journal.json"
                                 ? "journal"
                                 : (extension == ".rdc"                 ? "renderdoc"
                                    : files[i].rfind("shader_", 0) == 0 ? "shader"
                                                                        : "registers");
    manifest += "    {\"path\":";
    AppendJsonString(manifest, files[i]);
    manifest += ",\"kind\":";
    AppendJsonString(manifest, kind);
    manifest += ",\"size\":" + std::to_string(size) + "}";
  }
  manifest += "\n  ]\n}\n";
  return WriteNewFile(output_path_ / "manifest.json", manifest);
}

bool FrameCapture::WriteArtifacts(bool complete) {
  if (artifacts_written_ || output_path_.empty()) {
    return artifacts_written_;
  }
  std::vector<std::string> files;
  const auto add_file = [&files](const std::string& file) {
    if (std::find(files.begin(), files.end(), file) == files.end()) {
      files.push_back(file);
    }
  };

  if (complete) {
    std::error_code error;
    if (renderdoc_capture_path_.empty() ||
        !std::filesystem::is_regular_file(renderdoc_capture_path_, error) || error) {
      MarkFailure("renderdoc_capture_file_missing");
      complete = false;
    }
  }

  for (ShaderRecord& shader : shaders_) {
    const std::filesystem::path path = output_path_ / shader.file;
    if (!WriteDwords(path, shader.dwords, true)) {
      MarkFailure("shader_file_write_failed");
      complete = false;
      break;
    }
    add_file(shader.file);
  }
  if (!WriteJournal(files)) {
    MarkFailure("journal_file_write_failed");
    complete = false;
  }
  if (!renderdoc_capture_path_.empty()) {
    std::error_code error;
    if (std::filesystem::is_regular_file(renderdoc_capture_path_, error) && !error) {
      const auto root = std::filesystem::weakly_canonical(output_path_, error);
      const auto relative = std::filesystem::relative(renderdoc_capture_path_, root, error);
      const std::string relative_generic = relative.generic_string();
      if (!error && !relative.empty() && relative_generic != "." && relative_generic != ".." &&
          relative_generic.rfind("../", 0) != 0) {
        add_file(relative.generic_string());
      } else if (complete) {
        MarkFailure("renderdoc_capture_path_invalid");
        complete = false;
      }
    } else if (complete) {
      MarkFailure("renderdoc_capture_file_missing");
      complete = false;
    }
  }
  const bool manifest_complete =
      complete && failure_reason_.empty() &&
      FrameCapturePolicy::IsComplete(true, !renderdoc_started_, pending_swap_event_id_ == 0, true,
                                     !renderdoc_capture_path_.empty());
  if (!WriteManifest(manifest_complete, files)) {
    REXGPU_ERROR("Failed to publish D3D12 guest frame capture manifest at {}",
                 rex::path_to_utf8(output_path_));
    return false;
  }
  artifacts_written_ = true;
  return true;
}

}  // namespace rex::graphics::d3d12
