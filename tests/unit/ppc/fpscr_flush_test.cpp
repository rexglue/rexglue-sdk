/**
 * @file        tests/unit/ppc/fpscr_flush_test.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "rex_test.h"

#include <rex/ppc/context.h>

namespace {

struct RestoreCSR {
  u32 saved = rex::ppc::FPSCRRegister::Platform::GetCSR();
  ~RestoreCSR() { rex::ppc::FPSCRRegister::Platform::SetCSR(saved); }
};

}  // namespace

REX_TEST_CASE("SetFlushMode turns the host flush mode on and off", "[ppc][fpscr]") {
  const RestoreCSR restore;
  rex::ppc::FPSCRRegister fpscr{};
  fpscr.csr = fpscr.GetCSR();
  fpscr.SetFlushMode(false);
  CHECK((fpscr.GetCSR() & rex::ppc::FPSCRRegister::kFlushMask) == 0);
  fpscr.SetFlushMode(true);
  CHECK((fpscr.GetCSR() & rex::ppc::FPSCRRegister::kFlushMask) ==
        rex::ppc::FPSCRRegister::kFlushMask);
  fpscr.SetFlushMode(false);
  CHECK((fpscr.GetCSR() & rex::ppc::FPSCRRegister::kFlushMask) == 0);
}

REX_TEST_CASE("SetFlushMode leaves the other host control bits alone", "[ppc][fpscr]") {
  const RestoreCSR restore;
  rex::ppc::FPSCRRegister fpscr{};
  fpscr.StoreFromGuest(rex::ppc::kRoundUp);
  const u32 rounding = fpscr.GetCSR() & rex::ppc::FPSCRRegister::kRoundMaskVal;
  fpscr.SetFlushMode(true);
  fpscr.SetFlushMode(false);
  CHECK((fpscr.GetCSR() & rex::ppc::FPSCRRegister::kRoundMaskVal) == rounding);
}

REX_TEST_CASE("SetFlushMode does not write the host word when the mode matches", "[ppc][fpscr]") {
  const RestoreCSR restore;
  rex::ppc::FPSCRRegister fpscr{};
  fpscr.csr = fpscr.GetCSR() & ~static_cast<u32>(rex::ppc::FPSCRRegister::kFlushMask);
  fpscr.SetCSR(fpscr.csr);
  const u32 stale = fpscr.csr ^ rex::ppc::FPSCRRegister::Platform::kExceptionMask;
  fpscr.csr = stale;
  fpscr.SetFlushMode(false);
  CHECK(fpscr.csr == stale);
}

REX_TEST_CASE("SetFlushMode does not write the host word when the mode is already on",
              "[ppc][fpscr]") {
  const RestoreCSR restore;
  rex::ppc::FPSCRRegister fpscr{};
  fpscr.csr = fpscr.GetCSR();
  fpscr.SetFlushMode(true);
  fpscr.csr = fpscr.GetCSR();
  const u32 stale = fpscr.csr ^ rex::ppc::FPSCRRegister::Platform::kExceptionMask;
  fpscr.csr = stale;
  fpscr.SetFlushMode(true);
  CHECK(fpscr.csr == stale);
}
