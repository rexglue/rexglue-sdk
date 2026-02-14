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
#include <memory>

#include <rex/graphics/trace_writer.h>

// TODO(tomc): Enable on other platforms once the RTTI linking issue is resolved
#ifdef _WIN32
#define REX_TRACE_USE_SNAPPY 1
#include "snappy-sinksource.h"
#include "snappy.h"
#endif

#include <rex/assert.h>
#include <rex/filesystem.h>
#include <rex/graphics/registers.h>
#include <rex/graphics/xenos.h>
#include <rex/logging.h>

namespace rex::graphics {

TraceWriter::TraceWriter(uint8_t* membase) : membase_(membase), file_(nullptr) {}

TraceWriter::~TraceWriter() = default;

bool TraceWriter::Open(const std::filesystem::path& path, uint32_t title_id) {
  Close();

  auto canonical_path = std::filesystem::absolute(path);
  if (canonical_path.has_parent_path()) {
    auto base_path = canonical_path.parent_path();
    std::filesystem::create_directories(base_path);
  }

#ifdef _WIN32
  file_ = _wfopen(canonical_path.c_str(), L"wb");
#else
  file_ = std::fopen(canonical_path.c_str(), "wb");
#endif
  if (!file_) {
    REXGPU_ERROR("TraceWriter: Failed to open trace file: {}", canonical_path.string());
    return false;
  }

  REXGPU_INFO("TraceWriter: Opened trace file: {}", canonical_path.string());

  // Write header first. Must be at the top of the file.
  TraceHeader header;
  header.version = kTraceFormatVersion;
  // Use a static commit string for rexglue
  std::memset(header.build_commit_sha, 0, sizeof(header.build_commit_sha));
  std::strncpy(header.build_commit_sha, "rexglue-dev", sizeof(header.build_commit_sha) - 1);
  header.title_id = title_id;
  fwrite(&header, sizeof(header), 1, file_);

  cached_memory_reads_.clear();
  return true;
}

void TraceWriter::Flush() {
  if (file_) {
    fflush(file_);
  }
}

void TraceWriter::Close() {
  if (file_) {
    cached_memory_reads_.clear();

    fflush(file_);
    fclose(file_);
    file_ = nullptr;
    REXGPU_INFO("TraceWriter: Closed trace file");
  }
}

void TraceWriter::WritePrimaryBufferStart(uint32_t base_ptr, uint32_t count) {
  if (!file_) {
    return;
  }
  PrimaryBufferStartCommand cmd = {
      TraceCommandType::kPrimaryBufferStart,
      base_ptr,
      0,
  };
  fwrite(&cmd, 1, sizeof(cmd), file_);
}

void TraceWriter::WritePrimaryBufferEnd() {
  if (!file_) {
    return;
  }
  PrimaryBufferEndCommand cmd = {
      TraceCommandType::kPrimaryBufferEnd,
  };
  fwrite(&cmd, 1, sizeof(cmd), file_);
}

void TraceWriter::WriteIndirectBufferStart(uint32_t base_ptr, uint32_t count) {
  if (!file_) {
    return;
  }
  IndirectBufferStartCommand cmd = {
      TraceCommandType::kIndirectBufferStart,
      base_ptr,
      0,
  };
  fwrite(&cmd, 1, sizeof(cmd), file_);
}

void TraceWriter::WriteIndirectBufferEnd() {
  if (!file_) {
    return;
  }
  IndirectBufferEndCommand cmd = {
      TraceCommandType::kIndirectBufferEnd,
  };
  fwrite(&cmd, 1, sizeof(cmd), file_);
}

void TraceWriter::WritePacketStart(uint32_t base_ptr, uint32_t count) {
  if (!file_) {
    return;
  }
  PacketStartCommand cmd = {
      TraceCommandType::kPacketStart,
      base_ptr,
      count,
  };
  fwrite(&cmd, 1, sizeof(cmd), file_);
  fwrite(membase_ + base_ptr, 4, count, file_);
}

void TraceWriter::WritePacketEnd() {
  if (!file_) {
    return;
  }
  PacketEndCommand cmd = {
      TraceCommandType::kPacketEnd,
  };
  fwrite(&cmd, 1, sizeof(cmd), file_);
}

void TraceWriter::WriteMemoryRead(uint32_t base_ptr, size_t length, const void* host_ptr) {
  if (!file_) {
    return;
  }
  WriteMemoryCommand(TraceCommandType::kMemoryRead, base_ptr, length, host_ptr);
}

void TraceWriter::WriteMemoryReadCached(uint32_t base_ptr, size_t length) {
  if (!file_) {
    return;
  }

  // HACK: length is guaranteed to be within 32-bits (guest memory)
  uint64_t key = uint64_t(base_ptr) << 32 | uint64_t(length);
  if (cached_memory_reads_.find(key) == cached_memory_reads_.end()) {
    WriteMemoryCommand(TraceCommandType::kMemoryRead, base_ptr, length);
    cached_memory_reads_.insert(key);
  }
}

void TraceWriter::WriteMemoryReadCachedNop(uint32_t base_ptr, size_t length) {
  if (!file_) {
    return;
  }

  // HACK: length is guaranteed to be within 32-bits (guest memory)
  uint64_t key = uint64_t(base_ptr) << 32 | uint64_t(length);
  if (cached_memory_reads_.find(key) == cached_memory_reads_.end()) {
    cached_memory_reads_.insert(key);
  }
}

void TraceWriter::WriteMemoryWrite(uint32_t base_ptr, size_t length, const void* host_ptr) {
  if (!file_) {
    return;
  }
  WriteMemoryCommand(TraceCommandType::kMemoryWrite, base_ptr, length, host_ptr);
}

#if REX_TRACE_USE_SNAPPY
class SnappySink : public snappy::Sink {
 public:
  SnappySink(FILE* file) : file_(file) {}

  void Append(const char* bytes, size_t n) override { fwrite(bytes, 1, n, file_); }

 private:
  FILE* file_ = nullptr;
};
#endif

void TraceWriter::WriteMemoryCommand(TraceCommandType type, uint32_t base_ptr, size_t length,
                                     const void* host_ptr) {
  MemoryCommand cmd = {};
  cmd.type = type;
  cmd.base_ptr = base_ptr;
  cmd.encoding_format = MemoryEncodingFormat::kNone;
  cmd.encoded_length = cmd.decoded_length = static_cast<uint32_t>(length);

  if (!host_ptr) {
    host_ptr = membase_ + cmd.base_ptr;
  }

#if REX_TRACE_USE_SNAPPY
  bool compress = compress_output_ && length > compression_threshold_;
  if (compress) {
    // Write the header now so we reserve space in the buffer.
    long header_position = std::ftell(file_);
    cmd.encoding_format = MemoryEncodingFormat::kSnappy;
    fwrite(&cmd, 1, sizeof(cmd), file_);

    // Stream the content right to the buffer.
    snappy::ByteArraySource snappy_source(reinterpret_cast<const char*>(host_ptr),
                                          cmd.decoded_length);
    SnappySink snappy_sink(file_);
    cmd.encoded_length = static_cast<uint32_t>(snappy::Compress(&snappy_source, &snappy_sink));

    // Seek back and overwrite the header with our final size.
    std::fseek(file_, header_position, SEEK_SET);
    fwrite(&cmd, 1, sizeof(cmd), file_);
    std::fseek(file_, header_position + sizeof(cmd) + cmd.encoded_length, SEEK_SET);
  } else
#endif
  {
    // Uncompressed - write buffer directly to the file.
    cmd.encoding_format = MemoryEncodingFormat::kNone;
    fwrite(&cmd, 1, sizeof(cmd), file_);
    fwrite(host_ptr, 1, cmd.decoded_length, file_);
  }
}

void TraceWriter::WriteEdramSnapshot(const void* snapshot) {
  EdramSnapshotCommand cmd = {};
  cmd.type = TraceCommandType::kEdramSnapshot;

#if REX_TRACE_USE_SNAPPY
  if (compress_output_) {
    // Write the header now so we reserve space in the buffer.
    long header_position = std::ftell(file_);
    cmd.encoding_format = MemoryEncodingFormat::kSnappy;
    fwrite(&cmd, 1, sizeof(cmd), file_);

    // Stream the content right to the buffer.
    snappy::ByteArraySource snappy_source(reinterpret_cast<const char*>(snapshot),
                                          xenos::kEdramSizeBytes);
    SnappySink snappy_sink(file_);
    cmd.encoded_length = static_cast<uint32_t>(snappy::Compress(&snappy_source, &snappy_sink));

    // Seek back and overwrite the header with our final size.
    std::fseek(file_, header_position, SEEK_SET);
    fwrite(&cmd, 1, sizeof(cmd), file_);
    std::fseek(file_, header_position + sizeof(cmd) + cmd.encoded_length, SEEK_SET);
  } else
#endif
  {
    // Uncompressed - write buffer directly to the file.
    cmd.encoding_format = MemoryEncodingFormat::kNone;
    cmd.encoded_length = xenos::kEdramSizeBytes;
    fwrite(&cmd, 1, sizeof(cmd), file_);
    fwrite(snapshot, 1, xenos::kEdramSizeBytes, file_);
  }
}

void TraceWriter::WriteEvent(EventCommand::Type event_type) {
  if (!file_) {
    return;
  }
  EventCommand cmd = {
      TraceCommandType::kEvent,
      event_type,
  };
  fwrite(&cmd, 1, sizeof(cmd), file_);
}

void TraceWriter::WriteRegisters(uint32_t first_register, const uint32_t* register_values,
                                 uint32_t register_count, bool execute_callbacks_on_play) {
  RegistersCommand cmd = {};
  cmd.type = TraceCommandType::kRegisters;
  cmd.first_register = first_register;
  cmd.register_count = register_count;
  cmd.execute_callbacks = execute_callbacks_on_play;

  uint32_t uncompressed_length = uint32_t(sizeof(uint32_t) * register_count);
#if REX_TRACE_USE_SNAPPY
  if (compress_output_) {
    // Write the header now so we reserve space in the buffer.
    long header_position = std::ftell(file_);
    cmd.encoding_format = MemoryEncodingFormat::kSnappy;
    fwrite(&cmd, 1, sizeof(cmd), file_);

    // Stream the content right to the buffer.
    snappy::ByteArraySource snappy_source(reinterpret_cast<const char*>(register_values),
                                          uncompressed_length);
    SnappySink snappy_sink(file_);
    cmd.encoded_length = static_cast<uint32_t>(snappy::Compress(&snappy_source, &snappy_sink));

    // Seek back and overwrite the header with our final size.
    std::fseek(file_, header_position, SEEK_SET);
    fwrite(&cmd, 1, sizeof(cmd), file_);
    std::fseek(file_, header_position + sizeof(cmd) + cmd.encoded_length, SEEK_SET);
  } else
#endif
  {
    // Uncompressed - write the values directly to the file.
    cmd.encoding_format = MemoryEncodingFormat::kNone;
    cmd.encoded_length = uncompressed_length;
    fwrite(&cmd, 1, sizeof(cmd), file_);
    fwrite(register_values, 1, uncompressed_length, file_);
  }
}

void TraceWriter::WriteGammaRamp(const reg::DC_LUT_30_COLOR* gamma_ramp_256_entry_table,
                                 const reg::DC_LUT_PWL_DATA* gamma_ramp_pwl_rgb,
                                 uint32_t gamma_ramp_rw_component) {
  GammaRampCommand cmd = {};
  cmd.type = TraceCommandType::kGammaRamp;
  cmd.rw_component = uint8_t(gamma_ramp_rw_component);

  constexpr uint32_t k256EntryTableUncompressedLength = sizeof(reg::DC_LUT_30_COLOR) * 256;
  constexpr uint32_t kPWLUncompressedLength = sizeof(reg::DC_LUT_PWL_DATA) * 3 * 128;
  constexpr uint32_t kUncompressedLength =
      k256EntryTableUncompressedLength + kPWLUncompressedLength;
#if REX_TRACE_USE_SNAPPY
  if (compress_output_) {
    // Write the header now so we reserve space in the buffer.
    long header_position = std::ftell(file_);
    cmd.encoding_format = MemoryEncodingFormat::kSnappy;
    fwrite(&cmd, 1, sizeof(cmd), file_);

    // Stream the content right to the buffer.
    {
      std::unique_ptr<char[]> gamma_ramps(new char[kUncompressedLength]);
      std::memcpy(gamma_ramps.get(), gamma_ramp_256_entry_table, k256EntryTableUncompressedLength);
      std::memcpy(gamma_ramps.get() + k256EntryTableUncompressedLength, gamma_ramp_pwl_rgb,
                  kPWLUncompressedLength);
      snappy::ByteArraySource snappy_source(gamma_ramps.get(), kUncompressedLength);
      SnappySink snappy_sink(file_);
      cmd.encoded_length = static_cast<uint32_t>(snappy::Compress(&snappy_source, &snappy_sink));
    }

    // Seek back and overwrite the header with our final size.
    std::fseek(file_, header_position, SEEK_SET);
    fwrite(&cmd, 1, sizeof(cmd), file_);
    std::fseek(file_, header_position + sizeof(cmd) + cmd.encoded_length, SEEK_SET);
  } else
#endif
  {
    // Uncompressed - write the values directly to the file.
    cmd.encoding_format = MemoryEncodingFormat::kNone;
    cmd.encoded_length = kUncompressedLength;
    fwrite(&cmd, 1, sizeof(cmd), file_);
    fwrite(gamma_ramp_256_entry_table, 1, k256EntryTableUncompressedLength, file_);
    fwrite(gamma_ramp_pwl_rgb, 1, kPWLUncompressedLength, file_);
  }
}

}  // namespace rex::graphics
