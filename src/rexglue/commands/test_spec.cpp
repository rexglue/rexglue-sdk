#include "test_spec.h"

#include <charconv>
#include <optional>

#include <rex/string.h>

namespace rexglue::cli {

namespace {

std::vector<u8> ParseHexBytes(const std::string& hex) {
  std::vector<u8> result;
  for (size_t i = 0; i < hex.size(); i += 2) {
    if (i + 1 >= hex.size())
      break;
    if (hex[i] == ' ') {
      i--;
      continue;
    }
    u8 val = 0;
    std::from_chars(&hex[i], &hex[i + 2], val, 16);
    result.push_back(val);
  }
  return result;
}

/**
 * Parses a vector register line. The 4 words are listed high to low in the source and stored
 * low to high to match the runtime layout.
 *
 * @param line Directive line.
 * @param reg Register name.
 * @param after Offset in the line past the register name.
 * @return The register value.
 */
RegValue ParseVectorRegister(const std::string& line, std::string reg, size_t after) {
  RegValue rv;
  rv.reg = std::move(reg);
  rv.is_vector = true;
  auto open = line.find('[', after + 1);
  auto c0 = line.find(',', open + 1);
  auto c1 = line.find(',', c0 + 1);
  auto c2 = line.find(',', c1 + 1);
  auto close = line.find(']', c2 + 1);
  rv.vec_values[3] = line.substr(open + 1, c0 - open - 1);
  rv.vec_values[2] = line.substr(c0 + 2, c1 - c0 - 2);
  rv.vec_values[1] = line.substr(c1 + 2, c2 - c1 - 2);
  rv.vec_values[0] = line.substr(c2 + 2, close - c2 - 2);
  return rv;
}

RegValue ParseRegisterDirective(const std::string& line, size_t directive_idx) {
  auto sp1 = line.find(' ', directive_idx);
  auto sp2 = line.find(' ', sp1 + 1);
  std::string reg = line.substr(sp1 + 1, sp2 - sp1 - 1);
  if (!reg.empty() && reg[0] == 'v')
    return ParseVectorRegister(line, std::move(reg), sp2);

  RegValue rv;
  rv.reg = std::move(reg);
  rv.value = line.substr(sp2 + 1);
  rv.is_float = (line.find('.', sp2) != std::string::npos);
  return rv;
}

MemValue ParseMemoryDirective(const std::string& line, size_t directive_idx) {
  auto sp1 = line.find(' ', directive_idx);
  auto sp2 = line.find(' ', sp1 + 1);
  MemValue mv;
  mv.address = line.substr(sp1 + 1, sp2 - sp1 - 1);
  mv.data = ParseHexBytes(line.substr(sp2 + 1));
  return mv;
}

void ApplyDirective(const std::string& line, std::string_view reg_token, std::string_view mem_token,
                    std::vector<RegValue>& regs, std::vector<MemValue>& mems) {
  if (line.size() <= 1 || line[1] != '_')
    return;
  if (auto idx = line.find(reg_token); idx != std::string::npos) {
    regs.push_back(ParseRegisterDirective(line, idx));
  } else if (auto idx2 = line.find(mem_token); idx2 != std::string::npos) {
    mems.push_back(ParseMemoryDirective(line, idx2));
  }
}

}  // namespace

std::string BuildHierarchicalName(const std::string& stem, const std::string& label) {
  if (stem.starts_with("instr_")) {
    std::string group = stem.substr(6);
    std::string prefix = "test_" + group + "_";
    if (label.starts_with(prefix))
      return group + ".test_" + label.substr(prefix.size());
    if (label == "test_" + group)
      return group + ".test";
    return group + "." + label;
  }
  if (stem.starts_with("seq_")) {
    if (label.starts_with("seq_"))
      return "seq.test_" + label.substr(4);
    return "seq." + label;
  }
  return stem + "." + label;
}

std::vector<TestSpec> ParseTestSpecs(std::istream& in,
                                     const std::unordered_map<std::string, std::string>& symbols) {
  std::vector<TestSpec> specs;

  std::string line;
  bool in_block_comment = false;
  auto read_line = [&]() -> bool {
    while (std::getline(in, line)) {
      line = rex::TrimString(line);
      if (in_block_comment) {
        auto end = line.find("*/");
        if (end == std::string::npos)
          continue;
        in_block_comment = false;
        line = rex::TrimString(line.substr(end + 2));
        if (line.empty())
          continue;
      }
      if (auto start = line.find("/*"); start != std::string::npos) {
        if (auto end = line.find("*/", start + 2); end != std::string::npos) {
          line = line.substr(0, start) + line.substr(end + 2);
        } else {
          in_block_comment = true;
          line = line.substr(0, start);
        }
        line = rex::TrimString(line);
        if (line.empty())
          continue;
      }
      return true;
    }
    return false;
  };

  std::optional<TestSpec> spec;
  auto finish = [&] {
    if (spec && (!spec->inputs.empty() || !spec->outputs.empty() || !spec->mem_inputs.empty() ||
                 !spec->mem_outputs.empty())) {
      specs.push_back(std::move(*spec));
    }
    spec.reset();
  };

  while (read_line()) {
    if (line.empty())
      continue;
    if (line[0] == '#') {
      if (spec) {
        ApplyDirective(line, "REGISTER_IN", "MEMORY_IN", spec->inputs, spec->mem_inputs);
        ApplyDirective(line, "REGISTER_OUT", "MEMORY_OUT", spec->outputs, spec->mem_outputs);
      }
      continue;
    }
    auto colon_index = line.find(':');
    if (colon_index == std::string::npos)
      continue;
    auto name = line.substr(0, colon_index);
    auto symbol_it = symbols.find(name);
    if (symbol_it == symbols.end())
      continue;

    finish();
    spec.emplace();
    spec->name = name;
    spec->symbol = symbol_it->second;
  }
  finish();
  return specs;
}

}  // namespace rexglue::cli
