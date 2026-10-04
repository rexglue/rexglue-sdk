/**
 * Tests for PPCFPSCRRegister host control-word handling.
 */

#include <cfloat>
#include <cstdint>

#include <catch2/catch_test_macros.hpp>

#include <rex/ppc/context.h>

using namespace rex::ppc;

namespace {

using Platform = PPCFPSCRRegister::Platform;

#if defined(__x86_64__) || defined(_M_X64)
constexpr uint32_t ArmTraps(uint32_t csr) {
  return csr & ~Platform::ExceptionMask;
}
constexpr bool TrapsMasked(uint32_t csr) {
  return (csr & Platform::ExceptionMask) == Platform::ExceptionMask;
}
#else
constexpr uint32_t ArmTraps(uint32_t csr) {
  return csr | Platform::ExceptionMask;
}
constexpr bool TrapsMasked(uint32_t csr) {
  return (csr & Platform::ExceptionMask) == 0;
}
#endif

struct HostCsrGuard {
  uint32_t saved = Platform::getcsr();
  ~HostCsrGuard() { Platform::setcsr(saved); }
};

double Div(double a, double b) {
  volatile double x = a, y = b;
  return x / y;
}

float Mul(float a, float b) {
  volatile float x = a, y = b;
  return x * y;
}

}  // namespace

TEST_CASE("fpscr: guest rounding modes round host arithmetic correctly", "[ppc][fpscr]") {
  HostCsrGuard guard;
  PPCContext ctx{};
  ctx.fpscr.InitHost();

  ctx.fpscr.storeFromGuest(kRoundNearest);
  const double rn_pos = Div(1.0, 3.0);
  const double rn_neg = Div(-1.0, 3.0);

  ctx.fpscr.storeFromGuest(kRoundUp);
  const double up_pos = Div(1.0, 3.0);
  const double up_neg = Div(-1.0, 3.0);

  ctx.fpscr.storeFromGuest(kRoundDown);
  const double dn_pos = Div(1.0, 3.0);
  const double dn_neg = Div(-1.0, 3.0);

  ctx.fpscr.storeFromGuest(kRoundTowardZero);
  const double tz_pos = Div(1.0, 3.0);
  const double tz_neg = Div(-1.0, 3.0);

  ctx.fpscr.storeFromGuest(kRoundNearest);

  CHECK(up_pos > dn_pos);
  CHECK(up_neg > dn_neg);
  CHECK(tz_pos == dn_pos);
  CHECK(tz_neg == up_neg);
  CHECK(rn_pos == dn_pos);
  CHECK(rn_neg == up_neg);
}

TEST_CASE("fpscr: flush mode toggles denormal flushing", "[ppc][fpscr]") {
  HostCsrGuard guard;
  PPCContext ctx{};
  ctx.fpscr.InitHost();

  const float tiny = FLT_MIN / 4.0f;

  ctx.fpscr.disableFlushModeUnconditional();
  CHECK(Mul(tiny, 1.0f) != 0.0f);

  ctx.fpscr.enableFlushModeUnconditional();
  CHECK(Mul(tiny, 1.0f) == 0.0f);
  CHECK(Mul(FLT_MIN, 0.5f) == 0.0f);

  ctx.fpscr.disableFlushMode();
  CHECK(Mul(tiny, 1.0f) != 0.0f);
}

TEST_CASE("fpscr: setcsr keeps host traps masked and preserves guest bits", "[ppc][fpscr]") {
  HostCsrGuard guard;
  PPCContext ctx{};
  ctx.fpscr.InitHost();
  ctx.fpscr.storeFromGuest(kRoundTowardZero);
  ctx.fpscr.enableFlushModeUnconditional();

  const uint32_t before = ctx.fpscr.getcsr();
  ctx.fpscr.setcsr(ArmTraps(before));
  const uint32_t after = ctx.fpscr.getcsr();

  CHECK(TrapsMasked(after));
  CHECK((after & PPCFPSCRRegister::GuestMask) == (before & PPCFPSCRRegister::GuestMask));
  CHECK(ctx.fpscr.loadFromHost() == kRoundTowardZero);

  CHECK(Div(0.0, 0.0) != Div(0.0, 0.0));
  CHECK(Div(1.0, 0.0) > DBL_MAX);
}

TEST_CASE("fpscr: storeFromGuest on a zeroed csr does not arm traps", "[ppc][fpscr]") {
  HostCsrGuard guard;
  PPCContext ctx{};
  ctx.fpscr.csr = ArmTraps(0);

  ctx.fpscr.storeFromGuest(kRoundUp);

  CHECK(TrapsMasked(ctx.fpscr.getcsr()));
  CHECK(Div(0.0, 0.0) != Div(0.0, 0.0));
}
