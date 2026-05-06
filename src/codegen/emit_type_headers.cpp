/**
 * @file        codegen/emit_type_headers.cpp
 * @brief       Type / enum header emission from RecompilerConfig
 *
 * @copyright   Copyright (c) 2026 Tom Clay
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 *
 * @remarks     Conservative opaque-storage emission. Each producer
 *              type becomes an opaque struct sized to the producer's
 *              `size` field, with field offsets exposed via a sibling
 *              `<Type>_offsets` namespace. We deliberately do not
 *              translate the producer's MSVC type expressions into
 *              real C++ types -- that would require a dialect-aware
 *              parser plus dependency-sorting tens of thousands of
 *              UDTs, and is intentionally deferred. Field offsets
 *              cover the host substrate's actual need: replacing
 *              `blob + 0x40` with `<Type>_offsets::FieldName`.
 */

#include <rex/codegen/emit_type_headers.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <unordered_set>

#include <fmt/format.h>

#include <rex/codegen/codegen_context.h>
#include <rex/codegen/config.h>
#include <rex/logging.h>

#include "codegen_logging.h"

namespace rex::codegen {

namespace {

// Translate a producer-supplied enum underlying type string to a
// fixed-width C++ identifier. The Xbox 360 ABI maps `long` to 32
// bits, matching MSVC PPC. Anything we don't recognise falls back to
// int32_t (matches MSVC's default enum underlying).
std::string_view EnumUnderlying(std::string_view raw) {
  if (raw == "char") return "int8_t";
  if (raw == "unsigned char") return "uint8_t";
  if (raw == "short") return "int16_t";
  if (raw == "unsigned short") return "uint16_t";
  if (raw == "int") return "int32_t";
  if (raw == "unsigned int") return "uint32_t";
  if (raw == "long") return "int32_t";
  if (raw == "unsigned long") return "uint32_t";
  if (raw == "__int64") return "int64_t";
  if (raw == "unsigned __int64") return "uint64_t";
  if (raw == "bool") return "bool";
  return "int32_t";
}

// Re-escape a raw producer type expression for use inside a C++
// comment. PDB strings can contain `*/` literals (function-pointer
// return types like `void(...)*`), so collapse `*/` to `* /` to
// prevent the comment from terminating early.
std::string EscapeComment(std::string_view raw) {
  std::string out;
  out.reserve(raw.size());
  for (size_t i = 0; i < raw.size(); ++i) {
    if (i + 1 < raw.size() && raw[i] == '*' && raw[i + 1] == '/') {
      out.append("* /");
      ++i;
    } else {
      out.push_back(raw[i]);
    }
  }
  return out;
}

// Sanitize an enum value name. Producer values are typically already
// C identifiers but some include characters like `:` (anonymous
// nested values) that need flattening. C++ keywords are not guarded
// at this level -- enum values are conventionally upper-case in MSVC
// headers and collisions don't arise in practice.
std::string SanitizeEnumValue(std::string_view raw) {
  std::string out;
  out.reserve(raw.size());
  for (char c : raw) {
    const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                    (c >= '0' && c <= '9') || c == '_';
    out.push_back(ok ? c : '_');
  }
  if (out.empty() || (out[0] >= '0' && out[0] <= '9'))
    return {};
  return out;
}

// C++ keywords that must not appear as identifiers. Producer field
// names sometimes match these (e.g. `register`, `namespace`, `not`
// observed in real PDBs); without the guard the generated header
// would fail to compile. Includes the C alternative tokens
// defensively (GCC-strict mode rejects them as identifiers even
// though clang accepts them by default).
bool IsCppKeyword(std::string_view s) {
  static const std::unordered_set<std::string_view> kw = {
      "alignas",      "alignof",     "and",          "and_eq",       "asm",          "auto",
      "bitand",       "bitor",       "bool",         "break",        "case",         "catch",
      "char",         "char8_t",     "char16_t",     "char32_t",     "class",        "compl",
      "concept",      "const",       "consteval",   "constexpr",    "constinit",    "const_cast",
      "continue",     "co_await",    "co_return",   "co_yield",     "decltype",     "default",
      "delete",       "do",          "double",       "dynamic_cast", "else",         "enum",
      "explicit",     "export",      "extern",       "false",        "float",        "for",
      "friend",       "goto",        "if",           "inline",       "int",          "long",
      "mutable",      "namespace",   "new",          "noexcept",     "not",          "not_eq",
      "nullptr",      "operator",    "or",           "or_eq",        "private",      "protected",
      "public",       "register",    "reinterpret_cast",            "requires",     "return",
      "short",        "signed",      "sizeof",       "static",       "static_assert",
      "static_cast",  "struct",      "switch",       "template",     "this",         "thread_local",
      "throw",        "true",        "try",          "typedef",      "typeid",       "typename",
      "union",        "unsigned",    "using",        "virtual",      "void",         "volatile",
      "wchar_t",      "while",       "xor",          "xor_eq",
  };
  return kw.count(s) != 0;
}

// Sanitize a struct field name. Same rule as enum value plus a C++
// keyword guard: anything matching a reserved word gets an `f_`
// prefix so the generated header always parses.
std::string SanitizeFieldName(std::string_view raw) {
  auto out = SanitizeEnumValue(raw);
  if (out.empty())
    return out;
  if (IsCppKeyword(out))
    out.insert(0, "f_");
  return out;
}

}  // namespace

void EmitTypeHeaders(const CodegenContext& ctx) {
  const auto& cfg = ctx.Config();
  if (cfg.types.empty() && cfg.enums.empty())
    return;

  std::filesystem::path outDir =
      ctx.configDir() / cfg.outDirectoryPath / "mappings_generated";
  std::error_code ec;
  std::filesystem::create_directories(outDir, ec);
  if (ec) {
    REXCODEGEN_ERROR("[type-headers] failed to create {}: {}", outDir.string(), ec.message());
    return;
  }

  // ---- types.h --------------------------------------------------
  if (!cfg.types.empty()) {
    std::string buf;
    buf.reserve(cfg.types.size() * 256);
    fmt::format_to(std::back_inserter(buf),
                   "// Generated by ReXGlue from project config.\n"
                   "// Do not edit; regenerate by re-running codegen.\n"
                   "// Schema: docs/types_and_enums.md\n"
                   "#pragma once\n"
                   "\n"
                   "#include <cstddef>\n"
                   "#include <cstdint>\n"
                   "\n");

    size_t emittedTypes = 0;
    for (const auto& t : cfg.types) {
      // Opaque-storage emission: expose size + per-field offsets,
      // do not translate the producer's MSVC type expressions into
      // real C++ types. See the file-level remarks for rationale.
      fmt::format_to(std::back_inserter(buf),
                     "// {} (kind={}, unique_name={})\n"
                     "struct {} {{\n"
                     "    unsigned char _opaque[0x{:X}];\n"
                     "}};\n",
                     EscapeComment(t.rawName), t.kind, EscapeComment(t.uniqueName), t.name,
                     t.size == 0 ? 1u : t.size);

      // Only enforce sizeof when the producer reported a non-zero
      // size. Forward-declared / zero-sized PDB types report
      // size==0; emitting a static_assert there would either pass
      // trivially (sizeof always >= 1) or fail spuriously.
      if (t.size > 0) {
        fmt::format_to(std::back_inserter(buf),
                       "static_assert(sizeof({}) == 0x{:X}, \"{} size mismatch with producer\");\n",
                       t.name, t.size, t.name);
      }

      if (!t.fields.empty()) {
        fmt::format_to(std::back_inserter(buf), "namespace {}_offsets {{\n", t.name);
        std::unordered_set<std::string> seenFields;
        for (const auto& f : t.fields) {
          auto fname = SanitizeFieldName(f.name);
          if (fname.empty())
            continue;
          // Anonymous unions surface multiple fields at the same
          // offset; the header only needs one entry per unique
          // sanitized name. First occurrence wins.
          if (!seenFields.insert(fname).second)
            continue;

          fmt::format_to(std::back_inserter(buf),
                         "    inline constexpr size_t {} = 0x{:X}; // {}\n", fname, f.offset,
                         EscapeComment(f.typeExpr));
        }
        buf.append("}\n");
      }
      buf.push_back('\n');
      ++emittedTypes;
    }

    auto path = (outDir / "types.h").string();
    std::ofstream typesOut(path);
    if (!typesOut) {
      REXCODEGEN_ERROR("[type-headers] failed to open {} for writing", path);
    } else {
      typesOut << buf;
      REXCODEGEN_INFO("[type-headers] wrote {} ({} types)", path, emittedTypes);
    }
  }

  // ---- enums.h --------------------------------------------------
  if (!cfg.enums.empty()) {
    std::string buf;
    buf.reserve(cfg.enums.size() * 128);
    fmt::format_to(std::back_inserter(buf),
                   "// Generated by ReXGlue from project config.\n"
                   "// Do not edit; regenerate by re-running codegen.\n"
                   "// Schema: docs/types_and_enums.md\n"
                   "#pragma once\n"
                   "\n"
                   "#include <cstdint>\n"
                   "\n");

    size_t emittedEnums = 0;
    for (const auto& e : cfg.enums) {
      fmt::format_to(std::back_inserter(buf),
                     "// {} (underlying={}, unique_name={})\n"
                     "enum class {} : {} {{\n",
                     EscapeComment(e.rawName), e.underlying, EscapeComment(e.uniqueName), e.name,
                     EnumUnderlying(e.underlying));

      std::unordered_set<std::string> seenValues;
      for (const auto& v : e.values) {
        auto vname = SanitizeEnumValue(v.name);
        if (vname.empty())
          continue;
        // Two enumerators with the same name in one enum would be
        // a hard C++ error; dedupe by name and keep the first.
        if (!seenValues.insert(vname).second)
          continue;
        fmt::format_to(std::back_inserter(buf), "    {} = {},\n", vname, v.value);
      }
      buf.append("};\n\n");
      ++emittedEnums;
    }

    auto path = (outDir / "enums.h").string();
    std::ofstream enumsOut(path);
    if (!enumsOut) {
      REXCODEGEN_ERROR("[type-headers] failed to open {} for writing", path);
    } else {
      enumsOut << buf;
      REXCODEGEN_INFO("[type-headers] wrote {} ({} enums)", path, emittedEnums);
    }
  }
}

}  // namespace rex::codegen
