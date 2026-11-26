/**
 * @file        rex/graphics/util/shader_constant_overrides.h
 *
 * @brief       Per-shader float constant overrides (graphics enhancements without shader
 *              replacement).
 *
 * A guest pixel shader, identified by its ucode hash, gets selected float constant components
 * multiplied or replaced when its constant buffer is built. The guest register file is not
 * touched and the translated shader is unchanged, so pipelines, shader caches and anything that
 * hooks translated shaders by their bytecode (such as ReShade addons) are unaffected.
 *
 * Specification (cvar `pixel_shader_constant_overrides`):
 *
 *   spec    := entry (';' entry)*
 *   entry   := HASH ':' op (',' op)*
 *   op      := 'c' INDEX '.' COMPONENTS ('*' | '=') factor
 *   factor  := NUMBER | 'rsx' | 'rsy'        draw resolution scale on the x / y axis
 *
 * Example: "0123456789ABCDEF:c47.z*rsx,c47.w*rsy,c77.x*0.5" (multiply c47.z and c47.w by the
 * draw resolution scale and halve c77.x for the pixel shader with ucode hash 0123456789ABCDEF)
 *
 * Whitespace is ignored. A malformed entry is skipped; Update() reports how many were.
 *
 * Implemented by the D3D12 and the Vulkan command processors.
 */
#pragma once

#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

namespace rex::graphics::util {

class ShaderConstantOverrides {
 public:
  enum class Factor : uint8_t { kNumber, kResolutionScaleX, kResolutionScaleY };

  struct Op {
    uint32_t constant;        // c0...c255
    uint32_t component_mask;  // bit 0 = x ... bit 3 = w
    bool assign;              // '=' instead of '*'
    Factor factor;
    float number;
  };

  struct Entry {
    uint64_t ucode_hash;
    std::vector<Op> ops;
    uint64_t constant_bitmap[4] = {};  // constants touched by ops
  };

  // Re-parses only when the text changed. Returns true if the text changed (bindings that were
  // built with the previous overrides must be rebuilt). `malformed` receives the number of
  // skipped entries.
  bool Update(const std::string& text, uint32_t* malformed = nullptr) {
    if (text == text_) {
      return false;
    }
    text_ = text;
    entries_.clear();
    uint32_t bad = 0;
    std::string compact;
    compact.reserve(text.size());
    for (char c : text) {
      if (!std::isspace(static_cast<unsigned char>(c))) {
        compact.push_back(c);
      }
    }
    size_t pos = 0;
    while (pos < compact.size()) {
      size_t end = compact.find(';', pos);
      if (end == std::string::npos) {
        end = compact.size();
      }
      if (end > pos) {
        Entry entry;
        if (ParseEntry(compact.substr(pos, end - pos), entry)) {
          entries_.push_back(std::move(entry));
        } else {
          ++bad;
        }
      }
      pos = end + 1;
    }
    if (malformed) {
      *malformed = bad;
    }
    return true;
  }

  bool empty() const { return entries_.empty(); }

  const Entry* Find(uint64_t ucode_hash) const {
    for (const Entry& entry : entries_) {
      if (entry.ucode_hash == ucode_hash) {
        return &entry;
      }
    }
    return nullptr;
  }

  // Applies the ops of `entry` to a packed float constant buffer: the constants marked in the
  // shader's usage bitmap `map`, copied in ascending order, 16 bytes each (the layout both the
  // D3D12 and the Vulkan backends upload).
  static void ApplyToPacked(const Entry& entry, uint8_t* packed, const uint64_t map[4],
                            float resolution_scale_x, float resolution_scale_y) {
    uint32_t packed_index = 0;
    for (uint32_t i = 0; i < 4; ++i) {
      uint64_t bits = map[i];
      uint64_t touched = bits & entry.constant_bitmap[i];
      while (touched) {
        uint32_t bit = 0;
        while (!(touched & (uint64_t(1) << bit))) {
          ++bit;
        }
        // Position in the packed buffer: constants used before this one.
        uint64_t below = bits & ((uint64_t(1) << bit) - 1);
        uint32_t offset = packed_index;
        for (; below; below &= below - 1) {
          ++offset;
        }
        Apply(entry, (i << 6) + bit, reinterpret_cast<float*>(packed + 16 * size_t(offset)),
              resolution_scale_x, resolution_scale_y);
        touched &= touched - 1;
      }
      for (; bits; bits &= bits - 1) {
        ++packed_index;
      }
    }
  }

  // Applies the ops of `entry` that target `constant` to the float4 `value`.
  static void Apply(const Entry& entry, uint32_t constant, float* value, float resolution_scale_x,
                    float resolution_scale_y) {
    if (!(entry.constant_bitmap[constant >> 6] & (uint64_t(1) << (constant & 63)))) {
      return;
    }
    for (const Op& op : entry.ops) {
      if (op.constant != constant) {
        continue;
      }
      float factor = op.number;
      if (op.factor == Factor::kResolutionScaleX) {
        factor = resolution_scale_x;
      } else if (op.factor == Factor::kResolutionScaleY) {
        factor = resolution_scale_y;
      }
      for (uint32_t i = 0; i < 4; ++i) {
        if (op.component_mask & (1u << i)) {
          value[i] = op.assign ? factor : value[i] * factor;
        }
      }
    }
  }

 private:
  static bool ParseOp(const std::string& s, Op& op) {
    // c<index>.<components><*|=><factor>
    if (s.size() < 5 || (s[0] != 'c' && s[0] != 'C')) {
      return false;
    }
    char* end = nullptr;
    unsigned long index = std::strtoul(s.c_str() + 1, &end, 10);
    if (end == s.c_str() + 1 || index > 255 || *end != '.') {
      return false;
    }
    op.constant = uint32_t(index);
    op.component_mask = 0;
    const char* p = end + 1;
    for (; *p == 'x' || *p == 'y' || *p == 'z' || *p == 'w'; ++p) {
      static const char kComponents[] = "xyzw";
      op.component_mask |= 1u << uint32_t(std::string(kComponents).find(*p));
    }
    if (!op.component_mask || (*p != '*' && *p != '=')) {
      return false;
    }
    op.assign = *p == '=';
    std::string factor(p + 1);
    if (factor == "rsx") {
      op.factor = Factor::kResolutionScaleX;
      op.number = 1.0f;
    } else if (factor == "rsy") {
      op.factor = Factor::kResolutionScaleY;
      op.number = 1.0f;
    } else {
      char* number_end = nullptr;
      op.number = std::strtof(factor.c_str(), &number_end);
      if (factor.empty() || *number_end != '\0') {
        return false;
      }
      op.factor = Factor::kNumber;
    }
    return true;
  }

  static bool ParseEntry(const std::string& s, Entry& entry) {
    size_t colon = s.find(':');
    if (colon == std::string::npos || colon == 0) {
      return false;
    }
    std::string hash = s.substr(0, colon);
    char* end = nullptr;
    entry.ucode_hash = std::strtoull(hash.c_str(), &end, 16);
    if (*end != '\0') {
      return false;
    }
    size_t pos = colon + 1;
    while (pos <= s.size()) {
      size_t comma = s.find(',', pos);
      if (comma == std::string::npos) {
        comma = s.size();
      }
      Op op;
      if (!ParseOp(s.substr(pos, comma - pos), op)) {
        return false;
      }
      entry.ops.push_back(op);
      entry.constant_bitmap[op.constant >> 6] |= uint64_t(1) << (op.constant & 63);
      pos = comma + 1;
    }
    return !entry.ops.empty();
  }

  std::string text_;
  std::vector<Entry> entries_;
};

}  // namespace rex::graphics::util
