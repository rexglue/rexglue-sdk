/**
 * @file        ppc/midasm_entry.h
 * @brief       The macros that define the extern C entry of a mid asm body
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

/**
 * Counts the arguments, up to 32.
 *
 * @param ... The arguments.
 *
 * @code
 * REX_DETAIL_MIDASM_COUNT(a, b, c)
 * @endcode
 */
#define REX_DETAIL_MIDASM_COUNT(...)                                                             \
  REX_DETAIL_MIDASM_PICK(__VA_ARGS__ __VA_OPT__(, ) 32, 31, 30, 29, 28, 27, 26, 25, 24, 23, 22,  \
                         21, 20, 19, 18, 17, 16, 15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, \
                         1, 0)

/**
 * Picks the count out of the arguments of REX_DETAIL_MIDASM_COUNT.
 *
 * @param ... The 32 arguments, the count and the rest.
 *
 * @code
 * REX_DETAIL_MIDASM_PICK(1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21,
 *                        22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 7, 0)
 * @endcode
 */
#define REX_DETAIL_MIDASM_PICK(_1, _2, _3, _4, _5, _6, _7, _8, _9, _10, _11, _12, _13, _14, _15, \
                               _16, _17, _18, _19, _20, _21, _22, _23, _24, _25, _26, _27, _28,  \
                               _29, _30, _31, _32, count, ...)                                   \
  count

/**
 * Removes the parentheses around a parameter list.
 *
 * @param ... The parameters.
 *
 * @code
 * REX_DETAIL_MIDASM_UNPAREN (u32 a, u32 b)
 * @endcode
 */
#define REX_DETAIL_MIDASM_UNPAREN(...) __VA_ARGS__

/**
 * Pastes two tokens after expanding them.
 *
 * @param a The first token.
 * @param b The second token.
 *
 * @code
 * REX_DETAIL_MIDASM_CAT(REX_DETAIL_MIDASM_LIST_, 3)
 * @endcode
 */
#define REX_DETAIL_MIDASM_CAT(a, b) REX_DETAIL_MIDASM_CAT_IMPL(a, b)

/**
 * Pastes two tokens.
 *
 * @param a The first token.
 * @param b The second token.
 *
 * @code
 * REX_DETAIL_MIDASM_CAT_IMPL(rex_arg, 0)
 * @endcode
 */
#define REX_DETAIL_MIDASM_CAT_IMPL(a, b) a##b

/**
 * Declares the entry parameter of a body.
 *
 * @param body The body name.
 * @param index The parameter index.
 *
 * @code
 * REX_DETAIL_MIDASM_PARAM(ExampleStep_rex_body, 0)
 * @endcode
 */
#define REX_DETAIL_MIDASM_PARAM(body, index)                                           \
  rex::ppc::detail::MidAsmErasedT<rex::ppc::detail::MidAsmArg<decltype(&body), index>> \
      rex_arg##index

/**
 * Restores the entry argument of a body to its parameter type.
 *
 * @param body The body name.
 * @param index The parameter index.
 *
 * @code
 * REX_DETAIL_MIDASM_ARG(ExampleStep_rex_body, 0)
 * @endcode
 */
#define REX_DETAIL_MIDASM_ARG(body, index)                                                 \
  rex::ppc::detail::RestoreMidAsmArg<rex::ppc::detail::MidAsmArg<decltype(&body), index>>( \
      rex_arg##index)

/**
 * Lists no parameters.
 *
 * @param body The body name.
 * @param each The macro applied to the body and each parameter index.
 *
 * @code
 * REX_DETAIL_MIDASM_LIST_0(ExampleStep_rex_body, REX_DETAIL_MIDASM_PARAM)
 * @endcode
 */
#define REX_DETAIL_MIDASM_LIST_0(body, each)

/**
 * Lists 1 parameter, a comma separated application of a macro to each index.
 *
 * @param body The body name.
 * @param each The macro applied to the body and each parameter index.
 *
 * @code
 * REX_DETAIL_MIDASM_LIST_1(ExampleStep_rex_body, REX_DETAIL_MIDASM_PARAM)
 * @endcode
 */
#define REX_DETAIL_MIDASM_LIST_1(body, each) each(body, 0)

/**
 * Lists 2 parameters, a comma separated application of a macro to each index.
 *
 * @param body The body name.
 * @param each The macro applied to the body and each parameter index.
 *
 * @code
 * REX_DETAIL_MIDASM_LIST_2(ExampleStep_rex_body, REX_DETAIL_MIDASM_PARAM)
 * @endcode
 */
#define REX_DETAIL_MIDASM_LIST_2(body, each) REX_DETAIL_MIDASM_LIST_1(body, each), each(body, 1)

/**
 * Lists 3 parameters, a comma separated application of a macro to each index.
 *
 * @param body The body name.
 * @param each The macro applied to the body and each parameter index.
 *
 * @code
 * REX_DETAIL_MIDASM_LIST_3(ExampleStep_rex_body, REX_DETAIL_MIDASM_PARAM)
 * @endcode
 */
#define REX_DETAIL_MIDASM_LIST_3(body, each) REX_DETAIL_MIDASM_LIST_2(body, each), each(body, 2)

/**
 * Lists 4 parameters, a comma separated application of a macro to each index.
 *
 * @param body The body name.
 * @param each The macro applied to the body and each parameter index.
 *
 * @code
 * REX_DETAIL_MIDASM_LIST_4(ExampleStep_rex_body, REX_DETAIL_MIDASM_PARAM)
 * @endcode
 */
#define REX_DETAIL_MIDASM_LIST_4(body, each) REX_DETAIL_MIDASM_LIST_3(body, each), each(body, 3)

/**
 * Lists 5 parameters, a comma separated application of a macro to each index.
 *
 * @param body The body name.
 * @param each The macro applied to the body and each parameter index.
 *
 * @code
 * REX_DETAIL_MIDASM_LIST_5(ExampleStep_rex_body, REX_DETAIL_MIDASM_PARAM)
 * @endcode
 */
#define REX_DETAIL_MIDASM_LIST_5(body, each) REX_DETAIL_MIDASM_LIST_4(body, each), each(body, 4)

/**
 * Lists 6 parameters, a comma separated application of a macro to each index.
 *
 * @param body The body name.
 * @param each The macro applied to the body and each parameter index.
 *
 * @code
 * REX_DETAIL_MIDASM_LIST_6(ExampleStep_rex_body, REX_DETAIL_MIDASM_PARAM)
 * @endcode
 */
#define REX_DETAIL_MIDASM_LIST_6(body, each) REX_DETAIL_MIDASM_LIST_5(body, each), each(body, 5)

/**
 * Lists 7 parameters, a comma separated application of a macro to each index.
 *
 * @param body The body name.
 * @param each The macro applied to the body and each parameter index.
 *
 * @code
 * REX_DETAIL_MIDASM_LIST_7(ExampleStep_rex_body, REX_DETAIL_MIDASM_PARAM)
 * @endcode
 */
#define REX_DETAIL_MIDASM_LIST_7(body, each) REX_DETAIL_MIDASM_LIST_6(body, each), each(body, 6)

/**
 * Lists 8 parameters, a comma separated application of a macro to each index.
 *
 * @param body The body name.
 * @param each The macro applied to the body and each parameter index.
 *
 * @code
 * REX_DETAIL_MIDASM_LIST_8(ExampleStep_rex_body, REX_DETAIL_MIDASM_PARAM)
 * @endcode
 */
#define REX_DETAIL_MIDASM_LIST_8(body, each) REX_DETAIL_MIDASM_LIST_7(body, each), each(body, 7)

/**
 * Lists 9 parameters, a comma separated application of a macro to each index.
 *
 * @param body The body name.
 * @param each The macro applied to the body and each parameter index.
 *
 * @code
 * REX_DETAIL_MIDASM_LIST_9(ExampleStep_rex_body, REX_DETAIL_MIDASM_PARAM)
 * @endcode
 */
#define REX_DETAIL_MIDASM_LIST_9(body, each) REX_DETAIL_MIDASM_LIST_8(body, each), each(body, 8)

/**
 * Lists 10 parameters, a comma separated application of a macro to each index.
 *
 * @param body The body name.
 * @param each The macro applied to the body and each parameter index.
 *
 * @code
 * REX_DETAIL_MIDASM_LIST_10(ExampleStep_rex_body, REX_DETAIL_MIDASM_PARAM)
 * @endcode
 */
#define REX_DETAIL_MIDASM_LIST_10(body, each) REX_DETAIL_MIDASM_LIST_9(body, each), each(body, 9)

/**
 * Lists 11 parameters, a comma separated application of a macro to each index.
 *
 * @param body The body name.
 * @param each The macro applied to the body and each parameter index.
 *
 * @code
 * REX_DETAIL_MIDASM_LIST_11(ExampleStep_rex_body, REX_DETAIL_MIDASM_PARAM)
 * @endcode
 */
#define REX_DETAIL_MIDASM_LIST_11(body, each) REX_DETAIL_MIDASM_LIST_10(body, each), each(body, 10)

/**
 * Lists 12 parameters, a comma separated application of a macro to each index.
 *
 * @param body The body name.
 * @param each The macro applied to the body and each parameter index.
 *
 * @code
 * REX_DETAIL_MIDASM_LIST_12(ExampleStep_rex_body, REX_DETAIL_MIDASM_PARAM)
 * @endcode
 */
#define REX_DETAIL_MIDASM_LIST_12(body, each) REX_DETAIL_MIDASM_LIST_11(body, each), each(body, 11)

/**
 * Lists 13 parameters, a comma separated application of a macro to each index.
 *
 * @param body The body name.
 * @param each The macro applied to the body and each parameter index.
 *
 * @code
 * REX_DETAIL_MIDASM_LIST_13(ExampleStep_rex_body, REX_DETAIL_MIDASM_PARAM)
 * @endcode
 */
#define REX_DETAIL_MIDASM_LIST_13(body, each) REX_DETAIL_MIDASM_LIST_12(body, each), each(body, 12)

/**
 * Lists 14 parameters, a comma separated application of a macro to each index.
 *
 * @param body The body name.
 * @param each The macro applied to the body and each parameter index.
 *
 * @code
 * REX_DETAIL_MIDASM_LIST_14(ExampleStep_rex_body, REX_DETAIL_MIDASM_PARAM)
 * @endcode
 */
#define REX_DETAIL_MIDASM_LIST_14(body, each) REX_DETAIL_MIDASM_LIST_13(body, each), each(body, 13)

/**
 * Lists 15 parameters, a comma separated application of a macro to each index.
 *
 * @param body The body name.
 * @param each The macro applied to the body and each parameter index.
 *
 * @code
 * REX_DETAIL_MIDASM_LIST_15(ExampleStep_rex_body, REX_DETAIL_MIDASM_PARAM)
 * @endcode
 */
#define REX_DETAIL_MIDASM_LIST_15(body, each) REX_DETAIL_MIDASM_LIST_14(body, each), each(body, 14)

/**
 * Lists 16 parameters, a comma separated application of a macro to each index.
 *
 * @param body The body name.
 * @param each The macro applied to the body and each parameter index.
 *
 * @code
 * REX_DETAIL_MIDASM_LIST_16(ExampleStep_rex_body, REX_DETAIL_MIDASM_PARAM)
 * @endcode
 */
#define REX_DETAIL_MIDASM_LIST_16(body, each) REX_DETAIL_MIDASM_LIST_15(body, each), each(body, 15)

/**
 * Lists 17 parameters, a comma separated application of a macro to each index.
 *
 * @param body The body name.
 * @param each The macro applied to the body and each parameter index.
 *
 * @code
 * REX_DETAIL_MIDASM_LIST_17(ExampleStep_rex_body, REX_DETAIL_MIDASM_PARAM)
 * @endcode
 */
#define REX_DETAIL_MIDASM_LIST_17(body, each) REX_DETAIL_MIDASM_LIST_16(body, each), each(body, 16)

/**
 * Lists 18 parameters, a comma separated application of a macro to each index.
 *
 * @param body The body name.
 * @param each The macro applied to the body and each parameter index.
 *
 * @code
 * REX_DETAIL_MIDASM_LIST_18(ExampleStep_rex_body, REX_DETAIL_MIDASM_PARAM)
 * @endcode
 */
#define REX_DETAIL_MIDASM_LIST_18(body, each) REX_DETAIL_MIDASM_LIST_17(body, each), each(body, 17)

/**
 * Lists 19 parameters, a comma separated application of a macro to each index.
 *
 * @param body The body name.
 * @param each The macro applied to the body and each parameter index.
 *
 * @code
 * REX_DETAIL_MIDASM_LIST_19(ExampleStep_rex_body, REX_DETAIL_MIDASM_PARAM)
 * @endcode
 */
#define REX_DETAIL_MIDASM_LIST_19(body, each) REX_DETAIL_MIDASM_LIST_18(body, each), each(body, 18)

/**
 * Lists 20 parameters, a comma separated application of a macro to each index.
 *
 * @param body The body name.
 * @param each The macro applied to the body and each parameter index.
 *
 * @code
 * REX_DETAIL_MIDASM_LIST_20(ExampleStep_rex_body, REX_DETAIL_MIDASM_PARAM)
 * @endcode
 */
#define REX_DETAIL_MIDASM_LIST_20(body, each) REX_DETAIL_MIDASM_LIST_19(body, each), each(body, 19)

/**
 * Lists 21 parameters, a comma separated application of a macro to each index.
 *
 * @param body The body name.
 * @param each The macro applied to the body and each parameter index.
 *
 * @code
 * REX_DETAIL_MIDASM_LIST_21(ExampleStep_rex_body, REX_DETAIL_MIDASM_PARAM)
 * @endcode
 */
#define REX_DETAIL_MIDASM_LIST_21(body, each) REX_DETAIL_MIDASM_LIST_20(body, each), each(body, 20)

/**
 * Lists 22 parameters, a comma separated application of a macro to each index.
 *
 * @param body The body name.
 * @param each The macro applied to the body and each parameter index.
 *
 * @code
 * REX_DETAIL_MIDASM_LIST_22(ExampleStep_rex_body, REX_DETAIL_MIDASM_PARAM)
 * @endcode
 */
#define REX_DETAIL_MIDASM_LIST_22(body, each) REX_DETAIL_MIDASM_LIST_21(body, each), each(body, 21)

/**
 * Lists 23 parameters, a comma separated application of a macro to each index.
 *
 * @param body The body name.
 * @param each The macro applied to the body and each parameter index.
 *
 * @code
 * REX_DETAIL_MIDASM_LIST_23(ExampleStep_rex_body, REX_DETAIL_MIDASM_PARAM)
 * @endcode
 */
#define REX_DETAIL_MIDASM_LIST_23(body, each) REX_DETAIL_MIDASM_LIST_22(body, each), each(body, 22)

/**
 * Lists 24 parameters, a comma separated application of a macro to each index.
 *
 * @param body The body name.
 * @param each The macro applied to the body and each parameter index.
 *
 * @code
 * REX_DETAIL_MIDASM_LIST_24(ExampleStep_rex_body, REX_DETAIL_MIDASM_PARAM)
 * @endcode
 */
#define REX_DETAIL_MIDASM_LIST_24(body, each) REX_DETAIL_MIDASM_LIST_23(body, each), each(body, 23)

/**
 * Lists 25 parameters, a comma separated application of a macro to each index.
 *
 * @param body The body name.
 * @param each The macro applied to the body and each parameter index.
 *
 * @code
 * REX_DETAIL_MIDASM_LIST_25(ExampleStep_rex_body, REX_DETAIL_MIDASM_PARAM)
 * @endcode
 */
#define REX_DETAIL_MIDASM_LIST_25(body, each) REX_DETAIL_MIDASM_LIST_24(body, each), each(body, 24)

/**
 * Lists 26 parameters, a comma separated application of a macro to each index.
 *
 * @param body The body name.
 * @param each The macro applied to the body and each parameter index.
 *
 * @code
 * REX_DETAIL_MIDASM_LIST_26(ExampleStep_rex_body, REX_DETAIL_MIDASM_PARAM)
 * @endcode
 */
#define REX_DETAIL_MIDASM_LIST_26(body, each) REX_DETAIL_MIDASM_LIST_25(body, each), each(body, 25)

/**
 * Lists 27 parameters, a comma separated application of a macro to each index.
 *
 * @param body The body name.
 * @param each The macro applied to the body and each parameter index.
 *
 * @code
 * REX_DETAIL_MIDASM_LIST_27(ExampleStep_rex_body, REX_DETAIL_MIDASM_PARAM)
 * @endcode
 */
#define REX_DETAIL_MIDASM_LIST_27(body, each) REX_DETAIL_MIDASM_LIST_26(body, each), each(body, 26)

/**
 * Lists 28 parameters, a comma separated application of a macro to each index.
 *
 * @param body The body name.
 * @param each The macro applied to the body and each parameter index.
 *
 * @code
 * REX_DETAIL_MIDASM_LIST_28(ExampleStep_rex_body, REX_DETAIL_MIDASM_PARAM)
 * @endcode
 */
#define REX_DETAIL_MIDASM_LIST_28(body, each) REX_DETAIL_MIDASM_LIST_27(body, each), each(body, 27)

/**
 * Lists 29 parameters, a comma separated application of a macro to each index.
 *
 * @param body The body name.
 * @param each The macro applied to the body and each parameter index.
 *
 * @code
 * REX_DETAIL_MIDASM_LIST_29(ExampleStep_rex_body, REX_DETAIL_MIDASM_PARAM)
 * @endcode
 */
#define REX_DETAIL_MIDASM_LIST_29(body, each) REX_DETAIL_MIDASM_LIST_28(body, each), each(body, 28)

/**
 * Lists 30 parameters, a comma separated application of a macro to each index.
 *
 * @param body The body name.
 * @param each The macro applied to the body and each parameter index.
 *
 * @code
 * REX_DETAIL_MIDASM_LIST_30(ExampleStep_rex_body, REX_DETAIL_MIDASM_PARAM)
 * @endcode
 */
#define REX_DETAIL_MIDASM_LIST_30(body, each) REX_DETAIL_MIDASM_LIST_29(body, each), each(body, 29)

/**
 * Lists 31 parameters, a comma separated application of a macro to each index.
 *
 * @param body The body name.
 * @param each The macro applied to the body and each parameter index.
 *
 * @code
 * REX_DETAIL_MIDASM_LIST_31(ExampleStep_rex_body, REX_DETAIL_MIDASM_PARAM)
 * @endcode
 */
#define REX_DETAIL_MIDASM_LIST_31(body, each) REX_DETAIL_MIDASM_LIST_30(body, each), each(body, 30)

/**
 * Lists 32 parameters, a comma separated application of a macro to each index.
 *
 * @param body The body name.
 * @param each The macro applied to the body and each parameter index.
 *
 * @code
 * REX_DETAIL_MIDASM_LIST_32(ExampleStep_rex_body, REX_DETAIL_MIDASM_PARAM)
 * @endcode
 */
#define REX_DETAIL_MIDASM_LIST_32(body, each) REX_DETAIL_MIDASM_LIST_31(body, each), each(body, 31)

/**
 * Defines the extern C entry of a body. It takes the erased parameter types the generated code
 * declares and calls the body with the real ones.
 *
 * @param ret The return type.
 * @param name The body name, also its linked name.
 * @param params The parameter list in parentheses.
 *
 * @code
 * REX_DETAIL_MIDASM_ENTRY(void, ExampleStep, (PPCRegister& step))
 * @endcode
 */
// NOLINTBEGIN(bugprone-macro-parentheses): ret declares a function
#define REX_DETAIL_MIDASM_ENTRY(ret, name, params)                                           \
  extern "C" ret name(REX_DETAIL_MIDASM_CAT(                                                 \
      REX_DETAIL_MIDASM_LIST_, REX_DETAIL_MIDASM_COUNT(REX_DETAIL_MIDASM_UNPAREN params))(   \
      name##_rex_body, REX_DETAIL_MIDASM_PARAM)) {                                           \
    return name##_rex_body(REX_DETAIL_MIDASM_CAT(                                            \
        REX_DETAIL_MIDASM_LIST_, REX_DETAIL_MIDASM_COUNT(REX_DETAIL_MIDASM_UNPAREN params))( \
        name##_rex_body, REX_DETAIL_MIDASM_ARG));                                            \
  }
// NOLINTEND(bugprone-macro-parentheses)
