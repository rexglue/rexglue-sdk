/**
 * @file        code_emitter.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/codegen/code_emitter.h>

namespace rex::codegen {

void CodeEmitter::ensureCsrState(CsrState required) {
  if (csrState_ == required)
    return;

  // Emit CSR mode change if needed
  if (required == CsrState::Vmx) {
    line("REX_SET_FLUSH_MODE(true);");
  } else if (required == CsrState::Fpu) {
    line("REX_SET_FLUSH_MODE(false);");
  }

  csrState_ = required;
}

}  // namespace rex::codegen
