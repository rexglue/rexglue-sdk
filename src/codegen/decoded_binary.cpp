#include "decoded_binary.h"

#include <algorithm>

#include <rex/memory/byte_swap.h>
#include <rex/types.h>

namespace rex::codegen {

DecodedBinary::DecodedBinary(const BinaryView& binary) : binary_(binary) {}

void DecodedBinary::decode() {
  sections_.clear();
  codeRegions_.clear();

  for (const auto& section : binary_.sections()) {
    Section sec;
    sec.base = section.baseAddress;
    sec.size = section.size;

    sec.data.assign(section.data, section.data + section.size);

    if (section.executable) {
      size_t insnCount = section.size / 4;
      sec.instructions.reserve(insnCount);

      for (u32 offset = 0; offset < section.size; offset += 4) {
        u32 addr = section.baseAddress + offset;
        u32 raw = memory::load<be_u32>(section.data + offset);

        auto insn = rex::codegen::detail::decode_instruction(addr, raw);
        sec.instructions.push_back(insn);
      }
    }

    sections_.push_back(std::move(sec));
  }

  std::sort(sections_.begin(), sections_.end(),
            [](const Section& a, const Section& b) { return a.base < b.base; });

  computeCodeRegions();
}

DecodedInsn* DecodedBinary::get(u32 addr) {
  Section* sec = findSection(addr);
  return sec ? sec->get(addr) : nullptr;
}

const DecodedInsn* DecodedBinary::get(u32 addr) const {
  const Section* sec = findSection(addr);
  return sec ? sec->get(addr) : nullptr;
}

InsnRange DecodedBinary::range(u32 start, u32 end) {
  Section* sec = findSection(start);
  if (!sec || !sec->contains(end - 4)) {
    return InsnRange(nullptr, nullptr);
  }

  u32 startIdx = (start - sec->base) / 4;
  u32 endIdx = (end - sec->base) / 4;

  endIdx = std::min(endIdx, static_cast<u32>(sec->instructions.size()));

  return InsnRange(&sec->instructions[startIdx], &sec->instructions[endIdx]);
}

const u8* DecodedBinary::rawData(u32 addr, size_t len) const {
  const Section* sec = findSection(addr);
  if (!sec)
    return nullptr;

  u32 offset = addr - sec->base;
  if (offset + len > sec->data.size())
    return nullptr;

  return sec->data.data() + offset;
}

const CodeRegion* DecodedBinary::regionContaining(u32 addr) const {
  for (const auto& region : codeRegions_) {
    if (region.contains(addr))
      return &region;
  }
  return nullptr;
}

size_t DecodedBinary::instructionCount() const {
  size_t count = 0;
  for (const auto& sec : sections_) {
    count += sec.instructions.size();
  }
  return count;
}

DecodedBinary::Section* DecodedBinary::findSection(u32 addr) {
  for (auto& sec : sections_) {
    if (sec.contains(addr))
      return &sec;
  }
  return nullptr;
}

const DecodedBinary::Section* DecodedBinary::findSection(u32 addr) const {
  for (const auto& sec : sections_) {
    if (sec.contains(addr))
      return &sec;
  }
  return nullptr;
}

void DecodedBinary::computeCodeRegions() {
  codeRegions_.clear();

  constexpr size_t kMinNullRun = 2;

  for (const auto& sec : sections_) {
    if (sec.instructions.empty())
      continue;

    CodeRegion current;
    current.start = sec.base;
    size_t nullRun = 0;
    bool inCode = false;

    for (size_t i = 0; i < sec.instructions.size(); i++) {
      const auto& insn = sec.instructions[i];
      bool isNull = isInvalid(insn);

      if (isNull) {
        nullRun++;
        if (inCode && nullRun >= kMinNullRun) {
          current.end = sec.base + static_cast<u32>((i - nullRun + 1) * 4);
          if (current.end > current.start) {
            codeRegions_.push_back(current);
          }
          inCode = false;
        }
      } else {
        if (!inCode) {
          current.start = sec.base + static_cast<u32>(i * 4);
          inCode = true;
        }
        nullRun = 0;
      }
    }

    if (inCode) {
      current.end = sec.base + sec.size;
      if (current.end > current.start) {
        codeRegions_.push_back(current);
      }
    }
  }
}

}  // namespace rex::codegen
