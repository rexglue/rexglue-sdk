#include "hook_scan.h"

#include <algorithm>
#include <cctype>
#include <format>
#include <fstream>
#include <iterator>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <rex/memory/mapped_ptr.h>

namespace rex::codegen {

namespace {

namespace fs = std::filesystem;
namespace midasm = rex::ppc::detail;

enum class TokenType : u8 { kIdentifier, kNumber, kPunct, kLiteral };

struct Token {
  TokenType type;
  std::string text;
  u32 line;
  bool conditional;
};

struct ConditionalEntry {
  bool guard;
};

bool IsIdentifierStart(char c) {
  return std::isalpha(static_cast<unsigned char>(c)) != 0 || c == '_';
}

bool IsIdentifierChar(char c) {
  return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_';
}

std::string_view GetDirectiveName(std::string_view directive) {
  size_t begin = directive.find_first_not_of(" \t");
  if (begin == std::string_view::npos) {
    return {};
  }
  size_t end = begin;
  while (end < directive.size() && IsIdentifierChar(directive[end])) {
    ++end;
  }
  return directive.substr(begin, end - begin);
}

std::string_view GetDirectiveOperand(std::string_view directive) {
  std::string_view name = GetDirectiveName(directive);
  size_t begin = directive.find(name) + name.size();
  begin = directive.find_first_not_of(" \t", begin);
  if (begin == std::string_view::npos) {
    return {};
  }
  size_t end = begin;
  while (end < directive.size() && IsIdentifierChar(directive[end])) {
    ++end;
  }
  return directive.substr(begin, end - begin);
}

std::vector<Token> Lex(std::string_view text) {
  std::vector<Token> tokens;
  std::vector<ConditionalEntry> conditionals;
  u32 depth = 0;
  u32 directives = 0;
  std::string guard_name;
  u32 line = 1;
  bool line_start = true;
  size_t last_end = 0;
  size_t i = 0;
  const auto emit = [&](TokenType type, size_t begin, size_t end) {
    tokens.push_back({type, std::string(text.substr(begin, end - begin)), line, depth > 0});
    last_end = end;
  };
  while (i < text.size()) {
    const char c = text[i];
    if (c == '\n') {
      ++line;
      line_start = true;
      ++i;
      continue;
    }
    if (c == ' ' || c == '\t' || c == '\r' || c == '\f' || c == '\v') {
      ++i;
      continue;
    }
    if (c == '/' && i + 1 < text.size() && text[i + 1] == '/') {
      while (i < text.size() && text[i] != '\n') {
        ++i;
      }
      continue;
    }
    if (c == '/' && i + 1 < text.size() && text[i + 1] == '*') {
      i += 2;
      while (i + 1 < text.size() && !(text[i] == '*' && text[i + 1] == '/')) {
        if (text[i] == '\n') {
          ++line;
        }
        ++i;
      }
      i = std::min(i + 2, text.size());
      continue;
    }
    if (c == '#' && line_start) {
      size_t end = i + 1;
      std::string directive;
      while (end < text.size() && text[end] != '\n') {
        if (text[end] == '\\' && end + 1 < text.size() && text[end + 1] == '\n') {
          ++line;
          end += 2;
          continue;
        }
        if (text[end] == '\\' && end + 2 < text.size() && text[end + 1] == '\r' &&
            text[end + 2] == '\n') {
          ++line;
          end += 3;
          continue;
        }
        directive += text[end];
        ++end;
      }
      const std::string_view name = GetDirectiveName(directive);
      if (name == "if" || name == "ifdef" || name == "ifndef") {
        conditionals.push_back({false});
        ++depth;
        if (directives == 0 && name == "ifndef") {
          guard_name = std::string(GetDirectiveOperand(directive));
        }
      } else if (name == "define" && directives == 1 && !guard_name.empty() &&
                 GetDirectiveOperand(directive) == guard_name && conditionals.size() == 1) {
        conditionals.back().guard = true;
        --depth;
      } else if (name == "endif" && !conditionals.empty()) {
        if (!conditionals.back().guard) {
          --depth;
        }
        conditionals.pop_back();
      }
      ++directives;
      i = end;
      continue;
    }
    line_start = false;
    if (c == '"') {
      const bool raw = !tokens.empty() && tokens.back().type == TokenType::kIdentifier &&
                       last_end == i && tokens.back().text.back() == 'R';
      const size_t begin = i;
      if (raw) {
        size_t open = text.find('(', i + 1);
        if (open == std::string_view::npos) {
          open = text.size();
        }
        const std::string close = ")" + std::string(text.substr(i + 1, open - i - 1)) + "\"";
        const size_t end = text.find(close, open);
        i = end == std::string_view::npos ? text.size() : end + close.size();
      } else {
        ++i;
        while (i < text.size() && text[i] != '"' && text[i] != '\n') {
          i += text[i] == '\\' ? 2 : 1;
        }
        i = std::min(i + 1, text.size());
      }
      for (size_t k = begin; k < i; ++k) {
        if (text[k] == '\n') {
          ++line;
        }
      }
      emit(TokenType::kLiteral, begin, i);
      continue;
    }
    if (c == '\'') {
      const size_t begin = i;
      ++i;
      while (i < text.size() && text[i] != '\'' && text[i] != '\n') {
        i += text[i] == '\\' ? 2 : 1;
      }
      i = std::min(i + 1, text.size());
      emit(TokenType::kLiteral, begin, i);
      continue;
    }
    if (std::isdigit(static_cast<unsigned char>(c)) != 0 ||
        (c == '.' && i + 1 < text.size() &&
         std::isdigit(static_cast<unsigned char>(text[i + 1])) != 0)) {
      const size_t begin = i;
      while (i < text.size()) {
        const char d = text[i];
        const bool separator = d == '\'' && i + 1 < text.size() && IsIdentifierChar(text[i + 1]);
        const bool exponent_sign =
            (d == '+' || d == '-') && i > begin &&
            (text[i - 1] == 'e' || text[i - 1] == 'E' || text[i - 1] == 'p' || text[i - 1] == 'P');
        if (!IsIdentifierChar(d) && d != '.' && !separator && !exponent_sign) {
          break;
        }
        ++i;
      }
      emit(TokenType::kNumber, begin, i);
      continue;
    }
    if (IsIdentifierStart(c)) {
      const size_t begin = i;
      while (i < text.size() && IsIdentifierChar(text[i])) {
        ++i;
      }
      emit(TokenType::kIdentifier, begin, i);
      continue;
    }
    emit(TokenType::kPunct, i, i + 1);
    ++i;
  }
  return tokens;
}

struct Body {
  std::string name;
  MidAsmType type;
  std::vector<std::string> types;
  midasm::MidAsmBody descriptor;
  fs::path file;
  u32 line;
};

struct Attachment {
  std::string name;
  u32 address;
  std::string placement;
  std::vector<std::string> arguments;
  fs::path file;
  u32 line;
};

std::string_view StripQualifiers(std::string_view type) {
  if (type.starts_with("const ")) {
    type.remove_prefix(6);
  }
  if (type.starts_with("::")) {
    type.remove_prefix(2);
  }
  for (const std::string_view prefix : {"rex::", "memory::", "std::"}) {
    if (type.starts_with(prefix)) {
      type.remove_prefix(prefix.size());
    }
  }
  return type;
}

bool IsMappedPtrType(std::string_view type) {
  static constexpr std::string_view kAliases[] = {"MappedVoid", "MappedU8",     "MappedU16",
                                                  "MappedU32",  "MappedU64",    "MappedF32",
                                                  "MappedF64",  "MappedString", "MappedWString"};
  if (std::ranges::find(kAliases, type) != std::end(kAliases)) {
    return true;
  }
  for (const std::string_view prefix : {"MappedPtr<", "GuestPtrT<"}) {
    if (type.starts_with(prefix) && type.ends_with('>') && type.size() > prefix.size() + 1) {
      return true;
    }
  }
  return false;
}

midasm::MidAsmParam ClassifyType(std::string_view type) {
  if (type.ends_with('&') && type.starts_with("const ")) {
    return midasm::MidAsmParam::kInvalid;
  }
  type = StripQualifiers(type);
  if (IsMappedPtrType(type)) {
    return midasm::MidAsmParam::kMappedPointer;
  }
  if (type.size() > 1 && type.ends_with('*') && type.find('(') == std::string_view::npos) {
    return midasm::MidAsmParam::kPointer;
  }
  static const std::map<std::string_view, midasm::MidAsmParam> kTypes = {
      {"u32", midasm::MidAsmParam::kU32},
      {"uint32_t", midasm::MidAsmParam::kU32},
      {"u64", midasm::MidAsmParam::kU64},
      {"uint64_t", midasm::MidAsmParam::kU64},
      {"f64", midasm::MidAsmParam::kF64},
      {"double", midasm::MidAsmParam::kF64},
      {"PPCVRegister", midasm::MidAsmParam::kVector},
      {"PPCRegister&", midasm::MidAsmParam::kRegister},
      {"PPCVRegister&", midasm::MidAsmParam::kVectorRef},
      {"PPCCRRegister&", midasm::MidAsmParam::kCR},
      {"PPCXERRegister&", midasm::MidAsmParam::kXER},
      {"PPCFPSCRRegister&", midasm::MidAsmParam::kFPSCR},
      {"PPCContext&", midasm::MidAsmParam::kContext},
  };
  const auto found = kTypes.find(type);
  return found == kTypes.end() ? midasm::MidAsmParam::kInvalid : found->second;
}

std::string JoinTokens(std::span<const Token> tokens) {
  std::string text;
  const Token* previous = nullptr;
  for (const Token& token : tokens) {
    const auto is_word = [](const Token& t) {
      return t.type == TokenType::kIdentifier || t.type == TokenType::kNumber;
    };
    if (previous && is_word(*previous) && is_word(token)) {
      text += ' ';
    }
    text += token.text;
    previous = &token;
  }
  return text;
}

struct Invocation {
  std::vector<std::vector<Token>> arguments;
  size_t next = 0;
};

std::optional<Invocation> ReadInvocation(const std::vector<Token>& tokens, size_t open) {
  Invocation invocation;
  invocation.arguments.emplace_back();
  u32 depth = 0;
  for (size_t k = open + 1; k < tokens.size(); ++k) {
    const Token& token = tokens[k];
    if (token.type == TokenType::kPunct) {
      if (token.text == "(" || token.text == "[" || token.text == "{") {
        ++depth;
      } else if (token.text == ")" || token.text == "]" || token.text == "}") {
        if (depth == 0) {
          if (token.text != ")") {
            return std::nullopt;
          }
          invocation.next = k + 1;
          return invocation;
        }
        --depth;
      } else if (token.text == "," && depth == 0) {
        invocation.arguments.emplace_back();
        continue;
      }
    }
    invocation.arguments.back().push_back(token);
  }
  return std::nullopt;
}

bool IsSingle(const std::vector<Token>& argument, TokenType type) {
  return argument.size() == 1 && argument[0].type == type;
}

std::string FormatLocation(const fs::path& file, u32 line) {
  return std::format("{}:{}", file.string(), line);
}

std::string DescribeResult(midasm::MidAsmAtResult result) {
  switch (result) {
    case midasm::MidAsmAtResult::kBadPlacement:
      return "the placement must be before or after";
    case midasm::MidAsmAtResult::kUnexpectedTarget:
      return "only a branch body takes a target";
    case midasm::MidAsmAtResult::kMissingTarget:
      return "a branch body needs its target first";
    case midasm::MidAsmAtResult::kBadTarget:
      return "the target must be a nonzero multiple of 4";
    case midasm::MidAsmAtResult::kBadRegister:
      return "a register must be r0 to r31 except r1, f0 to f31, v0 to v127, cr0 to cr7, ctr, xer, "
             "fpscr, reserved or ctx";
    case midasm::MidAsmAtResult::kCountMismatch:
      return "the register count must equal the body parameter count";
    case midasm::MidAsmAtResult::kTypeMismatch:
      return "a register type does not fit its parameter";
    case midasm::MidAsmAtResult::kOk:
      break;
  }
  return {};
}

Result<std::vector<SourceMidAsmHook>> Fail(const fs::path& file, u32 line,
                                           const std::string& message) {
  return Err<std::vector<SourceMidAsmHook>>(
      ErrorCategory::kValidation, std::format("{}: {}", FormatLocation(file, line), message));
}

struct Scan {
  std::map<std::string, Body> bodies;
  std::vector<Attachment> attachments;
};

std::optional<std::string> ReadBody(const fs::path& file, const Token& macro,
                                    const Invocation& invocation, MidAsmType type, Scan& scan) {
  const auto& arguments = invocation.arguments;
  if (arguments.size() != 2 || !IsSingle(arguments[0], TokenType::kIdentifier)) {
    return std::format("{} needs a name and a parameter list", macro.text);
  }
  const auto& list = arguments[1];
  if (list.size() < 2 || list.front().text != "(" || list.back().text != ")") {
    return std::format("{} needs its parameters in parentheses", macro.text);
  }
  Body body{arguments[0][0].text, type, {}, {type, 0, {}}, file, macro.line};
  std::vector<std::vector<Token>> parameters(1);
  u32 depth = 0;
  for (size_t k = 1; k + 1 < list.size(); ++k) {
    if (list[k].text == "(" || list[k].text == "[" || list[k].text == "{") {
      ++depth;
    } else if (list[k].text == ")" || list[k].text == "]" || list[k].text == "}") {
      --depth;
    } else if (list[k].text == "," && depth == 0) {
      parameters.emplace_back();
      continue;
    }
    parameters.back().push_back(list[k]);
  }
  if (parameters.size() == 1 && parameters[0].empty()) {
    parameters.clear();
  }
  if (parameters.size() > midasm::kMaxMidAsmParams) {
    return std::format("{} takes at most {} parameters", body.name, midasm::kMaxMidAsmParams);
  }
  for (const auto& parameter : parameters) {
    std::string parameter_type = JoinTokens(parameter);
    midasm::MidAsmParam type_of = ClassifyType(parameter_type);
    if (type_of == midasm::MidAsmParam::kInvalid && parameter.size() > 1 &&
        parameter.back().type == TokenType::kIdentifier) {
      parameter_type = JoinTokens(std::span<const Token>(parameter).first(parameter.size() - 1));
      type_of = ClassifyType(parameter_type);
    }
    if (type_of == midasm::MidAsmParam::kInvalid) {
      return std::format(
          "the parameter '{}' of {} is not u32, u64, f64, PPCVRegister, a pointer, a MappedPtr, "
          "a reference to a register type or PPCContext&",
          JoinTokens(parameter), body.name);
    }
    if (type_of == midasm::MidAsmParam::kPointer) {
      parameter_type = "void*";
    } else if (type_of == midasm::MidAsmParam::kMappedPointer) {
      parameter_type = "rex::memory::MappedPtr<u8>";
    } else if (type_of == midasm::MidAsmParam::kU32) {
      parameter_type = "u32";
    } else if (type_of == midasm::MidAsmParam::kU64) {
      parameter_type = "u64";
    } else if (type_of == midasm::MidAsmParam::kF64) {
      parameter_type = "f64";
    } else {
      parameter_type = std::string(StripQualifiers(parameter_type));
    }
    body.descriptor.params[body.types.size()] = type_of;
    body.types.push_back(parameter_type);
  }
  body.descriptor.count = body.types.size();
  const auto [it, inserted] = scan.bodies.emplace(body.name, body);
  if (!inserted) {
    return std::format("{} is already declared at {}", body.name,
                       FormatLocation(it->second.file, it->second.line));
  }
  return std::nullopt;
}

std::optional<std::string> ReadAttachment(const fs::path& file, const Token& macro,
                                          const Invocation& invocation, Scan& scan) {
  const auto& arguments = invocation.arguments;
  if (arguments.size() < 3 || !IsSingle(arguments[0], TokenType::kIdentifier) ||
      !IsSingle(arguments[1], TokenType::kNumber) ||
      !IsSingle(arguments[2], TokenType::kIdentifier)) {
    return std::format("{} needs a body name, a literal address and a placement", macro.text);
  }
  u32 address = 0;
  if (!midasm::ParseMidAsmAddress(arguments[1][0].text, address) ||
      !midasm::IsMidAsmAddress(address)) {
    return std::format("the address {} must be a literal that is a nonzero multiple of 4",
                       arguments[1][0].text);
  }
  Attachment attachment{arguments[0][0].text, address, arguments[2][0].text, {}, file, macro.line};
  for (size_t k = 3; k < arguments.size(); ++k) {
    attachment.arguments.push_back(JoinTokens(arguments[k]));
  }
  scan.attachments.push_back(std::move(attachment));
  return std::nullopt;
}

std::optional<std::string> ScanFile(const fs::path& file, Scan& scan) {
  std::ifstream in(file, std::ios::binary);
  if (!in) {
    return std::format("{}: the file cannot be read", file.string());
  }
  const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  const std::vector<Token> tokens = Lex(text);
  for (size_t k = 0; k + 1 < tokens.size(); ++k) {
    const Token& macro = tokens[k];
    if (macro.type != TokenType::kIdentifier || tokens[k + 1].text != "(") {
      continue;
    }
    const bool is_body = macro.text == "REX_MIDASM_HOOK" || macro.text == "REX_MIDASM_BRANCH" ||
                         macro.text == "REX_MIDASM_RETURN_IF";
    if (!is_body && macro.text != "REX_MIDASM_AT") {
      continue;
    }
    const auto location = FormatLocation(file, macro.line);
    if (macro.conditional) {
      return std::format("{}: {} is inside a preprocessor conditional", location, macro.text);
    }
    const auto invocation = ReadInvocation(tokens, k + 1);
    if (!invocation) {
      return std::format("{}: {} cannot be read, its parentheses do not close", location,
                         macro.text);
    }
    std::optional<std::string> problem;
    if (is_body) {
      const MidAsmType type = macro.text == "REX_MIDASM_HOOK"     ? MidAsmType::kHook
                              : macro.text == "REX_MIDASM_BRANCH" ? MidAsmType::kBranch
                                                                  : MidAsmType::kReturnIf;
      problem = ReadBody(file, macro, *invocation, type, scan);
    } else {
      problem = ReadAttachment(file, macro, *invocation, scan);
    }
    if (problem) {
      return std::format("{}: {}", location, *problem);
    }
    k = invocation->next - 1;
  }
  return std::nullopt;
}

bool IsSourceFile(const fs::path& path) {
  const std::string extension = path.extension().string();
  return extension == ".cpp" || extension == ".cc" || extension == ".h" || extension == ".hpp";
}

}  // namespace

Result<std::vector<SourceMidAsmHook>> ScanHookSources(std::span<const fs::path> roots,
                                                      std::vector<fs::path>& scanned_files) {
  std::set<fs::path> seen;
  Scan scan;
  for (const fs::path& root : roots) {
    std::error_code ec;
    if (!fs::is_directory(root, ec)) {
      return Err<std::vector<SourceMidAsmHook>>(
          ErrorCategory::kIO,
          std::format("[project].hook_sources folder not found: {}", root.string()));
    }
    std::vector<fs::path> files;
    std::vector<fs::path> folders = {root};
    for (const auto& entry : fs::recursive_directory_iterator(root, ec)) {
      if (entry.is_regular_file() && IsSourceFile(entry.path())) {
        files.push_back(entry.path());
      } else if (entry.is_directory()) {
        folders.push_back(entry.path());
      }
    }
    std::sort(files.begin(), files.end());
    std::sort(folders.begin(), folders.end());
    scanned_files.insert(scanned_files.end(), folders.begin(), folders.end());
    for (const fs::path& file : files) {
      if (!seen.insert(fs::weakly_canonical(file, ec)).second) {
        continue;
      }
      scanned_files.push_back(file);
      if (auto problem = ScanFile(file, scan)) {
        return Err<std::vector<SourceMidAsmHook>>(ErrorCategory::kValidation, *problem);
      }
    }
  }

  std::vector<SourceMidAsmHook> hooks;
  std::map<u32, const Attachment*> by_address;
  for (const Attachment& attachment : scan.attachments) {
    const auto body = scan.bodies.find(attachment.name);
    if (body == scan.bodies.end()) {
      return Fail(
          attachment.file, attachment.line,
          std::format("REX_MIDASM_AT names {}, which no body macro declares", attachment.name));
    }
    std::string arguments;
    for (const std::string& argument : attachment.arguments) {
      arguments += arguments.empty() ? "" : ",";
      arguments += argument;
    }
    const auto result =
        midasm::CheckMidAsmAt(body->second.descriptor, attachment.placement, arguments);
    if (result != midasm::MidAsmAtResult::kOk) {
      return Fail(attachment.file, attachment.line,
                  std::format("REX_MIDASM_AT {}: {}", attachment.name, DescribeResult(result)));
    }
    const auto [claimed, inserted] = by_address.emplace(attachment.address, &attachment);
    if (!inserted) {
      return Fail(attachment.file, attachment.line,
                  std::format("0x{:08X} already has a mid asm hook at {}", attachment.address,
                              FormatLocation(claimed->second->file, claimed->second->line)));
    }
    SourceMidAsmHook hook{attachment.name,
                          attachment.address,
                          attachment.placement == "after",
                          0,
                          body->second.type,
                          {},
                          body->second.types,
                          attachment.file,
                          attachment.line};
    size_t first = 0;
    if (hook.type == MidAsmType::kBranch) {
      midasm::ParseMidAsmAddress(attachment.arguments[0], hook.target);
      first = 1;
    }
    for (size_t k = first; k < attachment.arguments.size(); ++k) {
      hook.registers.push_back(attachment.arguments[k]);
    }
    hooks.push_back(std::move(hook));
  }
  std::sort(hooks.begin(), hooks.end(), [](const SourceMidAsmHook& a, const SourceMidAsmHook& b) {
    return a.address < b.address;
  });
  return hooks;
}

MidAsmHook ToMidAsmHook(const SourceMidAsmHook& hook) {
  MidAsmHook entry;
  entry.name = hook.name;
  entry.registers = hook.registers;
  entry.parameters = hook.parameters;
  entry.after_instruction = hook.after;
  entry.from_source = true;
  if (hook.type == MidAsmType::kBranch) {
    entry.jump_address_on_true = hook.target;
  } else if (hook.type == MidAsmType::kReturnIf) {
    entry.return_on_true = true;
  }
  return entry;
}

}  // namespace rex::codegen
