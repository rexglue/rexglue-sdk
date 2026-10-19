#include "decoded_binary.h"

#include <algorithm>

#include <rex/memory/byte_swap.h>
#include <rex/types.h>

namespace rex::codegen {

DecodedBinary::DecodedBinary(const BinaryView& binary) : binary_(binary) {}

void DecodedBinary::Decode() {
  sections_.clear();
  code_regions_.clear();

  for (const auto& section : binary_.GetSections()) {
    Section sec;
    sec.base = section.base_address;
    sec.size = section.size;

    sec.data.assign(section.data, section.data + section.size);

    if (section.executable) {
      size_t insn_count = section.size / 4;
      sec.instructions.reserve(insn_count);

      for (u32 offset = 0; offset < section.size; offset += 4) {
        u32 addr = section.base_address + offset;
        u32 raw = memory::load<be_u32>(section.data + offset);

        auto insn = rex::codegen::detail::DecodeInstruction(addr, raw);
        sec.instructions.push_back(insn);
      }
    }

    sections_.push_back(std::move(sec));
  }

  std::sort(sections_.begin(), sections_.end(),
            [](const Section& a, const Section& b) { return a.base < b.base; });

  ComputeCodeRegions();
}

DecodedInsn* DecodedBinary::FindInsn(u32 addr) {
  Section* sec = FindSection(addr);
  return sec ? sec->FindInsn(addr) : nullptr;
}

const DecodedInsn* DecodedBinary::FindInsn(u32 addr) const {
  const Section* sec = FindSection(addr);
  return sec ? sec->FindInsn(addr) : nullptr;
}

InsnRange DecodedBinary::GetRange(u32 start, u32 end) {
  Section* sec = FindSection(start);
  if (!sec || !sec->Contains(end - 4)) {
    return InsnRange(nullptr, nullptr);
  }

  u32 start_idx = (start - sec->base) / 4;
  u32 end_idx = (end - sec->base) / 4;

  end_idx = std::min(end_idx, static_cast<u32>(sec->instructions.size()));

  return InsnRange(&sec->instructions[start_idx], &sec->instructions[end_idx]);
}

const u8* DecodedBinary::GetRawData(u32 addr, size_t len) const {
  const Section* sec = FindSection(addr);
  if (!sec)
    return nullptr;

  u32 offset = addr - sec->base;
  if (offset + len > sec->data.size())
    return nullptr;

  return sec->data.data() + offset;
}

const CodeRegion* DecodedBinary::FindRegionContaining(u32 addr) const {
  for (const auto& region : code_regions_) {
    if (region.Contains(addr))
      return &region;
  }
  return nullptr;
}

size_t DecodedBinary::GetInstructionCount() const {
  size_t count = 0;
  for (const auto& sec : sections_) {
    count += sec.instructions.size();
  }
  return count;
}

DecodedBinary::Section* DecodedBinary::FindSection(u32 addr) {
  for (auto& sec : sections_) {
    if (sec.Contains(addr))
      return &sec;
  }
  return nullptr;
}

const DecodedBinary::Section* DecodedBinary::FindSection(u32 addr) const {
  for (const auto& sec : sections_) {
    if (sec.Contains(addr))
      return &sec;
  }
  return nullptr;
}

void DecodedBinary::ComputeCodeRegions() {
  code_regions_.clear();

  constexpr size_t kMinNullRun = 2;

  for (const auto& sec : sections_) {
    if (sec.instructions.empty())
      continue;

    CodeRegion current;
    current.start = sec.base;
    size_t null_run = 0;
    bool in_code = false;

    for (size_t i = 0; i < sec.instructions.size(); i++) {
      const auto& insn = sec.instructions[i];
      bool is_invalid = IsInvalid(insn);

      if (is_invalid) {
        null_run++;
        if (in_code && null_run >= kMinNullRun) {
          current.end = sec.base + static_cast<u32>((i - null_run + 1) * 4);
          if (current.end > current.start) {
            code_regions_.push_back(current);
          }
          in_code = false;
        }
      } else {
        if (!in_code) {
          current.start = sec.base + static_cast<u32>(i * 4);
          in_code = true;
        }
        null_run = 0;
      }
    }

    if (in_code) {
      current.end = sec.base + sec.size;
      if (current.end > current.start) {
        code_regions_.push_back(current);
      }
    }
  }
}

}  // namespace rex::codegen
