/**
 * @file        tests/unit/ppc/midasm_macro_test.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "rex_test.h"

#include <cstddef>
#include <type_traits>

#include <rex/memory/mapped_ptr.h>
#include <rex/ppc/context.h>
#include <rex/ppc/midasm.h>

namespace detail = rex::ppc::detail;

REX_MIDASM_HOOK(MidAsmTestScale, (PPCRegister & step)) {
  step.f64 *= 2.0;
}
REX_MIDASM_AT(MidAsmTestScale, 0x82000100, after, f10);
REX_MIDASM_AT(MidAsmTestScale, 0x82000200, before, f12);

REX_MIDASM_BRANCH(MidAsmTestSkip, (u32 size, u64 wide)) {
  return size == 0 && wide == 0;
}
REX_MIDASM_AT(MidAsmTestSkip, 0x82000300, before, 0x8200030C, r5, r31);
REX_MIDASM_AT(MidAsmTestSkip, 0x82000310, after, 0x82000320, r5, r31);

REX_MIDASM_RETURN_IF(MidAsmTestSkipAll, (u32 count)) {
  return count == 0;
}
REX_MIDASM_AT(MidAsmTestSkipAll, 0x82000400, before, r3);

REX_MIDASM_HOOK(MidAsmTestEveryType,
                (PPCContext & ctx, u32 a, u64 b, f64 c, PPCRegister& d, PPCVRegister e,
                 PPCVRegister& f, PPCCRRegister& g, PPCRegister& h, PPCXERRegister& i,
                 PPCFPSCRRegister& j, PPCRegister& k)) {
  ctx.r3.u64 = a + b;
  d.f64 = c;
  f = e;
  g.lt = 1;
  h.u64 = 7;
  i.ca = 1;
  j.csr = 0;
  k.u64 = 9;
}
REX_MIDASM_AT(MidAsmTestEveryType, 0x82000480, after, ctx, r3, r4, f1, r5, v1, v2, cr6, ctr, xer,
              fpscr, reserved);

REX_MIDASM_RETURN_IF(MidAsmTestNoRegisters, ()) {
  return true;
}
REX_MIDASM_AT(MidAsmTestNoRegisters, 0x82000600, before);

struct MidAsmTestWidget {
  u32 value;
};

REX_MIDASM_HOOK(MidAsmTestPointers, (MidAsmTestWidget * widget, const MidAsmTestWidget* other,
                                     rex::memory::MappedPtr<MidAsmTestWidget> mapped)) {
  widget->value = other->value + mapped->value + mapped.GetGuestAddress();
}
REX_MIDASM_AT(MidAsmTestPointers, 0x82000700, before, r27, r28, r29);

namespace test_ns {

REX_MIDASM_HOOK(MidAsmTestNamespaced, (PPCRegister & value)) {
  value.u64 = 42;
}
REX_MIDASM_AT(MidAsmTestNamespaced, 0x82000500, after, r7);

}  // namespace test_ns

extern "C" void MidAsmTestNamespaced(PPCRegister& value);

REX_TEST_CASE("midasm: a hook body runs on the register it is given", "[ppc][midasm]") {
  PPCRegister step{};
  step.f64 = 1.5;
  MidAsmTestScale(step);
  CHECK(step.f64 == 3.0);
}

REX_TEST_CASE("midasm: a branch body answers from its registers", "[ppc][midasm]") {
  CHECK(MidAsmTestSkip(0, 0));
  CHECK_FALSE(MidAsmTestSkip(1, 0));
  CHECK(MidAsmTestSkipAll(0));
  CHECK_FALSE(MidAsmTestSkipAll(5));
}

REX_TEST_CASE("midasm: a body with no parameters and no registers is attached", "[ppc][midasm]") {
  CHECK(MidAsmTestNoRegisters());
}

REX_TEST_CASE("midasm: a body takes every register type", "[ppc][midasm]") {
  PPCContext ctx{};
  PPCRegister d{};
  PPCRegister h{};
  PPCRegister k{};
  PPCVRegister source{};
  source.u8[3] = 5;
  PPCVRegister target{};
  PPCCRRegister cr{};
  PPCXERRegister xer{};
  PPCFPSCRRegister fpscr{};
  MidAsmTestEveryType(ctx, 2, 3, 4.5, d, source, target, cr, h, xer, fpscr, k);
  CHECK(ctx.r3.u64 == 5);
  CHECK(d.f64 == 4.5);
  CHECK(target.u8[3] == 5);
  CHECK(cr.lt == 1);
  CHECK(h.u64 == 7);
  CHECK(xer.ca == 1);
  CHECK(k.u64 == 9);
}

REX_TEST_CASE("midasm: a body inside a namespace links by its plain name", "[ppc][midasm]") {
  PPCRegister value{};
  MidAsmTestNamespaced(value);
  CHECK(value.u64 == 42);
}

REX_TEST_CASE("midasm: the descriptor of a body holds its type and parameters", "[ppc][midasm]") {
  STATIC_REQUIRE(MidAsmTestScale_rex_midasm.type == detail::MidAsmType::kHook);
  STATIC_REQUIRE(MidAsmTestScale_rex_midasm.count == 1);
  STATIC_REQUIRE(MidAsmTestScale_rex_midasm.params[0] == detail::MidAsmParam::kRegister);
  STATIC_REQUIRE(MidAsmTestSkip_rex_midasm.type == detail::MidAsmType::kBranch);
  STATIC_REQUIRE(MidAsmTestSkip_rex_midasm.count == 2);
  STATIC_REQUIRE(MidAsmTestSkip_rex_midasm.params[0] == detail::MidAsmParam::kU32);
  STATIC_REQUIRE(MidAsmTestSkip_rex_midasm.params[1] == detail::MidAsmParam::kU64);
  STATIC_REQUIRE(MidAsmTestSkipAll_rex_midasm.type == detail::MidAsmType::kReturnIf);
  STATIC_REQUIRE(MidAsmTestEveryType_rex_midasm.count == 12);
  STATIC_REQUIRE(MidAsmTestEveryType_rex_midasm.params[0] == detail::MidAsmParam::kContext);
  STATIC_REQUIRE(MidAsmTestEveryType_rex_midasm.params[5] == detail::MidAsmParam::kVector);
  STATIC_REQUIRE(MidAsmTestEveryType_rex_midasm.params[6] == detail::MidAsmParam::kVectorRef);
}

REX_TEST_CASE("midasm: a parameter type no register fills is invalid", "[ppc][midasm]") {
  STATIC_REQUIRE(detail::ClassifyMidAsmParam<i32>() == detail::MidAsmParam::kInvalid);
  STATIC_REQUIRE(detail::ClassifyMidAsmParam<f32>() == detail::MidAsmParam::kInvalid);
  STATIC_REQUIRE(detail::ClassifyMidAsmParam<PPCRegister>() == detail::MidAsmParam::kInvalid);
  STATIC_REQUIRE(detail::ClassifyMidAsmParam<const PPCRegister&>() ==
                 detail::MidAsmParam::kInvalid);
  STATIC_REQUIRE(detail::ClassifyMidAsmParam<u32&>() == detail::MidAsmParam::kInvalid);
  STATIC_REQUIRE(detail::ClassifyMidAsmParam<PPCRegister&>() == detail::MidAsmParam::kRegister);
}

REX_TEST_CASE("midasm: the register parser accepts exactly the registers the context has",
              "[ppc][midasm]") {
  STATIC_REQUIRE(std::is_object_v<decltype(PPCContext::r31)>);
  STATIC_REQUIRE(std::is_object_v<decltype(PPCContext::f31)>);
  STATIC_REQUIRE(std::is_object_v<decltype(PPCContext::v127)>);
  STATIC_REQUIRE(std::is_object_v<decltype(PPCContext::cr7)>);
  STATIC_REQUIRE(std::is_object_v<decltype(PPCContext::ctr)>);
  STATIC_REQUIRE(std::is_object_v<decltype(PPCContext::xer)>);
  STATIC_REQUIRE(std::is_object_v<decltype(PPCContext::fpscr)>);
  STATIC_REQUIRE(std::is_object_v<decltype(PPCContext::reserved)>);

  using Type = detail::MidAsmRegisterType;
  STATIC_REQUIRE(detail::ParseMidAsmRegister("r0") == Type::kGPR);
  STATIC_REQUIRE(detail::ParseMidAsmRegister("r31") == Type::kGPR);
  STATIC_REQUIRE(detail::ParseMidAsmRegister("r32") == Type::kInvalid);
  STATIC_REQUIRE(detail::ParseMidAsmRegister("r1") == Type::kInvalid);
  STATIC_REQUIRE(detail::ParseMidAsmRegister("r01") == Type::kInvalid);
  STATIC_REQUIRE(detail::ParseMidAsmRegister("f0") == Type::kFPR);
  STATIC_REQUIRE(detail::ParseMidAsmRegister("f31") == Type::kFPR);
  STATIC_REQUIRE(detail::ParseMidAsmRegister("f32") == Type::kInvalid);
  STATIC_REQUIRE(detail::ParseMidAsmRegister("v0") == Type::kVR);
  STATIC_REQUIRE(detail::ParseMidAsmRegister("v127") == Type::kVR);
  STATIC_REQUIRE(detail::ParseMidAsmRegister("v128") == Type::kInvalid);
  STATIC_REQUIRE(detail::ParseMidAsmRegister("cr0") == Type::kCR);
  STATIC_REQUIRE(detail::ParseMidAsmRegister("cr7") == Type::kCR);
  STATIC_REQUIRE(detail::ParseMidAsmRegister("cr8") == Type::kInvalid);
  STATIC_REQUIRE(detail::ParseMidAsmRegister("ctr") == Type::kCTR);
  STATIC_REQUIRE(detail::ParseMidAsmRegister("xer") == Type::kXER);
  STATIC_REQUIRE(detail::ParseMidAsmRegister("fpscr") == Type::kFPSCR);
  STATIC_REQUIRE(detail::ParseMidAsmRegister("reserved") == Type::kReserved);
  STATIC_REQUIRE(detail::ParseMidAsmRegister("ctx") == Type::kContext);
  STATIC_REQUIRE(detail::ParseMidAsmRegister("lr") == Type::kInvalid);
  STATIC_REQUIRE(detail::ParseMidAsmRegister("R3") == Type::kInvalid);
  STATIC_REQUIRE(detail::ParseMidAsmRegister("") == Type::kInvalid);
  STATIC_REQUIRE(detail::ParseMidAsmRegister("r") == Type::kInvalid);
  STATIC_REQUIRE(detail::ParseMidAsmRegister("r3x") == Type::kInvalid);
}

REX_TEST_CASE("midasm: the stringified inputs of every register type are checked",
              "[ppc][midasm]") {
  constexpr auto& kEvery = MidAsmTestEveryType_rex_midasm;
  using Result = detail::MidAsmAtResult;
  STATIC_REQUIRE(detail::CheckMidAsmAt(kEvery, "after",
                                       "ctx, r3, r4, f1, r5, v1, v2, cr6, ctr, xer, fpscr, "
                                       "reserved") == Result::kOk);
  STATIC_REQUIRE(detail::CheckMidAsmAt(kEvery, "before",
                                       "ctx, r3, r4, f1, r5, v1, v2, cr6, ctr, xer, fpscr, "
                                       "reserved") == Result::kOk);
  STATIC_REQUIRE(detail::CheckMidAsmAt(kEvery, "after",
                                       "r3, r3, r4, f1, r5, v1, v2, cr6, ctr, xer, fpscr, "
                                       "reserved") == Result::kTypeMismatch);
  STATIC_REQUIRE(detail::CheckMidAsmAt(kEvery, "after",
                                       "ctx, r3, r4, f1, r5, v1, v2, cr6, ctr, xer, fpscr, "
                                       "ctr") == Result::kOk);
  STATIC_REQUIRE(detail::CheckMidAsmAt(kEvery, "after",
                                       "ctx, r3, r4, f1, r5, v1, v2, cr6, ctr, fpscr, xer, "
                                       "reserved") == Result::kTypeMismatch);
}

REX_TEST_CASE("midasm: an argument list parses with any spacing", "[ppc][midasm]") {
  constexpr auto& kSkip = MidAsmTestSkip_rex_midasm;
  using Result = detail::MidAsmAtResult;
  STATIC_REQUIRE(detail::CheckMidAsmAt(kSkip, "before", "0x8200030C, r5, r31") == Result::kOk);
  STATIC_REQUIRE(detail::CheckMidAsmAt(kSkip, "before", "0x8200030C,r5,r31") == Result::kOk);
  STATIC_REQUIRE(detail::CheckMidAsmAt(kSkip, "before", "0x8200030C , r5 , r31") == Result::kOk);
  STATIC_REQUIRE(detail::CheckMidAsmAt(kSkip, "before", "  0x8200030C,\n\tr5 ,\r\n r31  ") ==
                 Result::kOk);
  STATIC_REQUIRE(detail::CheckMidAsmAt(kSkip, " after ", "0x8200030C,r5,r31") == Result::kOk);
  STATIC_REQUIRE(detail::CheckMidAsmAt(kSkip, "before", "0x8200030C,r5,") == Result::kBadRegister);
  STATIC_REQUIRE(detail::CheckMidAsmAt(kSkip, "before", "0x8200030C,,r31") == Result::kBadRegister);
}

REX_TEST_CASE("midasm: an attachment that does not fit its body is reported", "[ppc][midasm]") {
  constexpr auto& kScale = MidAsmTestScale_rex_midasm;
  constexpr auto& kSkip = MidAsmTestSkip_rex_midasm;
  using Result = detail::MidAsmAtResult;
  STATIC_REQUIRE(detail::CheckMidAsmAt(kScale, "during", "f10") == Result::kBadPlacement);
  STATIC_REQUIRE(detail::CheckMidAsmAt(kScale, "after", "0x8200010C, f10") ==
                 Result::kUnexpectedTarget);
  STATIC_REQUIRE(detail::CheckMidAsmAt(kSkip, "before", "r5, r31") == Result::kMissingTarget);
  STATIC_REQUIRE(detail::CheckMidAsmAt(kSkip, "before", "") == Result::kMissingTarget);
  STATIC_REQUIRE(detail::CheckMidAsmAt(kSkip, "before", "0x8200030E, r5, r31") ==
                 Result::kBadTarget);
  STATIC_REQUIRE(detail::CheckMidAsmAt(kSkip, "before", "0, r5, r31") == Result::kBadTarget);
  STATIC_REQUIRE(detail::CheckMidAsmAt(kScale, "after", "r1") == Result::kBadRegister);
  STATIC_REQUIRE(detail::CheckMidAsmAt(kScale, "after", "lr") == Result::kBadRegister);
  STATIC_REQUIRE(detail::CheckMidAsmAt(kScale, "after", "f10, f11") == Result::kCountMismatch);
  STATIC_REQUIRE(detail::CheckMidAsmAt(kScale, "after", "") == Result::kCountMismatch);
  STATIC_REQUIRE(detail::CheckMidAsmAt(kSkip, "before", "0x8200030C, r5, f1") ==
                 Result::kTypeMismatch);
  STATIC_REQUIRE(detail::CheckMidAsmAt(kScale, "after", "v1") == Result::kTypeMismatch);
}

REX_TEST_CASE("midasm: a body takes pointers and mapped pointers", "[ppc][midasm]") {
  MidAsmTestWidget widget{1};
  MidAsmTestWidget other{10};
  MidAsmTestWidget third{100};
  MidAsmTestPointers(&widget, &other,
                     rex::memory::MappedPtr<u8>(reinterpret_cast<u8*>(&third), 1000));
  CHECK(widget.value == 10 + 100 + 1000);
  MidAsmTestPointers_rex_body(&widget, &other, rex::memory::MappedPtr<MidAsmTestWidget>(&third, 7));
  CHECK(widget.value == 10 + 100 + 7);
  STATIC_REQUIRE(std::is_same_v<decltype(&MidAsmTestPointers),
                                void (*)(void*, void*, rex::memory::MappedPtr<u8>)>);
  STATIC_REQUIRE(MidAsmTestPointers_rex_midasm.count == 3);
  STATIC_REQUIRE(MidAsmTestPointers_rex_midasm.params[0] == detail::MidAsmParam::kPointer);
  STATIC_REQUIRE(MidAsmTestPointers_rex_midasm.params[1] == detail::MidAsmParam::kPointer);
  STATIC_REQUIRE(MidAsmTestPointers_rex_midasm.params[2] == detail::MidAsmParam::kMappedPointer);
}

REX_TEST_CASE("midasm: only integer registers fill a pointer", "[ppc][midasm]") {
  using detail::IsMidAsmRegisterFit;
  using detail::MidAsmParam;
  using detail::MidAsmRegisterType;
  for (const MidAsmParam param : {MidAsmParam::kPointer, MidAsmParam::kMappedPointer}) {
    CHECK(IsMidAsmRegisterFit(MidAsmRegisterType::kGPR, param));
    CHECK_FALSE(IsMidAsmRegisterFit(MidAsmRegisterType::kFPR, param));
    CHECK_FALSE(IsMidAsmRegisterFit(MidAsmRegisterType::kVR, param));
    CHECK_FALSE(IsMidAsmRegisterFit(MidAsmRegisterType::kCTR, param));
    CHECK_FALSE(IsMidAsmRegisterFit(MidAsmRegisterType::kContext, param));
  }
}

REX_TEST_CASE("midasm: pointer parameter types are classified", "[ppc][midasm]") {
  STATIC_REQUIRE(detail::ClassifyMidAsmParam<u32*>() == detail::MidAsmParam::kPointer);
  STATIC_REQUIRE(detail::ClassifyMidAsmParam<const u8*>() == detail::MidAsmParam::kPointer);
  STATIC_REQUIRE(detail::ClassifyMidAsmParam<void*>() == detail::MidAsmParam::kPointer);
  STATIC_REQUIRE(detail::ClassifyMidAsmParam<u32**>() == detail::MidAsmParam::kPointer);
  STATIC_REQUIRE(detail::ClassifyMidAsmParam<rex::memory::MappedPtr<u32>>() ==
                 detail::MidAsmParam::kMappedPointer);
  STATIC_REQUIRE(detail::ClassifyMidAsmParam<void (*)()>() == detail::MidAsmParam::kInvalid);
  STATIC_REQUIRE(detail::ClassifyMidAsmParam<u32*&>() == detail::MidAsmParam::kInvalid);
  STATIC_REQUIRE(detail::ClassifyMidAsmParam<const rex::memory::MappedPtr<u32>&>() ==
                 detail::MidAsmParam::kInvalid);
}
