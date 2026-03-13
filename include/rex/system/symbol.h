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

#pragma once

#include <cstdint>
#include <string>

namespace rex::runtime {

class Module;

class Symbol {
 public:
  enum class Type {
    kFunction,
    kVariable,
  };

  Symbol(Type type, Module* module, uint32_t address)
      : type_(type), module_(module), address_(address) {}
  virtual ~Symbol() = default;

  Type type() const { return type_; }
  Module* module() const { return module_; }
  uint32_t address() const { return address_; }

  const std::string& name() const { return name_; }
  void set_name(const std::string_view value) { name_ = value; }

 protected:
  Type type_ = Type::kVariable;
  Module* module_ = nullptr;
  uint32_t address_ = 0;

  std::string name_;
};

}  // namespace rex::runtime
