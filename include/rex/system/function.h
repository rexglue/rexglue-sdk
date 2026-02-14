/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2013 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#pragma once

#include <cstdint>

#include <rex/system/symbol.h>

namespace rex::runtime {

class Module;
class ThreadState;

class Function : public Symbol {
 public:
  enum class Behavior {
    kDefault = 0,
    kProlog,
    kEpilog,
    kEpilogReturn,
    kBuiltin,
    kExtern,
  };

  virtual ~Function() = default;

  uint32_t address() const { return address_; }
  bool has_end_address() const { return end_address_ > 0; }
  uint32_t end_address() const { return end_address_; }
  void set_end_address(uint32_t value) { end_address_ = value; }
  Behavior behavior() const { return behavior_; }
  void set_behavior(Behavior value) { behavior_ = value; }
  bool is_guest() const { return behavior_ != Behavior::kBuiltin; }

  bool ContainsAddress(uint32_t addr) const {
    if (!address_ || !end_address_) {
      return false;
    }
    return addr >= address_ && addr < end_address_;
  }

 protected:
  Function(Module* module, uint32_t address) : Symbol(Symbol::Type::kFunction, module, address) {}

  uint32_t end_address_ = 0;
  Behavior behavior_ = Behavior::kDefault;
};

}  // namespace rex::runtime
