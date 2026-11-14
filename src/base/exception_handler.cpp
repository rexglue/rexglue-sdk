/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

/*
 * Based on VIXL Instruction::IsLoad and IsStore.
 * https://github.com/Linaro/vixl/blob/d48909dd0ac62197edb75d26ed50927e4384a199/src/aarch64/instructions-aarch64.cc#L484
 *
 * Copyright 2015, VIXL authors
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *   * Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright notice,
 *     this list of conditions and the following disclaimer in the documentation
 *     and/or other materials provided with the distribution.
 *   * Neither the name of ARM Limited nor the names of its contributors may be
 *     used to endorse or promote products derived from this software without
 *     specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS CONTRIBUTORS "AS IS" AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <rex/platform/exception_handler.h>

namespace rex::platform {

bool IsARM64LoadPrefetchStore(u32 instruction, bool& is_store_out) {
  if ((instruction & kARM64LoadLiteralFMask) == kARM64LoadLiteralFixed) {
    return true;
  }
  if ((instruction & kARM64LoadStoreAnyFMask) != kARM64LoadStoreAnyFixed) {
    return false;
  }
  if ((instruction & kARM64LoadStorePairAnyFMask) == kARM64LoadStorePairAnyFixed) {
    is_store_out = !(instruction & kARM64LoadStorePairLoadBit);
    return true;
  }
  switch (ARM64LoadStoreOp(instruction & kARM64LoadStoreMask)) {
    case ARM64LoadStoreOp::kLDRBW:
    case ARM64LoadStoreOp::kLDRHW:
    case ARM64LoadStoreOp::kLDRW:
    case ARM64LoadStoreOp::kLDRX:
    case ARM64LoadStoreOp::kLDRSBX:
    case ARM64LoadStoreOp::kLDRSHX:
    case ARM64LoadStoreOp::kLDRSWX:
    case ARM64LoadStoreOp::kLDRSBW:
    case ARM64LoadStoreOp::kLDRSHW:
    case ARM64LoadStoreOp::kLDRB:
    case ARM64LoadStoreOp::kLDRH:
    case ARM64LoadStoreOp::kLDRS:
    case ARM64LoadStoreOp::kLDRD:
    case ARM64LoadStoreOp::kLDRQ:
    case ARM64LoadStoreOp::kPRFM:
      is_store_out = false;
      return true;
    case ARM64LoadStoreOp::kSTRBW:
    case ARM64LoadStoreOp::kSTRHW:
    case ARM64LoadStoreOp::kSTRW:
    case ARM64LoadStoreOp::kSTRX:
    case ARM64LoadStoreOp::kSTRB:
    case ARM64LoadStoreOp::kSTRH:
    case ARM64LoadStoreOp::kSTRS:
    case ARM64LoadStoreOp::kSTRD:
    case ARM64LoadStoreOp::kSTRQ:
      is_store_out = true;
      return true;
    default:
      return false;
  }
}

}  // namespace rex::platform
